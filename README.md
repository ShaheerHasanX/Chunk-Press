# Chunkpress

**Parallel, lossless file compression for Ubuntu.** Chunkpress divides a file into independent chunks, compresses them with zlib across multiple threads, and stores them in a `.cpa` archive. Use the Qt desktop app to compress, extract, or verify a file, or use the CLI to run the same operations from the terminal.

In the project's 48 MiB log benchmark, **eight workers reduced median compression time from 0.929 s to 0.177 s (5.26× speedup)**. Extraction was checked byte for byte against the original.

- **Monitor the pipeline.** See worker activity and queue depth, pause and resume jobs, and resize queues during compression.
- **Tune concurrency.** Configure the worker pool, active worker limit, chunk size, and FIFO or even-chunk priority.
- **Check your files.** Verify the archive's structure and per-chunk CRC-32, with an optional comparison against the original.

## Run on Ubuntu

Open a Terminal in the extracted `Chunkpress` folder. Install the required packages once.

```bash
sudo apt update
sudo apt install build-essential cmake zlib1g-dev qt6-base-dev libxkbcommon-dev fonts-dejavu-core python3
```

Build and open the desktop app.

```bash
bash run.sh
```

The first launch compiles the app. Later launches can use `./build/chunkpress` directly. Run the app as your normal Ubuntu desktop user. Package installation is the only step that needs Internet access. The app does its work in local files and uses no accounts, server, database, or network service.

## Classroom demo

1. Leave **Compress** selected and use **Browse** to choose a file. The app suggests `<filename>.cpa` in the same folder. Choose a different destination if that file already exists.
2. Start with **4 workers**, a **1024 KiB** chunk size, and **8 slots per queue**. To demonstrate the active worker semaphore, set **Active worker limit** to **2**.
3. Select **Even chunks first** to demonstrate priority selection. Even IDs are served ahead of odd IDs already waiting. Chunks with the same priority are served in arrival order.
4. Click **Compress file**. The monitor shows written chunks, archive bytes, the size ratio and each worker’s state. On a larger file, click **Pause**, wait for **Paused**, change **Slots per queue**, then click **Resume**.
5. Click **Extract archive** after compression completes. The app fills in the archive, a `restored-…` filename and the original file for a byte-for-byte comparison. Select **Extract file**.
6. Choose **Verify** to check an archive without writing a copy. Select the source file for a byte-for-byte comparison. With an empty reference field, Verify checks archive structure, decompression and per-chunk checksums.

Create a compressible input to demonstrate size reduction, then use random input to demonstrate queue activity or pause and resume.

```bash
python3 -c 'import sys; sys.stdout.buffer.write((b"INFO request completed successfully\n" * 1000000)[:33554432])' > demo-text.log
dd if=/dev/urandom of=demo-random.bin bs=1M count=96 status=progress
```

Repeating text should shrink substantially. Random bytes can make the archive slightly larger because each chunk has a zlib stream and an index entry. More workers affect processing time. Archive size depends on the data and compression settings.

For a repeat demo, choose a new output filename. Chunkpress always protects existing files. Cancellation discards the unfinished output after active chunks reach a boundary. Forcing the process to quit with SIGKILL can leave a hidden `.chunkpress-*` temporary file next to the chosen destination.

## Command line and tests

Run these from the same directory after building.

```bash
./build/chunkpress-cli compress demo-text.log demo.cpa --workers 4 --active-limit 2 --priority even
./build/chunkpress-cli verify demo.cpa demo-text.log
./build/chunkpress-cli extract demo.cpa recovered.log --reference demo-text.log
cmp demo-text.log recovered.log
ctest --test-dir build --output-on-failure
```

Use `--help` for the CLI settings. `--sequential` selects the no-worker-pool baseline. The CLI handles Ctrl+C and SIGTERM at chunk boundaries. The tests cover empty and binary files, an exact multiple of the chunk size, a short final chunk, reordered physical chunks, malformed archives, mismatched references, output collisions, pause/resume, resize, active worker limits, and cancellation. The GUI test opens a Qt window offscreen and drives Compress, Extract and Verify.

On an Ubuntu system without a desktop, build the CLI independently.

```bash
sudo apt install build-essential cmake zlib1g-dev python3
cmake -S . -B build-cli -DCHUNKPRESS_GUI=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build-cli --parallel 4
ctest --test-dir build-cli --output-on-failure
```

## Repeat the performance experiment

```bash
python3 tools/benchmark.py build/chunkpress-cli --trials 3
sudo apt install python3-matplotlib
python3 tools/plot.py benchmark-output/summary.csv
```

The benchmark runs a true sequential baseline and pipelines with 1, 2, 4 and 8 compression workers. It uses the same chunk size, zlib level and input data for every mode and verifies each archive outside the timed interval. The bundled raw measurements, medians, environment and graph are in [docs/benchmark](docs/benchmark). The [final report](docs/Chunkpress_Project_Report.pdf) explains the implementation, assumptions and results.

### Reported median compression times

| Input | Sequential | 4 workers | 8 workers |
| --- | ---: | ---: | ---: |
| 8 MiB log text | 0.149 s | 0.048 s | 0.042 s |
| 48 MiB log text | 0.929 s | 0.268 s | 0.177 s |
| 8 MiB random binary | 0.246 s | 0.086 s | 0.055 s |
| 48 MiB random binary | 1.456 s | 0.381 s | 0.285 s |

Timing includes file I/O, compression, index creation, and archive publication. Results vary with the CPU, storage, and input.

## How it works

1. Up to two readers reserve chunk IDs and use `pread` to read at explicit file offsets. They send raw chunks through a bounded input queue.
2. Workers compress each chunk independently and calculate its CRC-32. They send the results through a bounded output queue.
3. One writer appends completed chunks in arrival order and records their IDs, offsets, lengths, and checksums.
4. During extraction, the decoder uses the index to restore chunks in their original order. It validates each chunk and publishes the output after successful verification.

## Source and archive format

| Path | Contents |
| --- | --- |
| `src/archive.cpp` | File I/O, archive layout, zlib, checksum and validation |
| `src/pipeline.cpp` | Reader/worker/writer threads, queues, semaphores and pause state |
| `src/gui.cpp` | Qt Widgets interface and background job lifecycle |
| `src/cli.cpp` | Command-line interface and signal handling |
| `tests/` | Correctness, synchronization and command-line checks |
| [docs/archive-format.md](docs/archive-format.md) | Version 1 archive specification |

The package also contains `Chunkpress.gitbundle` beside the source folder. It has the project’s `main` branch ready to clone if you need Git history. For a course submission, create an empty GitHub repository under your account and push that branch from the cloned directory.

```bash
git clone -b main Chunkpress.gitbundle Chunkpress-repo
cd Chunkpress-repo
git remote set-url origin https://github.com/YOUR-USERNAME/YOUR-REPOSITORY.git
git push -u origin main
```

## Course project

**Multi Threaded File Compression System** · Operating Systems

| Student | Roll number |
| --- | --- |
| Syed Shaheer Hasan | 24K-2023 |
| Syed Muhammad Ahsan | 24K-2041 |
| Muhammad Hassan Gohar | 24K-2033 |

Prepared for Sir Sandesh Kumar.
