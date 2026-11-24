# Chunkpress archive format, version 1

A `.cpa` file consists of a 40-byte header, independent zlib streams and a trailing index. All integers are unsigned and little-endian. An archive stores one file’s bytes. The person extracting it chooses the destination filename.

## Header

| Offset | Bytes | Value |
| ---: | ---: | --- |
| 0 | 8 | ASCII magic `CHNKPRS1` |
| 8 | 4 | Format version, 1 |
| 12 | 4 | Configured chunk size in bytes |
| 16 | 8 | Original file length in bytes |
| 24 | 8 | Number of chunks, rounded up from original length divided by chunk size |
| 32 | 8 | Byte offset of the trailing index |

An empty source has zero chunks and yields a 40-byte archive. The permitted chunk size is 4 KiB to 16 MiB and the implementation accepts at most 1,000,000 chunk entries.

## Index entry

One 32-byte entry is written for each payload in physical write order. Worker completion can make this order differ from the source. Extraction validates entries in physical order and stores them by chunk ID to reconstruct the source.

| Entry offset | Bytes | Value |
| ---: | ---: | --- |
| 0 | 8 | Zero-based chunk ID |
| 8 | 8 | Payload offset from the start of the archive |
| 16 | 4 | Original chunk length |
| 20 | 4 | Compressed payload length |
| 24 | 4 | CRC-32 of the original chunk |
| 28 | 4 | Reserved, zero |

The parser requires unique IDs in range, expected original lengths, consecutive and non-overlapping payloads, and an exact index length. Each zlib stream must consume its entire stored payload and decode to the expected size and CRC-32. Supplying a reference compares decoded bytes directly to the original. CRC-32 checks accidental corruption.

## Output lifecycle

The app creates a private temporary file next to the destination. On success it completes the index or verifies extraction, syncs the file and publishes it atomically with no replacement. Linux `renameat2` with `RENAME_NOREPLACE` provides the normal path, with a same-directory hard link as a fallback where supported. Errors and graceful cancellation remove the temporary file. The application can detect ordinary changes to the open source by comparing its size and timestamps before completing. Keep input files unchanged during a job.
