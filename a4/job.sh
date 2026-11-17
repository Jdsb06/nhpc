#!/bin/bash
#PBS -N IMT2024022_a4
#PBS -S /bin/bash
#PBS -l select=1:ncpus=16:mpiprocs=16:mem=4gb
#PBS -l place=excl
#PBS -l walltime=00:30:00
#PBS -j oe
#PBS -o /home/student4/IMT2024022/a4/pbs.log

set -euo pipefail
cd /home/student4/IMT2024022/a4
export LC_ALL=C
if ! command -v mpicc >/dev/null; then
    source /etc/profile.d/modules.sh
    module load intel-2018
fi

mkdir -p results
run_dir="results/$(date +%Y%m%d_%H%M%S)_${PBS_JOBID:-manual}"
mkdir -p "$run_dir"
exec >"$run_dir/job.log" 2>&1
export TMPDIR="$PWD/$run_dir/tmp"
export I_MPI_HYDRA_TMPDIR="$TMPDIR"
mkdir -p "$TMPDIR"

if [[ -z "${PBS_NODEFILE:-}" || ! -f "$PBS_NODEFILE" ]]; then
    echo 'Submit this script with qsub on a single compute node.' >&2
    exit 1
fi
if [[ $(sort -u "$PBS_NODEFILE" | wc -l) -ne 1 ]]; then
    echo 'This assignment requires exactly one compute node.' >&2
    exit 1
fi
slots=$(wc -l < "$PBS_NODEFILE")
cp "$PBS_NODEFILE" "$run_dir/nodes.txt"
{
    date -Is
    hostname
    lscpu
    mpicc -show
    mpirun --version
    env | sort | sed '/^PBS_JOBCOOKIE=/d' | sed -n '/^PBS_/p; /^OMP_/p; /^I_MPI_/p'
} > "$run_dir/environment.txt"
mkdir -p "$run_dir/source"
cp allreduce.c job.sh Makefile summarize.py "$run_dir/source/"
sha256sum allreduce.c job.sh Makefile summarize.py > "$run_dir/sources.sha256"
make
export OMP_DYNAMIC=FALSE
export OMP_PROC_BIND=close
export OMP_PLACES=cores
export I_MPI_FABRICS=shm
export I_MPI_PIN=1
export I_MPI_PIN_DOMAIN=omp
export I_MPI_PIN_RESPECT_CPUSET=1

# Keep ranks * threads within the allocated cores and save every result.
for ranks in 2 4; do
    if (( ranks == 2 )); then
        thread_counts=${THREADS_2:-'1 2 4 8'}
    else
        thread_counts=${THREADS_4:-'1 2 4 8'}
    fi
    for threads in $thread_counts; do
        if (( ranks * threads > slots )); then continue; fi
        export OMP_NUM_THREADS="$threads"
        mpirun -np "$ranks" -ppn "$ranks" -f "$PBS_NODEFILE" \
            ./allreduce --threads "$threads" --iterations "${ITERATIONS:-20}" \
            --trials "${TRIALS:-7}" \
            > "$run_dir/r${ranks}_t${threads}.csv" \
            2> "$run_dir/r${ranks}_t${threads}.log"
    done
done
if command -v python3 >/dev/null; then
    python3 summarize.py "$run_dir"
else
    echo "Python 3 is unavailable here. Run on the login node: python3 summarize.py $run_dir"
fi
echo "Completed: $PWD/$run_dir"
