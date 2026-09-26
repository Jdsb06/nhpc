# Assignment 3: Eager vs Rendezvous Protocol Performance Report (On-The-Fly Memory Registration)

## 📌 Overview

This assignment evaluates two InfiniBand RDMA communication protocols—**Eager Protocol** and **Rendezvous Protocol**—implemented via InfiniBand Verbs (`libverbs`), and compares their median latency performance against the **Unmodified MPI Latency Benchmark** (`mpi-latency.c`) across two dedicated cluster nodes for message sizes ranging from **1 B to 4 MB**.

---

## 🏗️ Architectural & Protocol Implementation

### 1. Task 1: Eager Protocol (`myverbs_send_eager` / `myverbs_wait_recv_eager`)
- **Pre-allocation & Registration**: During `myverbs_init`, a page-aligned buffer of size **128 KB** (`PA_BUFFER_SIZE`) is allocated and registered **once** with InfiniBand hardware (`ibv_reg_mr`).
- **Chunking Mechanism**: If a user buffer transfer exceeds 128 KB (e.g., 512 KB), the payload is divided into smaller chunks:
  $$\text{Chunk Count} = \left\lceil \frac{\text{Message Size}}{\text{PA\_BUFFER\_SIZE}} \right\rceil = \frac{512\text{ KB}}{128\text{ KB}} = 4\text{ iterations}$$
- **Sender Logic**: Copies each chunk into `prealloc_buf`, posts a Send Work Request (`IBV_WR_SEND`), and polls the CQ (`poll_completion`) for receipt.
- **Receiver Logic**: Pre-posts receive buffer (`myverbs_prepost_recv_eager`), polls CQ (`poll_completion`), and copies data to user buffer.

### 2. Task 2: Rendezvous Protocol with On-The-Fly Registration (`myverbs_send_rendezvous` / `myverbs_wait_recv_rendezvous`)
- **On-The-Fly Memory Registration**: Unlike Eager mode, Rendezvous does **not** pre-register user data buffers. On every transfer:
  1. **Receiver**: Registers user buffer `buf` of size `size` on-the-fly (`ibv_reg_mr`).
  2. **Handle Exchange**: Receiver sends its buffer virtual address (`vaddr`) and remote key (`rkey`) to the sender via verbs channel semantics (`IBV_WR_SEND`).
  3. **Sender**: Registers user buffer `buf` of size `size` on-the-fly (`ibv_reg_mr`).
  4. **Zero-Copy RDMA Write**: Sender performs `IBV_WR_RDMA_WRITE_WITH_IMM` directly from its memory into the receiver's memory.
  5. **Deregistration**: Both endpoints deregister their memory regions (`ibv_dereg_mr`) after the transfer completes.

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

## 📈 Complete Measured Performance Comparison (1 B to 4 MB, Median Latency)

Executed across nodes **`compute00.local`** and **`compute01.local`** over InfiniBand (`mlx4_0`, OFA fabric):

| Message Size | Iterations | Task 1: Eager Protocol (4-Slot Ring) | Task 2: Rendezvous Protocol (On-The-Fly Reg) | Unmodified MPI (`MPI_Send` / `MPI_Recv`) |
|-------------:|-----------:|-------------------------------------:|---------------------------------------------:|-----------------------------------------:|
| **1 B**      | 100        | **2.59 µs**                          | 25.63 µs                                     | **1.73 µs**                              |
| **2 B**      | 100        | **2.59 µs**                          | 25.50 µs                                     | **1.71 µs**                              |
| **4 B**      | 100        | **2.58 µs**                          | 24.54 µs                                     | **1.65 µs**                              |
| **8 B**      | 100        | **2.58 µs**                          | 24.18 µs                                     | **1.62 µs**                              |
| **16 B**     | 100        | **2.58 µs**                          | 24.11 µs                                     | **1.64 µs**                              |
| **32 B**     | 100        | **2.60 µs**                          | 24.09 µs                                     | **1.74 µs**                              |
| **64 B**     | 100        | **2.69 µs**                          | 24.24 µs                                     | **2.21 µs**                              |
| **128 B**    | 100        | **2.73 µs**                          | 24.17 µs                                     | **2.32 µs**                              |
| **256 B**    | 100        | **2.81 µs**                          | 24.35 µs                                     | **2.41 µs**                              |
| **512 B**    | 100        | **2.97 µs**                          | 24.42 µs                                     | **2.60 µs**                              |
| **1 KB**     | 100        | **3.39 µs**                          | 24.72 µs                                     | **3.20 µs**                              |
| **2 KB**     | 100        | **4.10 µs**                          | 25.20 µs                                     | **3.78 µs**                              |
| **4 KB**     | 100        | **5.84 µs**                          | 26.35 µs                                     | **4.56 µs**                              |
| **8 KB**     | 100        | **10.55 µs**                         | 28.14 µs                                     | **6.39 µs**                              |
| **16 KB**    | 100        | **14.32 µs**                         | 31.15 µs                                     | **9.24 µs**                              |
| **32 KB**    | 100        | **23.86 µs**                         | 36.51 µs                                     | **13.52 µs**                             |
| **64 KB**    | 100        | **39.15 µs**                         | 46.64 µs                                     | **23.03 µs**                             |
| **128 KB**   | 100        | 85.09 µs                             | **64.68 µs**                                 | **42.58 µs**                             |
| **256 KB**   | 100        | 181.27 µs *(0.18 ms)*                | **103.80 µs *(0.10 ms)***                    | **73.48 µs *(0.07 ms)***                 |
| **512 KB**   | 100        | 304.74 µs *(0.30 ms)*                | **176.05 µs *(0.18 ms)***                    | **286.47 µs *(0.29 ms)***                |
| **1 MB**     | 100        | 529.99 µs *(0.53 ms)*                | **323.05 µs *(0.32 ms)***                    | **457.45 µs *(0.46 ms)***                |
| **2 MB**     | 100        | 1,146.90 µs *(1.15 ms)*              | **624.42 µs *(0.62 ms)***                    | **956.50 µs *(0.96 ms)***                |
| **4 MB**     | 100        | 1,868.14 µs *(1.87 ms)*              | **1,239.18 µs *(1.24 ms)***                  | **1,575.80 µs *(1.58 ms)***              |

