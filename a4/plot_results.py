#!/usr/bin/env python3
"""Plot the separate validation run. Requires matplotlib."""
import csv
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


base = Path(__file__).resolve().parent
with (base / "measurements/validation/all_configurations.csv").open() as stream:
    rows = list(csv.DictReader(stream))

plt.rcParams.update({"font.size": 11, "axes.spines.top": False,
                     "axes.spines.right": False})
figure, axes = plt.subplots(1, 2, figsize=(12, 4.8), sharey=True)
for axis, ranks, threads in zip(axes, (2, 4), (8, 4)):
    data = sorted((r for r in rows if int(r["ranks"]) == ranks),
                  key=lambda r: int(r["bytes"]))
    sizes = [int(r["bytes"]) // 2**20 for r in data]
    stock = [float(r["mpi_us"]) / 1000 for r in data]
    custom = [float(r["custom_us"]) / 1000 for r in data]
    axis.plot(sizes, stock, "o-", color="#526578", label="MPI_Allreduce", linewidth=2)
    axis.plot(sizes, custom, "s-", color="#117C71", label="Shared memory + OpenMP", linewidth=2)
    for size, mpi_time, custom_time in zip(sizes, stock, custom):
        axis.annotate(f"{mpi_time / custom_time:.2f}x", (size, custom_time),
                      xytext=(0, -17), textcoords="offset points", ha="center",
                      color="#117C71", fontsize=9)
    axis.set_xscale("log", base=2)
    axis.set_yscale("log")
    axis.set_xticks(sizes, labels=[str(size) for size in sizes])
    axis.set_ylim(0.07, 200)
    axis.set_title(f"{ranks} MPI ranks, {threads} OpenMP threads per rank")
    axis.set_xlabel("Message size (MiB)")
    axis.grid(True, which="major", alpha=0.2)
    axis.legend(loc="upper left", frameon=False, fontsize=9)
axes[0].set_ylabel("Time per call (ms, logarithmic scale)")
figure.suptitle("Assignment 4: independent validation on compute02", fontsize=15)
figure.text(0.5, 0.015, "Median of 9 trials x 30 calls; slowest rank per trial. Labels show speedup. All memcmp checks passed.",
            ha="center", fontsize=9, color="#45515c")
figure.tight_layout(rect=(0, 0.06, 1, 0.94))
figure.savefig(base / "comparison.png", dpi=180)
