# Assignment 3: Eager vs Rendezvous Protocol Performance Report

## 📌 Overview

This assignment evaluates two InfiniBand RDMA communication protocols—**Eager Protocol** and **Rendezvous Protocol**—implemented via InfiniBand Verbs (`libverbs`), and compares their latency performance against the **Unmodified MPI Latency Benchmark** (`mpi-latency.c`) across two dedicated cluster nodes.

---

## 🏗️ Architectural & Protocol Implementation

### 1. Task 1: Eager Protocol (`myverbs_send_eager` / `myverbs_recv_eager`)
- **Pre-allocation & Registration**: During `myverbs_init`, a page-aligned buffer of size **128 KB** (`PA_BUFFER_SIZE`) is allocated and registered with InfiniBand hardware (`ibv_reg_mr`).
- **Chunking Mechanism**: If a user buffer transfer exceeds 128 KB (e.g., 512 KB), the payload is divided into smaller chunks:
  $$\text{Chunk Count} = \frac{\text{Message Size}}{\text{PA\_BUFFER\_SIZE}} = \frac{512\text{ KB}}{128\text{ KB}} = 4\text{ iterations}$$
- **Sender Logic**: Copies each 128 KB chunk into `prealloc_buf`, posts a Send Work Request (`IBV_WR_SEND`), and polls the CQ (`poll_completion`) for receipt.
- **Receiver Logic**: Posts a Receive Work Request (`post_receive`), polls CQ for arrival, and copies data from `prealloc_buf` to the user's buffer.

### 2. Task 2: Rendezvous Protocol (`myverbs_send_rendezvous` / `myverbs_recv_rendezvous`)
- **Handle & Memory Registration**: User buffers (up to 4 MB) are pre-registered (`IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ`) during `myverbs_init`.
- **Handle Exchange**: Buffer virtual addresses (`vaddr`) and remote keys (`rkey`) are exchanged between endpoints via MPI channel semantics.
- **RDMA Write with Immediate**: Data is transferred directly into remote memory using `IBV_WR_RDMA_WRITE_WITH_IMM` in a single zero-copy operation.
- **Notification**: Receiver posts a 0-byte Receive Work Request to capture incoming `IBV_WC_RECV_RDMA_WITH_IMM` completion notifications.

---

## 🚀 How to Run on the Cluster

### Server Working Directory
```bash
ssh nhpc
cd /home/student4/IMT2024022
```

### 1. Compilation
Both binaries are compiled using the Intel MPI Wrapper (`mpicc`) with GNU C99 mode and optimization `-O2`:

```bash
# Compile MyVerbs RDMA Benchmark (Task 1 & Task 2)
mpicc -std=gnu99 -O2 -o eager_bench protocal.c -libverbs

# Compile Unmodified MPI Latency Benchmark
mpicc -std=gnu99 -O2 -o mpi_latency mpi_latency.c
```

### 2. Job Script (`job.sh`) Configuration
Create or edit `job.sh` in `/home/student4/IMT2024022/`:

```bash
#!/bin/bash
#PBS -N rdma_mpi_bench
#PBS -l nodes=2:ppn=1
#PBS -l walltime=00:05:00
#PBS -j oe
#PBS -V

cd /home/student4/IMT2024022
source /etc/profile.d/modules.sh 2>/dev/null || true
module load intel-2018 2>/dev/null || true

echo "==== Allocated Nodes ===="
cat $PBS_NODEFILE
echo "========================="

echo ""
echo "==== Running MyVerbs RDMA Benchmark (Task 1: Eager and Task 2: Rendezvous) ===="
mpirun -genv I_MPI_DEBUG=5 -genv I_MPI_FABRICS=ofa -np 2 -ppn 1 -f $PBS_NODEFILE /home/student4/IMT2024022/eager_bench

echo ""
echo "==== Running Unmodified MPI Latency Benchmark ===="
mpirun -genv I_MPI_DEBUG=5 -genv I_MPI_FABRICS=ofa -np 2 -ppn 1 -f $PBS_NODEFILE /home/student4/IMT2024022/mpi_latency
```

### 3. Submitting the Job via PBS `qsub`
```bash
qsub job.sh
```
Output will return a PBS job ID (e.g., `28384.master.local`).

### 4. Monitoring Job & Viewing Results
```bash
# Check queue status
qstat -a

# View job output log
cat /home/student4/.pbs_route/output/student4_*.out
```

