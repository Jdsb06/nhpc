# Assignment 4: faster single-node Allreduce

**Roll number:** IMT2024022
**Remote directory:** `/home/student4/IMT2024022/a4/`

The implementation uses MPI shared memory and OpenMP to speed up integer-sum
Allreduce on one compute node. Read [report.md](report.md) for the measured
comparison with `MPI_Allreduce` for 1, 2, 4, 8, 16, 32 and 64 MiB messages.

## Files

- `allreduce.c`: both implementations, `memcmp` checks and timing.
- `job.sh`: single-node PBS job and OpenMP thread sweep.
- `validate_job.sh`: repeat the selected thread counts on `compute02`.
- `summarize.py`: check CSV results and generate comparison tables.
- `plot_results.py`: recreate the chart locally with matplotlib.
- `measurements/sweep/`: all thread configurations and raw timings.
- `measurements/validation/`: separate run using the selected configurations.
- `question.txt`: the assignment question.

## Run on the cluster

Upload from this local `a4` directory:

```bash
ssh nhpc 'mkdir -p /home/student4/IMT2024022/a4'
scp allreduce.c Makefile job.sh validate_job.sh summarize.py question.txt \
    nhpc:/home/student4/IMT2024022/a4/
```

The cluster's `qsub` wrapper routes jobs through `student3`. Give that account
access only to the assignment directory. Setting `HOME` for the submission
keeps the wrapper's staging files under the roll-number directory too.

```bash
ssh nhpc
cd /home/student4/IMT2024022/a4
setfacl -m u:student3:rwx,d:u:student3:rwx,d:u:student4:rwx .
HOME="$PWD" qsub job.sh
```

The job reserves one node exclusively, with 16 cores and 4 GiB of memory. It
loads Intel MPI 2018 and builds with its GCC-backed `mpicc` wrapper:

```bash
make
# mpicc -O3 -march=native -std=c11 -Wall -Wextra -Wpedantic -fopenmp allreduce.c -o allreduce
```

GCC 4.8.5 is available on the cluster. The loops use compatible OpenMP
directives; `-O3 -march=native` lets the compiler vectorize suitable loops.
The Intel C compiler is not needed.

The sweep tests 2 and 4 MPI ranks with 1, 2, 4 or 8 OpenMP threads per rank,
skipping configurations that exceed the 16-core allocation. Intel MPI uses
shared-memory transport and one pinning domain per OpenMP team. No Allreduce
algorithm is forced for the MPI baseline.

Results go into `results/DATE_JOBID/`. Python 3 is available on the login node,
so generate the summary there if it is unavailable on the allocated compute node:

```bash
python3 summarize.py results/DATE_JOBID
```

To repeat the selected configurations with nine trials of thirty calls:

```bash
HOME="$PWD" qsub validate_job.sh
```

That job uses 8 threads per rank for 2 ranks and 4 threads per rank for 4 ranks,
on `compute02`, matching the measured experiment. Runtime files and results
stay in `IMT2024022/a4/`; PBS and MPI also use their own system-managed storage.
