# Assignment 3: Eager vs Rendezvous Protocol Performance Report (Median Latency & Pre-Posting)

## 📌 Overview

This assignment evaluates two InfiniBand RDMA communication protocols—**Eager Protocol** and **Rendezvous Protocol**—implemented via InfiniBand Verbs (`libverbs`), and compares their median latency performance against the **Unmodified MPI Latency Benchmark** (`mpi-latency.c`) across two dedicated cluster nodes for message sizes ranging from **1 B to 4 MB**.

To eliminate measurement noise and jitter caused by OS scheduling and transient network interrupts, all benchmark loops use **high-resolution nanosecond timers (`clock_gettime(CLOCK_MONOTONIC)`)** for individual iterations, sort the results via `qsort`, and report the **Median Latency** for each message size.

---

## 🏎️ Hardware Optimization: RNR (Receiver Not Ready) Elimination via RX Pre-Posting

In InfiniBand RDMA Channel Semantics (`IBV_WR_SEND` / `IBV_WR_RECV`) and RDMA Write with Immediate (`IBV_WR_RDMA_WRITE_WITH_IMM`), if a sender posts a send request (`post_send`) before the receiver's NIC has an active receive buffer posted (`post_receive`), the remote HCA triggers an **RNR (Receiver Not Ready) NACK**. The sender HCA is forced to back off and retry after a hardware timer (~10–20 µs), introducing artificial latency retries.

### Solution: Decoupled Pre-Posting Architecture

We split the receive pipeline into two distinct phases:
1. **`myverbs_prepost_recv_*`**: Posts the receive buffer (`post_receive`) to the Receive Queue (RQ) *in advance*.
2. **`myverbs_wait_recv_*`**: Polls the CQ (`poll_completion`) for packet arrival and handles memory copying if necessary.

```
[Rank 0]                                      [Rank 1]
   │                                             │
   ├─────── Pre-post RX Buffer ──────────────────┤
   │                                             │
   ├─────────── MPI_Barrier ─────────────────────┤
   │                                             │
   ├─ myverbs_send_* ─── Ping Packet ───────────►│ (NIC ready, zero RNR backoff)
   │                                             │
   │◄─── Pong Packet ─── myverbs_send_* ─────────┤
   │                                             │
   ├─ Pre-post RX for Next Ping ─────────────────┤
```

---

## 🏗️ Architectural & Protocol Implementation

### 1. Task 1: Eager Protocol (`myverbs_send_eager` / `myverbs_wait_recv_eager`)
- **Pre-allocation & Registration**: During `myverbs_init`, a page-aligned buffer of size **128 KB** (`PA_BUFFER_SIZE`) is allocated and registered with InfiniBand hardware (`ibv_reg_mr`).
- **Chunking Mechanism**: If a user buffer transfer exceeds 128 KB (e.g., 512 KB), the payload is divided into smaller chunks:
  $$\text{Chunk Count} = \left\lceil \frac{\text{Message Size}}{\text{PA\_BUFFER\_SIZE}} \right\rceil = \frac{512\text{ KB}}{128\text{ KB}} = 4\text{ iterations}$$
- **Sender Logic**: Copies each chunk into `prealloc_buf`, posts a Send Work Request (`IBV_WR_SEND`), and polls the CQ (`poll_completion`) for receipt.
- **Receiver Logic**: Pre-posts receive buffer (`myverbs_prepost_recv_eager`), polls CQ (`poll_completion`), and copies data to user buffer.

### 2. Task 2: Rendezvous Protocol (`myverbs_send_rendezvous` / `myverbs_wait_recv_rendezvous`)
- **Handle & Memory Registration**: User buffers (up to 4 MB) are pre-registered (`IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ`) during `myverbs_init`.
- **Handle Exchange**: Buffer virtual addresses (`vaddr`) and remote keys (`rkey`) are exchanged between endpoints via MPI channel semantics.
- **RDMA Write with Immediate**: Data is transferred directly into remote memory using `IBV_WR_RDMA_WRITE_WITH_IMM` in a single zero-copy operation.
- **Notification**: Receiver pre-posts a 0-byte Receive Work Request (`myverbs_prepost_recv_rendezvous`) to capture incoming `IBV_WC_RECV_RDMA_WITH_IMM` completion notifications without RNR retries.

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

echo '==== Allocated Nodes ===='
cat $PBS_NODEFILE
echo '========================='

echo ''
echo '==== Running MyVerbs RDMA Benchmark ===='
mpirun -genv I_MPI_DEBUG=5 -genv I_MPI_FABRICS=ofa -np 2 -ppn 1 -f $PBS_NODEFILE /home/student4/IMT2024022/eager_bench

