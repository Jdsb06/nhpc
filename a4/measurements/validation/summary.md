# Assignment 4 measured results

Times are median trial means in microseconds per call; each trial uses the slowest rank. Speedup = MPI time / custom time.

When multiple thread counts are present, the summary selects one fixed count per rank count by geometric mean speedup over all seven sizes. That selection is exploratory tuning on the same measurements. A run with one configuration per rank count simply reports those fixed configurations. All measurements remain in `all_configurations.csv`.

## 2 ranks, 8 OpenMP threads per rank

| Size (MiB) | MPI (us) | Custom (us) | Speedup | memcmp |
| ---: | ---: | ---: | ---: | :--- |
| 1 | 424.30 | 143.73 | 2.95x | PASS |
| 2 | 837.76 | 277.67 | 3.02x | PASS |
| 4 | 1736.00 | 546.27 | 3.18x | PASS |
| 8 | 5806.13 | 1330.47 | 4.36x | PASS |
| 16 | 14979.97 | 3549.20 | 4.22x | PASS |
| 32 | 34304.40 | 7674.17 | 4.47x | PASS |
| 64 | 83390.83 | 15669.27 | 5.32x | PASS |

Custom faster at 7/7 sizes. Geometric mean speedup: 3.84x.

Thread sweep (geometric mean speedup):

- 8 threads/rank: 3.84x

## 4 ranks, 4 OpenMP threads per rank

| Size (MiB) | MPI (us) | Custom (us) | Speedup | memcmp |
| ---: | ---: | ---: | ---: | :--- |
| 1 | 1088.50 | 243.37 | 4.47x | PASS |
| 2 | 2501.40 | 462.17 | 5.41x | PASS |
| 4 | 5856.03 | 1111.09 | 5.27x | PASS |
| 8 | 12087.70 | 2983.80 | 4.05x | PASS |
| 16 | 24343.67 | 6453.63 | 3.77x | PASS |
| 32 | 56729.83 | 13560.60 | 4.18x | PASS |
| 64 | 112630.10 | 27223.83 | 4.14x | PASS |

Custom faster at 7/7 sizes. Geometric mean speedup: 4.43x.

Thread sweep (geometric mean speedup):

- 4 threads/rank: 4.43x
