#!/usr/bin/env python3
"""Validate actual benchmark CSVs and summarize every measured configuration."""
import csv
import math
from pathlib import Path
import statistics
import sys


def summarize(directory):
    rows = []
    expected_bytes = {2**power for power in range(20, 27)}
    seen = set()
    for filename in sorted(directory.glob("r*_t*.csv")):
        with filename.open(newline="") as stream:
            measurements = list(csv.DictReader(stream))
        sizes = []
        for item in measurements:
            row = {key: int(item[key]) for key in ("ranks", "threads", "bytes")}
            row.update({key: float(item[key]) for key in ("mpi_us", "custom_us")})
            row["memcmp"] = item["memcmp"]
            key = (row["ranks"], row["threads"], row["bytes"])
            if (key in seen or row["ranks"] not in (2, 4) or row["threads"] < 1
                    or item["memcmp"] != "PASS"
                    or any(not math.isfinite(row[k]) or row[k] <= 0
                           for k in ("mpi_us", "custom_us"))):
                raise ValueError(f"Invalid or duplicate measurement in {filename}: {item}")
            row["speedup"] = row["mpi_us"] / row["custom_us"]
            seen.add(key)
            sizes.append(row["bytes"])
            rows.append(row)
        if set(sizes) != expected_bytes or len(sizes) != len(expected_bytes):
            raise ValueError(f"Incomplete size range in {filename}")
    if {r["ranks"] for r in rows} != {2, 4}:
        raise ValueError("Both 2-rank and 4-rank measurements are required")
    fields = ["ranks", "threads", "bytes", "mpi_us", "custom_us", "speedup", "memcmp"]
    with (directory / "all_configurations.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)
    lines = ["# Assignment 4 measured results", "",
             "Times are median trial means in microseconds per call; each trial uses "
             "the slowest rank. Speedup = MPI time / custom time.", "",
             "A fixed thread count is selected for each rank count by the geometric "
             "mean speedup over all seven sizes. This is exploratory tuning on the "
             "same measurements, not an independent validation run. All configurations "
             "remain in `all_configurations.csv`.", ""]
    for ranks in (2, 4):
        configurations = {}
        for row in rows:
            if row["ranks"] == ranks:
                configurations.setdefault(row["threads"], []).append(row)
        def geometric_speedup(items):
            return math.exp(statistics.mean(math.log(r["speedup"]) for r in items))
        selected = max(configurations, key=lambda t: geometric_speedup(configurations[t]))
        lines.extend([f"## {ranks} ranks, {selected} OpenMP threads per rank", "",
                      "| Size (MiB) | MPI (us) | Custom (us) | Speedup | memcmp |",
                      "| ---: | ---: | ---: | ---: | :--- |"])
        selected_rows = sorted(configurations[selected], key=lambda r: r["bytes"])
        for row in selected_rows:
            lines.append(f"| {row['bytes'] // 2**20} | {row['mpi_us']:.2f} | "
                         f"{row['custom_us']:.2f} | {row['speedup']:.2f}x | PASS |")
        wins = sum(row["speedup"] > 1 for row in selected_rows)
        lines.extend(["", f"Custom faster at {wins}/7 sizes. Geometric mean speedup: "
                      f"{geometric_speedup(selected_rows):.2f}x.", "",
                      "Thread sweep (geometric mean speedup):", ""])
        for threads, items in sorted(configurations.items()):
            lines.append(f"- {threads} threads/rank: {geometric_speedup(items):.2f}x")
        lines.append("")
    (directory / "summary.md").write_text("\n".join(lines))
    print("\n".join(lines))


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("Usage: python3 summarize.py RESULTS_DIRECTORY")
    summarize(Path(sys.argv[1]))
