#!/bin/bash
#PBS -N IMT2024022_a4_validate
#PBS -S /bin/bash
#PBS -l select=1:ncpus=16:mpiprocs=16:mem=4gb:host=compute02
#PBS -l place=excl
#PBS -l walltime=00:15:00
#PBS -j oe

# Repeat the best thread counts from the sweep on the same compute node.
export THREADS_2=8
export THREADS_4=4
export ITERATIONS=30
export TRIALS=9
exec bash /home/student4/IMT2024022/a4/job.sh
