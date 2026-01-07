#include <mpi.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include <cstring>
#include <string>
#include <vector>
#include <iostream>
#include <cstdint>
#include <chrono>

/* ===========================
   Safe 64-bit hash (byte-wise)
   =========================== */
struct FastHash64 {
    uint64_t state = 0x27D4EB2F165667C5ULL;
    static constexpr uint64_t PRIME = 0x9E3779B185EBCA87ULL;

    void update(const void* data, size_t len) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < len; ++i) {
            state ^= p[i];
            state *= PRIME;
            state = (state << 27) | (state >> 37);
        }
    }
    uint64_t digest() const { return state; }
};

/* ===========================
   Utilities
   =========================== */
struct FileInfo {
    bool exists = false;
    uint64_t size = 0;
};

FileInfo stat_file(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0)
        return {true, static_cast<uint64_t>(st.st_size)};
    return {false, 0};
}

uint64_t hash_file(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return 0;
    FastHash64 h;
    constexpr size_t HASH_CHUNK = 4 << 20; // 4MB
    std::vector<char> buf(HASH_CHUNK);
    ssize_t n;
    while ((n = read(fd, buf.data(), buf.size())) > 0)
        h.update(buf.data(), n);
    close(fd);
    return h.digest();
}

[[noreturn]] void die(const std::string& msg, MPI_Comm comm) {
    int rank; MPI_Comm_rank(comm, &rank);
    if (rank == 0)
        std::cerr << "ERROR: " << msg << std::endl;
    MPI_Abort(comm, 1);
    std::exit(1);
}

/* ===========================
   Double buffer helper
   =========================== */
struct DoubleBuffer {
    std::vector<char> buf[2];
    int cur = 0, next = 1;
    void swap() { std::swap(cur, next); }
    DoubleBuffer(size_t sz) {
        buf[0].resize(sz);
        buf[1].resize(sz);
    }
};

/* ===========================
   Timing helpers
   =========================== */
using my_clock = std::chrono::steady_clock;
double seconds_since(my_clock::time_point t0) {
    return std::chrono::duration<double>(my_clock::now() - t0).count();
}

/* ===========================
   Main
   =========================== */
