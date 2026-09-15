#!/bin/sh
#PBS -N rdma_test
#PBS -l nodes=2:ppn=1
#PBS -l walltime=00:08:00

echo "allocated nodes:"
cat $PBS_NODEFILE
echo "running on: $(hostname)"

# Load the MPI environment
module load intel-2018

# Target working directory
TARGET_DIR="/home/student4/IMT2024022"
cd "$TARGET_DIR" || exit 1

# Run exactly 2 processes (-np 2)
mpirun -genv I_MPI_FABRICS=ofa -np 2 -f $PBS_NODEFILE ./rdma_bench
