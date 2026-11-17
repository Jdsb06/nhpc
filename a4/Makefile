MPICC ?= mpicc
CFLAGS ?= -O3 -march=native -std=c11 -Wall -Wextra -Wpedantic -fopenmp

.PHONY: all
all: allreduce

allreduce: allreduce.c
	$(MPICC) $(CFLAGS) $< -o $@
