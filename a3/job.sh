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
