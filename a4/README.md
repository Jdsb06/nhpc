# Assignment 4: single-node Allreduce with shared memory and OpenMP

Roll number: **IMT2024022**. Remote directory:
`/home/student4/IMT2024022/a4/`.

## Current status

The code and PBS script are ready. **Compute-node compilation and benchmarks
are still pending.** There are no measured results or speedup claims yet.

## Files

- `allreduce.c`: stock `MPI_Allreduce`, custom `my_mpi_allreduce`, correctness
  checks and timing in the same program.
- `Makefile`: GNU-compatible MPI C wrapper, OpenMP, native vector instructions.
- `job.sh`: one PBS compute node, 2/4 ranks, thread-count sweep, captured logs.
- `summarize.py`: validate measured CSVs and produce tables for both rank counts.
- `question.txt`: original assignment requirements.

## Algorithm

The custom operation supports out-of-place integer sums within `MPI_COMM_WORLD`
on a single node. Initialization uses `MPI_Comm_split_type` to verify that the
entire communicator shares memory, then creates an MPI shared-memory window.
Each rank has one input slot; rank zero also reserves a shared result buffer.
This setup is reused across calls and excluded from steady-state timings.

Each call performs three steps:

1. Each rank copies its private input into its shared input slot with OpenMP.
2. Each rank computes one disjoint portion of the result, reading the input
   slots directly. OpenMP and SIMD distribute and vectorize the integer sums.
3. Every rank copies the complete shared result into its own private output,
   again with OpenMP.

The requested 2-rank and 4-rank reductions have specialized vectorizable loops.
There are two interprocess synchronization points, each using
`MPI_Win_sync`, `MPI_Barrier`, and `MPI_Win_sync`, inside a passive-target epoch.
OpenMP barriers ensure the worker threads have completed each phase. MPI calls
occur only on the initializing thread, as required by `MPI_THREAD_FUNNELED`.
The MPI standard explains why a process barrier must be supplemented with
[shared-window memory synchronization](https://www.mpi-forum.org/docs/mpi-3.1/mpi31-report/node294.htm).

A final process barrier is unnecessary: the first barrier of the next call
ensures every previous output copy has finished before the shared result can
be overwritten. Destruction explicitly synchronizes the ranks before freeing
the window. Inputs and destinations must be separate private buffers.

## Correctness and timing

For each message size (1, 2, 4, 8, 16, 32 and 64 MiB), the program runs one
stock and one custom operation into **different destination buffers**, compares
them using `memcmp`, and verifies that **every rank** passes. It also checks
zero-length, tiny, and uneven counts with changing positive/negative inputs.
Checks repeat after each timing trial to exercise buffer reuse.

Each implementation gets five warmup calls. A default measurement has seven
trials of twenty calls. The order of the two implementations alternates between
trials. Each trial reports the maximum per-call elapsed time across ranks; CSV
times are the median of those trial means. Synchronization, shared-buffer input
copies, reduction, and private-output copies are all included in custom timing.
Allocation, initialization, correctness checks, and reporting are excluded.
The raw per-trial samples remain in the configuration's `.log` file.

The thread sweep uses 1, 2, 4 and 8 threads per rank where the allocation permits.
Rank count multiplied by thread count never exceeds allocated PBS slots. MPI
uses its standard collective selection and the shared-memory transport; there
is no artificial delay or forced slow Allreduce algorithm. The custom method
may use more cores than the MPI baseline; report both ranks and threads when
describing any speedup. Thread creation, synchronization and memory bandwidth
can limit performance, especially for small messages.

All sizes are binary MiB (1 MiB = 1,048,576 bytes). Results also contain exact byte
counts so there is no ambiguity with decimal MB.

## Upload and run when campus access is available

From this local `a4` directory:

```bash
ssh nhpc 'mkdir -p /home/student4/IMT2024022/a4'
scp allreduce.c Makefile job.sh summarize.py README.md question.txt \
    nhpc:/home/student4/IMT2024022/a4/
ssh nhpc 'cd /home/student4/IMT2024022/a4 && qsub job.sh'
```

`job.sh` requests `nodes=1:ppn=16`. Before submitting, check available compute-node
capacity with `pbsnodes -a`; adjust `ppn` if necessary. Do not run the benchmark
on the login node. The job loads `intel-2018` if no MPI wrapper is already in
the environment and compiles on the compute node. Its launcher flags and pinning
configuration target the Intel MPI environment used in Assignment 3.

The PBS output is directed to `IMT2024022/a4/pbs.log`. Project data, build outputs,
measurement logs, result tables and explicitly configured temporary directories
stay below `IMT2024022/a4/`. The cluster scheduler and MPI runtime may also use
their own system-managed storage.

```bash
ssh nhpc 'qstat -u student4'
ssh nhpc 'ls -lt /home/student4/IMT2024022/a4/results/'
# Substitute the actual directory name printed by the job:
scp -r nhpc:/home/student4/IMT2024022/a4/results/RUN_DIRECTORY ./results/
```

Each run preserves `environment.txt`, `sources.sha256`, node allocation, all
configuration CSVs, and raw timing logs. `summary.md` and
`all_configurations.csv` are generated automatically. The summary chooses one
fixed thread count per rank count by geometric mean speedup across all seven
sizes, labels that as exploratory tuning, and includes the entire thread sweep.
An independent repeat with the selected configuration is needed to substantiate
a tuned speedup. Slower results are retained and reported.

To regenerate a summary:

```bash
python3 summarize.py results/RUN_DIRECTORY
```

If any size is slower than MPI, inspect the thread sweep, binding and node
topology, optimize, and rerun; do not describe the assignment's speedup target
as achieved until the measurements demonstrate it for both rank counts.
