# Comparative Performance Report: TCP Sockets vs. RDMA Verbs

**Author:** IMT2024022  
**Course:** Network & High-Performance Computing (NHPC)  
**Location:** `/home/student4/IMT2024022`  

---

## 1. Objective

The objective of this laboratory report is to empirically evaluate and compare the round-trip latency performance of the traditional, OS-managed TCP network stack (Assignment 1) against kernel-bypassing RDMA hardware primitives using Infiniband Channel and Memory Semantics (Assignment 2).

---

## 2. Benchmark Data Table

Below is the merged benchmark dataset measured across cluster compute nodes (`compute00.local` and `compute01.local`). All latency values are converted to **microseconds (µs)** (RDMA raw nanosecond values divided by 1000).

| Message Size (Bytes) | TCP Sockets (A1) Latency (µs) | RDMA Channel Semantics (A2 Task 1) (µs) | RDMA Memory Semantics (A2 Task 2) (µs) |
| :------------------- | :---------------------------- | :------------------------------------- | :------------------------------------- |
| 1                    | 91.89                         | 2.400                                  | 3.656                                  |
| 2                    | 68.55                         | 2.150                                  | 3.394                                  |
| 4                    | 68.23                         | 2.065                                  | 2.779                                  |
| 8                    | 68.91                         | 2.064                                  | 2.621                                  |
| 16                   | 68.17                         | 2.060                                  | 2.146                                  |
| 32                   | 70.57                         | 2.099                                  | 2.255                                  |
| 64                   | 69.98                         | 2.073                                  | 1.936                                  |
| 128                  | 73.08                         | 2.249                                  | 2.547                                  |
| 256                  | 77.43                         | 2.243                                  | 2.817                                  |
| 512                  | 96.59                         | 2.576                                  | 2.361                                  |
| 1024                 | 161.30                        | 2.785                                  | 2.475                                  |
| 2048                 | 188.45                        | 3.779                                  | 7.733                                  |
| 4096                 | 253.21                        | 4.736                                  | 3.924                                  |
| 8192                 | 469.26                        | 4.586                                  | 5.003                                  |
| 16384                | 528.31                        | 5.831                                  | 7.066                                  |
| 32768                | 857.39                        | 8.179                                  | 9.481                                  |
| 65536                | 1576.38                       | 13.045                                 | 14.803                                 |
| 131072               | 2648.32                       | 25.936                                 | 28.236                                 |
| 262144               | 4650.06                       | 46.208                                 | 58.881                                 |
| 524288               | 9254.04                       | 88.562                                 | 112.956                                |
| 1048576              | 18086.93                      | 174.135                                | 224.285                                |
| 2097152              | 35941.96                      | 354.221                                | 462.490                                |
| 4194304              | 71549.95                      | 714.948                                | 825.518                                |

---

## 3. Technical Analysis & Conclusion

### 1. TCP Sockets High Overhead (Assignment 1)
TCP sockets suffer from significantly higher baseline latencies (~68 µs – 91 µs for small payloads, scaling up to ~71.55 ms for 4 MB messages). This performance gap is attributed to:
* **OS Kernel Overhead & Context Switching:** Every socket system call (`send`, `recv`, `read`) requires context switching between user mode and kernel mode, consuming CPU clock cycles and incurring scheduler latency.
* **Socket Buffer Memory Copying:** TCP requires multiple data copies: moving data from application space to the kernel socket buffer (SKB), and subsequently to the network interface controller (NIC) ring buffers.
* **Receiver CPU Processing:** The remote CPU must handle software interrupts, process TCP protocol ACK packet generation, perform TCP window scaling, and handle packet reassembly in software.

### 2. RDMA Verbs Acceleration (Assignment 2)
RDMA Verbs achieve orders of magnitude lower latency (~2.0 µs – 3.6 µs for small payloads, ~0.71 ms for 4 MB messages), yielding up to a **38x - 100x speedup** over TCP:
* **Kernel Bypass & Zero-Copy Hardware Architecture:** User-space application memory buffers are registered directly with the Host Channel Adapter (HCA). Data is DMAed directly between application host memory and Infiniband NIC hardware without involving kernel protocol stacks.

### 3. Channel Semantics (Task 1) vs. Memory Semantics (Task 2)
* **Channel Semantics (Send/Recv):** Requires both sender and receiver to participate active CPU work. The receiver must post a Receive Work Request (RWR) to its Receive Queue (RQ) prior to packet arrival.
* **Memory Semantics (RDMA Write):** Achieves true **receiver CPU bypass**. The local host directly writes into registered remote host memory using virtual memory keys (`rkey`) without requiring any CPU interaction or software thread intervention on the receiving host. Consequently, RDMA Memory Semantics eliminate remote host processing bottlenecks, providing ultra-low, deterministic latency.