echo ''
echo '==== Running Unmodified MPI Latency Benchmark ===='
mpirun -genv I_MPI_DEBUG=5 -genv I_MPI_FABRICS=ofa -np 2 -ppn 1 -f $PBS_NODEFILE /home/student4/IMT2024022/mpi_latency
```

### 3. Submitting the Job via PBS `qsub`
```bash
qsub job.sh
```

### 4. Viewing Results
```bash
cat /home/student4/.pbs_route/output/student4_*.out
```

---

## 📈 Complete Measured Performance Comparison (1 B to 4 MB, 100 Iterations Median)

Executed across nodes **`compute00.local`** and **`compute01.local`** over InfiniBand (`mlx4_0`, OFA fabric):

| Message Size | Iterations | Task 1: Eager Protocol (Median) | Task 2: Rendezvous Protocol (Median) | Unmodified MPI (`MPI_Send` / `MPI_Recv`) |
|-------------:|-----------:|--------------------------------:|------------------------------------:|-----------------------------------------:|
| **1 B**      | 100        | **2.50 µs**                     | **2.24 µs**                         | **1.67 µs**                              |
| **2 B**      | 100        | 2.52 µs                         | **2.34 µs**                         | **1.59 µs**                              |
| **4 B**      | 100        | 2.84 µs                         | **2.23 µs**                         | **1.53 µs**                              |
| **8 B**      | 100        | 2.74 µs                         | **2.04 µs**                         | **1.58 µs**                              |
| **16 B**     | 100        | 2.96 µs                         | **2.05 µs**                         | **1.53 µs**                              |
| **32 B**     | 100        | 2.82 µs                         | **2.03 µs**                         | **1.63 µs**                              |
| **64 B**     | 100        | 2.69 µs                         | **2.12 µs**                         | **2.21 µs**                              |
| **128 B**    | 100        | 2.72 µs                         | **2.22 µs**                         | **2.33 µs**                              |
| **256 B**    | 100        | 2.72 µs                         | **2.28 µs**                         | **2.42 µs**                              |
| **512 B**    | 100        | 2.80 µs                         | **2.47 µs**                         | **2.57 µs**                              |
| **1 KB**     | 100        | 3.13 µs                         | **2.98 µs**                         | **2.90 µs**                              |
| **2 KB**     | 100        | 5.21 µs                         | **4.29 µs**                         | **3.65 µs**                              |
| **4 KB**     | 100        | 6.37 µs                         | **5.12 µs**                         | **4.59 µs**                              |
| **8 KB**     | 100        | 7.99 µs                         | **6.30 µs**                         | **6.56 µs**                              |
| **16 KB**    | 100        | 14.19 µs                        | **12.02 µs**                        | **9.69 µs**                              |
| **32 KB**    | 100        | 23.09 µs                        | **18.25 µs**                        | **13.71 µs**                             |
| **64 KB**    | 100        | 39.95 µs                        | **35.16 µs**                        | **23.31 µs**                             |
| **128 KB**   | 100        | 94.22 µs                        | **86.06 µs**                        | **40.90 µs**                             |
| **256 KB**   | 100        | 2,631.00 µs *(2.63 ms)*         | **180.88 µs *(0.18 ms)***           | **77.68 µs *(0.08 ms)***                 |
| **512 KB**   | 100        | 6,683.33 µs *(6.68 ms)*         | **345.52 µs *(0.35 ms)***           | **245.86 µs *(0.25 ms)***                |
| **1 MB**     | 100        | 11,339.24 µs *(11.34 ms)*       | **727.46 µs *(0.73 ms)***           | **434.83 µs *(0.43 ms)***                |
| **2 MB**     | 100        | 22,700.77 µs *(22.70 ms)*       | **1,572.19 µs *(1.57 ms)***         | **763.52 µs *(0.76 ms)***                |
| **4 MB**     | 100        | 42,738.41 µs *(42.74 ms)*       | **3,165.20 µs *(3.17 ms)***         | **1,710.13 µs *(1.71 ms)***              |

---

## 🔍 Key Insights & Benefits of Median Benchmarking

1. **Jitter Elimination**:
   - Measuring individual ping-pong iterations with `clock_gettime(CLOCK_MONOTONIC)` and taking the median via `qsort` removes random OS kernel context switches and network interrupt spikes.
   - Small message latencies for both Eager and Rendezvous are smooth and consistent around **2.0 µs – 2.8 µs**.

2. **Protocol Scaling**:
   - **Eager Protocol**: Highly efficient for small messages ($\le 128\text{ KB}$), but scales poorly for large messages due to 128 KB chunking and intermediate `memcpy` operations (**42.74 ms** at 4 MB).
   - **Rendezvous Protocol**: Zero-copy RDMA Write (`IBV_WR_RDMA_WRITE_WITH_IMM`) achieves **3.17 ms** at 4 MB (**~13.5x faster** than Eager protocol).
