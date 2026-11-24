"""End-to-end CLI regression tests using temporary files only."""
import hashlib
import json
import os
import random
import signal
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

CLI = Path(sys.argv[1]).resolve()


def run(*args, ok=True):
    result = subprocess.run([CLI, *args], capture_output=True, text=True, timeout=12)
    assert (result.returncode == 0) == ok, (args, result.returncode, result.stdout, result.stderr)
    return result


def round_trips(work):
    randomizer = random.Random(827)
    samples = {"empty": b"", "tiny.txt": b"chunkpress\n", "file with spaces-ü.bin": randomizer.randbytes(221_031),
               "exact-chunks": b"01234567" * 1024, "repeated": b"same phrase\n" * 52_071}
    for label, content in samples.items():
        source = work / label
        source.write_bytes(content)
        for mode in ("sequential", "pipeline"):
            archive = work / f"{label}-{mode}.cpa"
            out = work / f"{label}-{mode}.restored"
            args = ("--sequential",) if mode == "sequential" else ("--workers", "8", "--priority", "even")
            result = run("compress", source, archive, "--chunk-kib", "4", "--capacity", "1", "--json", *args)
            metrics = json.loads(result.stdout)
            assert metrics["original_bytes"] == len(content)
            assert metrics["archive_bytes"] == archive.stat().st_size
            if not content:
                assert metrics["archive_ratio"] is None
            run("verify", archive)
            run("verify", archive, source)
            run("extract", archive, out, "--reference", source)
            assert out.read_bytes() == content
            before = hashlib.sha256(archive.read_bytes()).digest()
            run("compress", source, archive, ok=False)
            run("extract", archive, out, ok=False)
            assert hashlib.sha256(archive.read_bytes()).digest() == before
            assert out.read_bytes() == content


def malformed_archives(work):
    source = work / "index-source"
    content = random.Random(43).randbytes(9_001)
    source.write_bytes(content)
    archive = work / "valid-index.cpa"
    run("compress", source, archive, "--chunk-kib", "4", "--sequential")
    valid = archive.read_bytes()
    index = struct.unpack_from("<Q", valid, 32)[0]
    records = [struct.unpack_from("<QQIIII", valid, index + 32 * i) for i in range(3)]
    header = valid[:40]

    payload = bytearray()
    metadata = bytearray()
    for record in reversed(records):
        chunk, offset, raw_size, packed_size, checksum, flags = record
        metadata.extend(struct.pack("<QQIIII", chunk, 40 + len(payload), raw_size, packed_size, checksum, flags))
        payload.extend(valid[offset:offset + packed_size])
    shuffled = work / "out-of-order.cpa"
    shuffled.write_bytes(header + payload + metadata)
    run("extract", shuffled, work / "out-of-order.bin", "--reference", source)

    corruptions = {"truncated": valid[:-1], "magic": b"BADMAGIC" + valid[8:], "extra": valid + b"\0",
                   "payload": valid[:47] + bytes([valid[47] ^ 0x40]) + valid[48:],
                   "offset": valid[:32] + b"\xff" * 8 + valid[40:],
                   "duplicate ID": valid[:index + 32] + b"\0" * 8 + valid[index + 40:]}
    target = work / "should-not-exist"
    for name, data in corruptions.items():
        bad = work / f"bad-{name}.cpa"
        bad.write_bytes(data)
        run("extract", bad, target, ok=False)
        assert not target.exists()
    wrong = work / "wrong-reference"
    wrong.write_bytes(bytes([content[0] ^ 1]) + content[1:])
    run("verify", archive, wrong, ok=False)
    run("extract", archive, target, "--reference", wrong, ok=False)
    assert not target.exists()
    run("compress", source, source, ok=False)
    run("compress", work, target, ok=False)
    run("compress", source, target, "--workers", "0", ok=False)
    run("compress", source, target, "--workers", "32", "--chunk-kib", "16384", ok=False)
    run("compress", source, target, "--priority", "invalid", ok=False)
    fifo = work / "named pipe"
    os.mkfifo(fifo)
    run("compress", fifo, target, ok=False)
    symlink = work / "output-symlink"
    symlink.symlink_to(work / "absent-file")
    run("compress", source, symlink, ok=False)
    assert symlink.is_symlink()


def signal_cancellation(work):
    source = work / "signal-source.bin"
    block = random.Random(9).randbytes(1024 * 1024)
    with source.open("wb") as handle:
        for _ in range(48):
            handle.write(block)
    archive = work / "killed.cpa"
    process = subprocess.Popen([CLI, "compress", source, archive, "--sequential"], stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True)
    try:
        deadline = time.monotonic() + 8
        while not list(work.glob(".chunkpress-*")) and process.poll() is None and time.monotonic() < deadline:
            time.sleep(0.005)
        assert process.poll() is None
        process.send_signal(signal.SIGINT)
        _, errors = process.communicate(timeout=8)
        assert process.returncode == 130 and "cancelled" in errors, errors
        assert not archive.exists() and not list(work.glob(".chunkpress-*"))
    finally:
        if process.poll() is None:
            process.kill()
            process.communicate()


with tempfile.TemporaryDirectory(prefix="chunkpress-cli-") as directory:
    path = Path(directory)
    round_trips(path)
    malformed_archives(path)
    signal_cancellation(path)
    print("Round trips, reordered indexes, corrupt input, output protection, and Ctrl+C passed.")
