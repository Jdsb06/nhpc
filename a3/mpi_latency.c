#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

#define MAX_SIZE (1024 * 1024 * 4)
#define ITERATIONS 100
#define WARMUP 10

int main(int argc, char **argv) {
  int size, rank, src_rank = 0, dst_rank = 1;
  char *src = NULL, *dst = NULL;
  int i, msg_size;
  MPI_Status status;
  struct timeval start, end;
  float total_time = 0;

  if (argc > 1) src_rank = atoi(argv[1]);
  if (argc > 2) dst_rank = atoi(argv[2]);

  MPI_Init(&argc, &argv);

  MPI_Comm_size(MPI_COMM_WORLD, &size);
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  if (rank == src_rank) {
    printf("\n=======================================================\n");
    printf("         Unmodified MPI Latency Benchmark              \n");
    printf("=======================================================\n");
    printf("%14s   %12s   %18s\n", "Size (bytes)", "Iterations", "Avg Latency (us)");
    printf("-------------------------------------------------------\n");
  }

  src = (char *)malloc(MAX_SIZE);
  dst = (char *)malloc(MAX_SIZE);
  if (src == NULL || dst == NULL) {
     perror("cannot allocate memory\n");
     exit(1);
  }

  // Warmup iterations starting from 1 byte
  for (msg_size = 1; msg_size <= MAX_SIZE; msg_size *= 2) {
    for (i = 0; i < WARMUP; i++) {
       if (rank == src_rank) {
         MPI_Send(src, msg_size, MPI_CHAR, dst_rank, 1, MPI_COMM_WORLD);
         MPI_Recv(dst, msg_size, MPI_CHAR, dst_rank, 1, MPI_COMM_WORLD, &status);
       } else if (rank == dst_rank) {
         MPI_Recv(dst, msg_size, MPI_CHAR, src_rank, 1, MPI_COMM_WORLD, &status);
         MPI_Send(src, msg_size, MPI_CHAR, src_rank, 1, MPI_COMM_WORLD);
       }
    }
  }

  // Measured Benchmark iterations starting from 1 byte
  for (msg_size = 1; msg_size <= MAX_SIZE; msg_size *= 2) {
    MPI_Barrier(MPI_COMM_WORLD);

    gettimeofday(&start, NULL);
    for (i = 0; i < ITERATIONS; i++) {
       if (rank == src_rank) {
         MPI_Send(src, msg_size, MPI_CHAR, dst_rank, 1, MPI_COMM_WORLD);
         MPI_Recv(dst, msg_size, MPI_CHAR, dst_rank, 1, MPI_COMM_WORLD, &status);
       } else if (rank == dst_rank) {
         MPI_Recv(dst, msg_size, MPI_CHAR, src_rank, 1, MPI_COMM_WORLD, &status);
         MPI_Send(src, msg_size, MPI_CHAR, src_rank, 1, MPI_COMM_WORLD);
       }
    }
    gettimeofday(&end, NULL);

    total_time = ((end.tv_sec - start.tv_sec) * 1000000.0 + (end.tv_usec - start.tv_usec)) / 2.0;
    total_time /= ITERATIONS;

    if (rank == src_rank) {
      printf("%14d   %12d   %18.2f\n", msg_size, ITERATIONS, total_time);
    }
  }

  free(src);
  free(dst);
  MPI_Finalize();
  return 0;
}