int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);
    MPI_Comm world = MPI_COMM_WORLD;
    int rank, world_size;
    MPI_Comm_rank(world, &rank);
    MPI_Comm_size(world, &world_size);

    std::string src, dst;
    bool overwrite = false;
    bool verbose = false;
    int source_rank_opt = -1;
    enum ValMode { NONE, SIZE, CHECKSUM };
    ValMode pre_val = SIZE, post_val = NONE;

    // --- Parse CLI ---
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--source") src = argv[++i];
        else if (a == "--dest") dst = argv[++i];
        else if (a == "--overwrite") overwrite = true;
        else if (a == "--pre-validate") {
            std::string v = argv[++i];
            pre_val = (v == "checksum") ? CHECKSUM : SIZE;
        } else if (a == "--post-validate") {
            std::string v = argv[++i];
            if (v == "checksum") post_val = CHECKSUM;
            else if (v == "size") post_val = SIZE;
        } else if (a == "--verbose") verbose = true;
        else if (a == "--source-rank") source_rank_opt = std::stoi(argv[++i]);
    }
    if (src.empty() || dst.empty())
        die("Missing --source or --dest", world);

    auto t_start = my_clock::now();

    // --- 1 rank per node check ---
    MPI_Comm node_comm;
    MPI_Comm_split_type(world, MPI_COMM_TYPE_SHARED, 0, MPI_INFO_NULL, &node_comm);
    int local_size; MPI_Comm_size(node_comm, &local_size);
    MPI_Comm_free(&node_comm);
    if (local_size != 1)
        die("More than one MPI rank on a node", world);
    auto t_topo = my_clock::now();

    if (verbose && rank == 0)
        std::cerr << "[0] Topology check done: " << seconds_since(t_start) << " s\n";

    // --- Determine source rank ---
    FileInfo src_info = stat_file(src);
    int has_src = src_info.exists ? 1 : 0;
    int global_has_src;
    MPI_Allreduce(&has_src, &global_has_src, 1, MPI_INT, MPI_SUM, world);
    if (global_has_src == 0)
        die("Source file not found on any node", world);

    int source_rank = -1;
    if (source_rank_opt >= 0) {
        source_rank = source_rank_opt;
        if (rank == 0 && verbose)
            std::cerr << "[0] Using source rank " << source_rank << "\n";
        if (!src_info.exists && rank == source_rank)
            die("Selected source rank does not have the file", world);
    } else {
        int candidate = src_info.exists ? rank : world_size;
        MPI_Allreduce(&candidate, &source_rank, 1, MPI_INT, MPI_MIN, world);
        if (rank == 0 && verbose)
            std::cerr << "[0] Auto-selected source rank " << source_rank << "\n";
    }
    auto t_source = my_clock::now();

    // --- Canonical metadata ---
    uint64_t canonical_size = 0, canonical_hash = 0;
    if (rank == source_rank) canonical_size = src_info.size;
    MPI_Bcast(&canonical_size, 1, MPI_UINT64_T, source_rank, world);

    if (pre_val == CHECKSUM || post_val == CHECKSUM) {
        if (rank == source_rank) canonical_hash = hash_file(src);
        MPI_Bcast(&canonical_hash, 1, MPI_UINT64_T, source_rank, world);
        if (verbose && rank == 0)
            std::cerr << "[0] Source checksum computed: " << canonical_hash << "\n";
    }
    auto t_meta = my_clock::now();

    // --- Pre-validation ---
    FileInfo dst_info = stat_file(dst);
    bool local_need_copy = true;
    if (dst_info.exists && !overwrite) {
        if (pre_val == SIZE)
            local_need_copy = (dst_info.size != canonical_size);
        else if (pre_val == CHECKSUM)
            local_need_copy = (dst_info.size != canonical_size || hash_file(dst) != canonical_hash);
    }

    // --- Global need_copy ---
    int global_need_copy_int = local_need_copy ? 1 : 0;
    int global_need_copy_flag = 0;
    MPI_Allreduce(&global_need_copy_int, &global_need_copy_flag, 1, MPI_INT, MPI_LOR, world);
    bool need_copy = global_need_copy_flag != 0;

    if (rank == 0 && verbose) {
        std::cerr << "[0] Copy required: " << (need_copy ? "yes" : "no") << "\n";
    }

    auto t_pre = my_clock::now();

    // --- Copy phase ---
    if (need_copy) {
        int color = 1; // all ranks participate
        MPI_Comm copy_comm;
        MPI_Comm_split(world, color, rank, &copy_comm);

        bool source_writes = src != dst;

        if (verbose && rank == 0)
            std::cerr << "[0] Starting copy phase (source writes: "
                      << (source_writes ? "yes" : "no") << ")\n";

        constexpr size_t CHUNK = 16 << 20;
        DoubleBuffer dbuf(CHUNK);
        ssize_t bytes[2] = {0,0};
        int fd_in = -1, fd_out = -1;
        if (rank == source_rank) {
            fd_in = open(src.c_str(), O_RDONLY);
            if (fd_in < 0) die("Failed to open source", copy_comm);
            if (!source_writes) fd_out = -1;
        }
        if (rank != source_rank || source_writes) {
            fd_out = open(dst.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
            if (fd_out < 0) die("Failed to open dest", copy_comm);
        }

        FastHash64 dst_hash;
        uint64_t offset = 0;

        // --- Prime first chunk ---
        if (rank == source_rank)
            bytes[dbuf.cur] = read(fd_in, dbuf.buf[dbuf.cur].data(), CHUNK);
        MPI_Bcast(&bytes[dbuf.cur], 1, MPI_LONG, source_rank, copy_comm);

        // --- Copy loop ---
        while (bytes[dbuf.cur] > 0) {
            MPI_Request req;
            MPI_Ibcast(dbuf.buf[dbuf.cur].data(), bytes[dbuf.cur], MPI_BYTE,
                       source_rank, copy_comm, &req);

            if (rank == source_rank)
                bytes[dbuf.next] = read(fd_in, dbuf.buf[dbuf.next].data(), CHUNK);

            MPI_Wait(&req, MPI_STATUS_IGNORE);

            if (rank != source_rank || source_writes) {
                ssize_t w = pwrite(fd_out, dbuf.buf[dbuf.cur].data(), bytes[dbuf.cur], offset);
                if (w != bytes[dbuf.cur])
                    die("Write failed", copy_comm);
                if (post_val == CHECKSUM)
                    dst_hash.update(dbuf.buf[dbuf.cur].data(), bytes[dbuf.cur]);
            }

            offset += bytes[dbuf.cur];

            if (verbose && rank == 0) {
                double bw = (offset / 1024.0 / 1024.0) / seconds_since(t_pre);
                std::cerr << "\r[0] Progress: " << (offset*100)/canonical_size
                          << "%, Bandwidth: " << bw << " MB/s" << std::flush;
            }

            MPI_Bcast(&bytes[dbuf.next], 1, MPI_LONG, source_rank, copy_comm);
            dbuf.swap();
        }

        if (fd_in >= 0) close(fd_in);
        if (fd_out >= 0) close(fd_out);
        MPI_Comm_free(&copy_comm);

        if (verbose && rank == 0) {
            std::cerr << "\r[0] Copy complete, Bandwidth: "
                      << (canonical_size / 1024.0 / 1024.0) / seconds_since(t_pre)
                      << " MB/s\n";
        }
    } else {
        // dummy broadcast to keep ranks in sync
        char dummy = 0;
        MPI_Bcast(&dummy, 1, MPI_BYTE, source_rank, world);
        if (verbose && rank == 0)
            std::cerr << "[0] No copy needed, all ranks synchronized\n";
    }
    auto t_copy = my_clock::now();

    // --- Post-validation ---
    if (post_val != NONE && need_copy) {
        if (post_val == SIZE) {
            FileInfo fi = stat_file(dst);
            if (fi.size != canonical_size)
                die("Post-validation size mismatch", world);
        } else if (post_val == CHECKSUM) {
            FastHash64 h;
            uint64_t hval = hash_file(dst);
            if (hval != canonical_hash)
                die("Post-validation checksum mismatch", world);
        }
        if (verbose && rank == 0)
            std::cerr << "[0] Post-validation complete\n";
    }
    auto t_post = my_clock::now();

    if (verbose && rank == 0) {
        std::cerr << "[0] Timings (s):\n";
        std::cerr << "  Topology check:    " << seconds_since(t_topo) << "\n";
        std::cerr << "  Source metadata:   " << seconds_since(t_source) - seconds_since(t_topo) << "\n";
        std::cerr << "  Pre-validation:    " << seconds_since(t_pre) - seconds_since(t_source) << "\n";
        std::cerr << "  Copy phase:        " << seconds_since(t_copy) - seconds_since(t_pre) << "\n";
        std::cerr << "  Post-validation:   " << seconds_since(t_post) - seconds_since(t_copy) << "\n";
        std::cerr << "  Total time:        " << seconds_since(t_start) << "\n";
    }

    MPI_Finalize();
    return 0;
}

