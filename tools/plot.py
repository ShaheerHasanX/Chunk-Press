#!/usr/bin/env python3
"""Draw the benchmark graphs from a summary.csv produced by benchmark.py."""
import argparse
import csv
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FixedLocator, FuncFormatter, NullLocator

PALETTE = ("#006457", "#53776B", "#375982", "#858095")


def plot(source, output):
    with source.open(encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))
    conditions = list(dict.fromkeys((row["file_type"], row["size_mib"]) for row in rows))
    plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 10,
                         "axes.spines.top": False, "axes.spines.right": False,
                         "axes.labelcolor": "#243632", "xtick.color": "#243632",
                         "ytick.color": "#243632", "axes.edgecolor": "#AEBFB8"})
    figure, axes = plt.subplots(3, 1, figsize=(8.4, 9.3), constrained_layout=True,
                                gridspec_kw={"hspace": 0.22})
    metrics = [("median_seconds", "Elapsed time", "Seconds", True),
               ("mib_per_second", "Throughput", "MiB/s", True),
               ("speedup", "Speedup against sequential", "× sequential", False)]
    for axis, (metric, title, unit, logarithmic) in zip(axes, metrics):
        for (kind, size), color, marker in zip(conditions, PALETTE, ("o", "s", "^", "D")):
            values = [float(row[metric]) for row in rows if (row["file_type"], row["size_mib"]) == (kind, size)]
            axis.plot(range(len(values)), values, marker=marker, markersize=5, linewidth=1.6,
                      color=color, label=f"{kind} · {size} MiB")
        if logarithmic:
            axis.set_yscale("log")
            ticks = [0.05, 0.1, 0.2, 0.5, 1.0] if metric == "median_seconds" else [30, 50, 100, 200]
            axis.yaxis.set_major_locator(FixedLocator(ticks))
            axis.yaxis.set_major_formatter(FuncFormatter(lambda value, _: f"{value:g}"))
            axis.yaxis.set_minor_locator(NullLocator())
        else:
            axis.axhline(1, color="#8E9997", linewidth=0.75, linestyle="--")
        axis.set_title(title, loc="left", fontweight="bold", color="#182B26", pad=9)
        axis.set_ylabel(unit)
        axis.set_xticks(range(5), ["Sequential", "1 worker", "2 workers", "4 workers", "8 workers"])
        axis.grid(axis="y", color="#DDE5E1", linewidth=0.65)
    axes[0].legend(loc="upper center", bbox_to_anchor=(0.5, 1.34), ncols=2,
                   frameon=False, fontsize=9)
    output.mkdir(parents=True, exist_ok=True)
    figure.savefig(output / "performance.png", dpi=180, facecolor="white")
    figure.savefig(output / "performance.pdf", facecolor="white")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("summary", type=Path)
    parser.add_argument("--output", type=Path, default=Path("benchmark-output"))
    args = parser.parse_args()
    plot(args.summary, args.output)


if __name__ == "__main__":
    main()
