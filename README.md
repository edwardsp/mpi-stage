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
make
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

## Example: CycleCloud Workspace for Slurm

This example demonstrates using `mpi-stage` with [Azure CycleCloud Workspace for Slurm](https://github.com/Azure/ai-infrastructure-on-azure/tree/main/infrastructure_references/azure_cyclecloud_workspace_for_slurm) to distribute a 16 GB container image across 156 GPU nodes.  The image is copied from an Azure NetApp file share to each compute node's local NVMe storage.

### Slurm Job Script

```bash
#!/bin/bash
#SBATCH --job-name=sync-image
#SBATCH --output=sync-image_%j.out
#SBATCH --time=02:00:00
#SBATCH --partition=gpu
#SBATCH --ntasks-per-node=1
#SBATCH --mem=0
#SBATCH --exclusive

source /etc/profile
module load mpi/hpcx

srun --mpi=pmix \
     ./mpi_stage \
     --source /shared/home/hpcadmin/images/nvidia+nemo+25.09.00.sqsh \
     --dest /nvme/images/nvidia+nemo+25.09.00.sqsh \
     --pre-validate size \
     --post-validate size \
     --verbose \
     --source-rank 0
```

### Submit the Job

```bash
# Check available nodes
sinfo

# Submit to 156 nodes
sbatch -N 156 sync_image.slurm
```

### Sample Output

```
Loading mpi/hpcx
  Loading requirement:
    /opt/hpcx-v2.24.1-gcc-doca_ofed-ubuntu24.04-cuda13-aarch64/modulefiles/hpcx
[0] Topology check done: 0.0316861 s
[0] Using source rank 0
[0] Copy required: yes
[0] Starting copy phase (source writes: yes)
[0] Copy complete, Bandwidth: 3007.14 MB/s
[0] Post-validation complete
[0] Timings (s):
  Topology check:    5.22463
  Source metadata:   0.00803746
  Pre-validation:    0.0046786
  Copy phase:        5.21189
  Post-validation:   2.2944e-05
  Total time:        5.2563
```

In this example, a large container image (~16 GB) was distributed to 156 nodes at **~3 GB/s** in just over 5 seconds.

---

## Notes

- Only **one MPI rank per node** should attempt to write at a time. MPI-stage will abort if multiple ranks per node are detected.  
- The tool is optimized for large files (10s–100s of GBs) using double-buffered chunked transfers.  
- For large clusters with fast interconnects (e.g., InfiniBand), it uses MPI broadcasts to efficiently distribute file data.  

---

## License

MIT License – see `LICENSE`  