---

## ⏱️ Diagnostic Probe: One-Way Rendezvous Latency Component Breakdown (Medians)

A high-resolution timing probe stored per-iteration timings in component arrays and computed medians via `compute_median`:

| Message Size | `ibv_reg_mr` Cost | Control Handshake | RDMA Write Transfer | `ibv_dereg_mr` Cost | **Probe Component Sum** | **Measured One-Way Latency (RTT/2)** |
|-------------:|------------------:|------------------:|--------------------:|--------------------:|------------------------:|-------------------------------------:|
| **1 B**      | 11.99 µs          | 0.27 µs           | 2.66 µs             | 9.54 µs             | **24.46 µs**            | **25.63 µs**                         |
| **64 B**     | 10.78 µs          | 0.29 µs           | 2.64 µs             | 9.18 µs             | **22.89 µs**            | **24.24 µs**                         |
| **4 KB**     | 10.78 µs          | 0.29 µs           | 4.69 µs             | 9.26 µs             | **25.01 µs**            | **26.35 µs**                         |
| **128 KB**   | 14.13 µs          | 0.93 µs           | 38.71 µs            | 10.39 µs            | **64.16 µs**            | **64.68 µs**                         |
| **1 MB**     | 34.68 µs          | 3.35 µs           | 270.00 µs           | 15.18 µs            | **323.21 µs**           | **323.05 µs**                        |
| **4 MB**     | 110.43 µs         | 4.11 µs           | 1,064.13 µs         | 37.26 µs            | **1,215.93 µs**         | **1,239.18 µs**                      |

### 🔬 Key Diagnostic Takeaways:
1. **Airtight Mathematical Consistency**: By computing `compute_median` across component arrays for each iteration, the sum of probe component medians matches the overall 1-way median latency (`RTT / 2`) across all sizes (e.g. **64.16 µs probe sum vs 64.68 µs total at 128 KB**, and **1,215.93 µs probe sum vs 1,239.18 µs total at 4 MB**).
2. **Outperforming Intel MPI from 512 KB Onward**: Zero-copy Rendezvous outperforms unmodified Intel MPI (`MPI_Send`/`MPI_Recv`) for all message sizes $\ge 512\text{ KB}$:
   - **512 KB**: Rendezvous (**176.05 µs**) beats MPI (**286.47 µs**) by **38.5%**.
   - **1 MB**: Rendezvous (**323.05 µs**) beats MPI (**457.45 µs**) by **29.4%**.
   - **2 MB**: Rendezvous (**624.42 µs**) beats MPI (**956.50 µs**) by **34.7%**.
   - **4 MB**: Rendezvous (**1,239.18 µs**) beats MPI (**1,575.80 µs**) by **21.4%**.
   Bypassing internal MPI communicator checks, matching queue searches, and intermediate data buffering allows direct RDMA Write to achieve superior throughput.
3. **Eager Ring Buffer Scaling**: The 4-slot ring buffer (`EAGER_SLOTS 4`) completely eliminated the multi-chunk RNR backoff cliff at 256 KB. Eager latency now scales smoothly from **85.09 µs** (128 KB) up to **1.87 ms** (4 MB).

---

## 🔍 Trade-Off Analysis: Eager vs Rendezvous Protocol

1. **Small Message Trade-off (1 B to 16 KB)**:
   - **Eager Protocol**: Performs memory registration **once** during initialization. Transfer costs zero `ibv_reg_mr` overhead per message, achieving **~2.58 µs** latency.
   - **Rendezvous Protocol**: Registers and deregisters user memory on-the-fly (`ibv_reg_mr` + `ibv_dereg_mr`) for every single transfer. Operating system page pinning and kernel bookkeeping add a fixed **~20 µs - 25 µs** registration overhead per message. Per assignment specifications, doing fresh on-the-fly registration per transfer without caching MR handles is a pedagogically honest implementation of true zero-copy Rendezvous. This explicitly demonstrates why real-world MPI implementations switch from Eager to Rendezvous only at larger message sizes!

2. **Large Message Trade-off (256 KB to 4 MB)**:
   - **Eager Protocol**: Payload is split into 128 KB chunks (32 sequential chunk transfers at 4 MB). The 4-slot ring buffer prevents hardware DMA stalls, allowing latency to scale smoothly to **1.87 ms** at 4 MB.
   - **Rendezvous Protocol**: Despite the on-the-fly `ibv_reg_mr` overhead (~20 µs - 110 µs), zero-copy RDMA Write (`IBV_WR_RDMA_WRITE_WITH_IMM`) bypasses all CPU `memcpy` and chunking overhead, reducing 4 MB latency to **1.24 ms** (**~1.5x faster** than Eager protocol and **~1.27x faster** than unmodified Intel MPI!).

3. **Portability & Spec Compliance Notes**:
   - **Network Byte Order**: Immediate data (`imm_data`) is converted using `htonl` and `ntohl` per InfiniBand architecture spec for strict cross-endian portability.
   - **Addressing Model**: Queue pairs use LID-based native InfiniBand addressing (`is_global = 0`). For RoCE networks, `is_global = 1` with explicit GID routing can be configured.
