#!/usr/bin/env python3
"""Measure the actual CLI engine and verify every archive against its input."""
import argparse
import csv
import json
import os
import platform
import random
import statistics
import subprocess
import tempfile
from collections import defaultdict
from pathlib import Path

MIB = 1024 * 1024
MODES = ("Sequential", "1 worker", "2 workers", "4 workers", "8 workers")
CASES = [(kind, size) for kind in ("Log text", "Random binary") for size in (8, 48)]


def fixture(path, kind, size):
    randomizer = random.Random(7204)
    with path.open("wb") as handle:
        if kind == "Random binary":
            for _ in range(size):
                handle.write(randomizer.randbytes(MIB))
            return
        block = bytearray()
        for index in range(20_000):
            line = (f"2026-11-15T09:{index % 60:02d}:{index % 57:02d} INFO "
                    f"node-{index % 37:02d} job={randomizer.randrange(5000):04d} "
                    f"event={'accepted' if index % 23 else 'retry'} bytes={index % 700:04d}\n")
            block.extend(line.encode())
            if len(block) >= MIB:
                break
        for _ in range(size):
            handle.write(block[:MIB])


def write_csv(path, rows):
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)


def run(binary, output, trials):
    output.mkdir(parents=True, exist_ok=True)
    raw = []
    order = random.Random(3021)
    with tempfile.TemporaryDirectory(prefix="work-", dir=output) as directory:
        work = Path(directory)
        for kind, size in CASES:
            source = work / "input"
            archive = work / "trial.cpa"
            fixture(source, kind, size)
            for trial in range(1, trials + 1):
                modes = list(MODES)
                order.shuffle(modes)
                for mode in modes:
                    mode_args = ["--sequential"] if mode == "Sequential" else ["--workers", mode.split()[0]]
                    command = [binary, "compress", source, archive, "--chunk-kib", "1024",
                               "--capacity", "8", "--json", *mode_args]
                    result = json.loads(subprocess.check_output(command, text=True))
                    subprocess.run([binary, "verify", archive, source], check=True, stdout=subprocess.DEVNULL)
                    archive.unlink()
                    raw.append({"file_type": kind, "size_mib": size, "trial": trial, "mode": mode,
                                "seconds": result["seconds"], "original_bytes": result["original_bytes"],
                                "archive_bytes": result["archive_bytes"]})
            print(f"Verified {trials * len(MODES)} archives for {kind}, {size} MiB.", flush=True)

    grouped = defaultdict(list)
    for row in raw:
        grouped[row["file_type"], row["size_mib"], row["mode"]].append(row)

    summary = []
    for kind, size in CASES:
        baseline = statistics.median(row["seconds"] for row in grouped[kind, size, "Sequential"])
        for mode in MODES:
            group = grouped[kind, size, mode]
            seconds = statistics.median(row["seconds"] for row in group)
            example = group[0]
            summary.append({"file_type": kind, "size_mib": size, "mode": mode,
                            "median_seconds": round(seconds, 6),
                            "mib_per_second": round(size / seconds, 2),
                            "speedup": round(baseline / seconds, 3),
                            "archive_percent": round(100 * example["archive_bytes"] / example["original_bytes"], 3)})
    write_csv(output / "trials.csv", raw)
    write_csv(output / "summary.csv", summary)
    cpu = platform.processor()
    try:
        cpu = next(line.split(":", 1)[1].strip() for line in Path("/proc/cpuinfo").read_text().splitlines()
                   if line.startswith("model name"))
    except (OSError, StopIteration):
        pass
    distro = platform.freedesktop_os_release().get("PRETTY_NAME", "") if hasattr(platform, "freedesktop_os_release") else ""
    environment = {"os": distro, "kernel": platform.release(), "cpu": cpu,
                   "logical_cpus_available": len(os.sched_getaffinity(0)), "trials_per_configuration": trials,
                   "chunk_kib": 1024, "queue_capacity_each": 8, "zlib_level": 6}
    (output / "environment.json").write_text(json.dumps(environment, indent=2) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path, help="Path to the compiled chunkpress-cli")
    parser.add_argument("--output", type=Path, default=Path("benchmark-output"))
    parser.add_argument("--trials", type=int, default=3)
    args = parser.parse_args()
    if args.trials < 1:
        parser.error("--trials must be at least 1")
    run(args.binary.resolve(), args.output, args.trials)


if __name__ == "__main__":
    main()
