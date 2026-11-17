# Assignment 4 measured results

Times are median trial means in microseconds per call; each trial uses the slowest rank. Speedup = MPI time / custom time.

When multiple thread counts are present, the summary selects one fixed count per rank count by geometric mean speedup over all seven sizes. That selection is exploratory tuning on the same measurements. A run with one configuration per rank count simply reports those fixed configurations. All measurements remain in `all_configurations.csv`.

## 2 ranks, 8 OpenMP threads per rank

| Size (MiB) | MPI (us) | Custom (us) | Speedup | memcmp |
| ---: | ---: | ---: | ---: | :--- |
| 1 | 423.91 | 159.35 | 2.66x | PASS |
| 2 | 835.49 | 292.35 | 2.86x | PASS |
| 4 | 1826.06 | 561.65 | 3.25x | PASS |
| 8 | 6163.10 | 1300.95 | 4.74x | PASS |
| 16 | 14617.20 | 3390.50 | 4.31x | PASS |
| 32 | 34860.69 | 7453.66 | 4.68x | PASS |
| 64 | 83938.85 | 15632.61 | 5.37x | PASS |

Custom faster at 7/7 sizes. Geometric mean speedup: 3.86x.

Thread sweep (geometric mean speedup):

- 1 threads/rank: 1.18x
- 2 threads/rank: 2.11x
- 4 threads/rank: 2.95x
- 8 threads/rank: 3.86x

## 4 ranks, 4 OpenMP threads per rank

| Size (MiB) | MPI (us) | Custom (us) | Speedup | memcmp |
| ---: | ---: | ---: | ---: | :--- |
| 1 | 1067.25 | 251.95 | 4.24x | PASS |
| 2 | 2472.51 | 476.44 | 5.19x | PASS |
| 4 | 5925.15 | 1142.44 | 5.19x | PASS |
| 8 | 12013.85 | 3068.85 | 3.91x | PASS |
| 16 | 24244.95 | 6615.45 | 3.66x | PASS |
| 32 | 61477.15 | 13679.19 | 4.49x | PASS |
| 64 | 113735.75 | 27254.55 | 4.17x | PASS |

Custom faster at 7/7 sizes. Geometric mean speedup: 4.37x.

Thread sweep (geometric mean speedup):

- 1 threads/rank: 1.87x
- 2 threads/rank: 2.51x
- 4 threads/rank: 4.37x