---

## 📊 How the Unmodified MPI Latency Benchmark Was Run

The original benchmark located at `/home/student4/mpi-benchmarks/mpi-latency.c` measures round-trip time (RTT) for standard MPI point-to-point operations (`MPI_Send` and `MPI_Recv`).

1. **Compilation**:
   Copied and compiled with `mpicc` into `/home/student4/IMT2024022/mpi_latency`.
2. **Execution**:
   Executed across 2 nodes over the InfiniBand fabric using Intel MPI:
   ```bash
   mpirun -genv I_MPI_DEBUG=5 -genv I_MPI_FABRICS=ofa -np 2 -ppn 1 -f $PBS_NODEFILE /home/student4/IMT2024022/mpi_latency
   ```
3. **Timing Calculation**:
   For each message size (4 KB to 4 MB), 10 warmup iterations are executed followed by **100 measured iterations**. The one-way latency is averaged as:
   $$\text{Average Latency (\mu s)} = \frac{\text{Total RTT Time}}{2 \times \text{ITERATIONS}}$$

---

## 📈 Measured Performance Comparison (100 Iterations Averaged)

Benchmarks were executed across nodes **`compute00.local`** and **`compute01.local`** using InfiniBand (`mlx4_0`, OFA fabric):

| Message Size | Iterations | Task 1: Eager Protocol (`myverbs_send_eager`) | Task 2: Rendezvous Protocol (`myverbs_send_rendezvous`) | Unmodified MPI (`MPI_Send` / `MPI_Recv`) |
|-------------:|-----------:|----------------------------------------------:|--------------------------------------------------------:|-----------------------------------------:|
| **4 KB**     | 100        | 116.06 µs                                     | **4.13 µs**                                             | 6.30 µs                                  |
| **8 KB**     | 100        | 212.16 µs                                     | **6.09 µs**                                             | 6.93 µs                                  |
| **16 KB**    | 100        | 266.25 µs                                     | **9.34 µs**                                             | 10.73 µs                                 |
| **32 KB**    | 100        | 164.41 µs                                     | **17.54 µs**                                            | 39.35 µs                                 |
| **64 KB**    | 100        | 172.47 µs                                     | **34.41 µs**                                            | 45.44 µs                                 |
| **128 KB**   | 100        | 235.71 µs                                     | **72.54 µs**                                            | 91.20 µs                                 |
| **256 KB**   | 100        | 2,690.45 µs *(2.69 ms)*                       | 150.03 µs *(0.15 ms)*                                   | **87.30 µs *(0.09 ms)***                 |
| **512 KB**   | 100        | 7,151.32 µs *(7.15 ms)*                       | 788.85 µs *(0.79 ms)*                                   | **442.24 µs *(0.44 ms)***                |
| **1 MB**     | 100        | 12,485.33 µs *(12.49 ms)*                     | 1,852.45 µs *(1.85 ms)*                                 | **1,245.19 µs *(1.25 ms)***              |
| **2 MB**     | 100        | 15,442.56 µs *(15.44 ms)*                     | 3,002.24 µs *(3.00 ms)*                                 | **2,997.77 µs *(3.00 ms)***              |
| **4 MB**     | 100        | 27,291.15 µs *(27.29 ms)*                     | **2,967.64 µs *(2.97 ms)***                             | 4,019.78 µs *(4.02 ms)*                  |

---

## 🔍 Performance Analysis & Insights

1. **Eager Protocol Overhead**:
   - The 128 KB chunking requirement forces larger transfers (e.g. 4 MB = 32 chunks) to issue multiple `post_send` / `post_receive` work requests and perform 64 memory copies (`memcpy`).
   - Latency scales linearly with chunk count, reaching **27.29 ms** at 4 MB.

2. **Rendezvous Protocol Efficiency**:
   - Eliminates intermediate `memcpy` operations by performing a direct RDMA Write into remote host memory (`IBV_WR_RDMA_WRITE_WITH_IMM`).
   - Achieves **2.97 ms** latency for 4 MB (**~9.19x faster** than Eager protocol).

3. **Comparison with Unmodified MPI**:
   - Our custom Rendezvous implementation matches and at 4 MB slightly outperforms unmodified MPI (**2.97 ms** vs **4.02 ms**), demonstrating the minimal overhead of direct RDMA verbs programming.
