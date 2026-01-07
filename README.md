# mpi-stage

**High-performance distributed file staging using MPI**  

`mpi-stage` is a lightweight MPI-based tool to efficiently copy large files from a source node or shared storage to local node storage (e.g., NVMe) across a cluster. It supports pre/post-copy validation, progress reporting, and bandwidth measurement.

---

## Features

- Parallel file copy across multiple nodes using MPI  
- Optional pre-copy validation (size or checksum) to skip unnecessary copies  
- Optional post-copy validation (size or checksum) to ensure integrity  
- Verbose mode with progress and bandwidth reporting  
- Handles large files efficiently with chunked double-buffered transfers  
- Avoids multiple ranks on the same node writing simultaneously  
- Allows specifying source rank manually if file exists on multiple nodes  

---

## Installation

```bash
git clone https://github.com/edwardsp/mpi-stage.git
cd mpi-stage
mpicxx -O3 -std=c++17 mpi_stage.cpp -o mpi-stage
```

---

## Usage

```bash
mpirun -np <num_nodes> --hostfile <hosts> --map-by ppr:1:node ./mpi-stage \
    --source <source_path> \
    --dest <dest_path> \
    [--overwrite] \
    [--pre-validate size|checksum] \
    [--post-validate size|checksum] \
    [--verbose] \
    [--source-rank <rank>]
```

### Example

Copy a large squashfs image from a source node to all nodes’ NVMe:

```bash
mpirun -np 16 --hostfile hosts --map-by ppr:1:node ./mpi-stage \
    --source /shared/images/nvidia.sqsh \
    --dest /nvme/images/nvidia.sqsh \
    --pre-validate size \
    --post-validate checksum \
    --verbose
```

---

## Command-line Options

| Option | Description |
|--------|-------------|
| `--source <path>` | Path to the source file |
| `--dest <path>` | Destination path on each node |
| `--overwrite` | Always overwrite destination even if it exists |
| `--pre-validate size|checksum` | Skip copy if destination matches source (default: `size`) |
| `--post-validate size|checksum` | Validate file after copy (default: none) |
| `--verbose` | Print progress, bandwidth, and timing |
| `--source-rank <rank>` | Manually specify the rank that has the source file |

---

## Notes

- Only **one MPI rank per node** should attempt to write at a time. MPI-stage will abort if multiple ranks per node are detected.  
- The tool is optimized for large files (10s–100s of GBs) using double-buffered chunked transfers.  
- For large clusters with fast interconnects (e.g., InfiniBand), it uses MPI broadcasts to efficiently distribute file data.  

---

## License

MIT License – see `LICENSE`  
