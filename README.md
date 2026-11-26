# LSM-Tree Key-Value Storage Engine (`lsmkv`)

A high-performance, disk-backed LSM-Tree (Log-Structured Merge-tree) persistent key-value storage engine in C++20. Designed with focus on clarity, modular architecture, crash durability, zero memory garbage collection overhead, and reproducible benchmarking.

---

## Architecture Overview

```
                      +-------------------+
                      |   Client API      |
                      | Put/Get/Del/Scan  |
                      +---------+---------+
                                |
             +------------------+------------------+
             | Write Path                          | Read Path
             v                                     v
    +-----------------+                  +-------------------+
    | Write-Ahead Log |                  | Active MemTable   |
    | (WAL with CRC)  |                  | (SkipList)        |
    +--------+--------+                  +---------+---------+
             |                                     | (miss)
             v                                     v
    +-----------------+                  +-------------------+
    | MemTable        |                  | Immutable MemTable|
    | (SkipList)      |                  +---------+---------+
    +--------+--------+                            | (miss)
             | (freeze & flush)                    v
             v                           +-------------------+
    +-----------------+                  | Level 0 SSTables  |
    | SSTable Flush   |                  | (Bloom + Index)   |
    +--------+--------+                  +---------+---------+
             |                                     | (miss)
             v                                     v
    +-----------------+                  +-------------------+
    | Level 0..N      |                  | Level 1..N        |
    | SSTable Files   |<---------------->| SSTables          |
    +-----------------+  Compaction      +-------------------+
```

### Component Breakdown
1. **Core Data Encoding & Checksums** (`src/coding.*`, `src/crc32.*`, `src/format.*`):
   - Internal keys formatted with 56-bit sequence numbers and 8-bit operation tag (`TypeValue` or `TypeDeletion`).
   - IEEE 802.3 CRC32 checksum protected records across WAL, SSTable data blocks, and manifest version edits.
2. **MemTable & Concurrent Skip List** (`src/skiplist.h`, `src/memtable.*`):
   - Lock-free reader skip list with probabilistic height generation and memory order barriers (`acquire`/`release`).
   - Length-prefixed serialization storing internal keys and values sorted by user key ascending and sequence descending.
3. **Write-Ahead Log (WAL)** (`src/wal.*`):
   - Sequential append-only format with 8-byte framing `[CRC32: 4B][Length: 4B]` and binary record payload.
   - Durability controls: configurable `fsync` per write or buffered OS page cache flushes.
4. **Sorted String Tables (SSTables)** (`src/sstable_builder.*`, `src/sstable_reader.*`):
   - Data blocks with 16-key prefix restart compression and binary search lookup.
   - Sparse index block pointing to data block handles `(offset, size)`.
   - MurmurHash2 Bloom filter block (10 bits/key, ~1% false positive rate) minimizing disk block reads.
   - Fixed-size 48-byte footer with magic checksum verification.
5. **Manifest & VersionSet** (`src/manifest.*`):
   - Two-phase metadata management recording state changes via `VersionEdit`.
   - Atomic pointer file updates via `CURRENT` temporary rename.
6. **Leveled Background Compaction** (`src/compaction.*`):
   - Multi-way sorted merge using a min-heap across overlapping key ranges.
   - Obsolete version deduplication and bottom-level tombstone elimination.

---

## Directory Layout

```
.
├── CMakeLists.txt
├── README.md
├── include/
│   └── lsmkv/
│       └── db.h                  # Public client API
├── src/
│   ├── bloom.h / bloom.cpp       # Bloom filter policy
│   ├── coding.h / coding.cpp     # Varint & fixed integer binary coding
│   ├── compaction.h / .cpp       # Multi-level sorted merge compaction
│   ├── crc32.h / crc32.cpp       # IEEE 802.3 CRC32 implementation
│   ├── db.cpp / db_impl.h        # Storage engine orchestration & merged scan
│   ├── format.h / format.cpp     # Internal key format & comparator
│   ├── manifest.h / manifest.cpp # Manifest & VersionSet state tracking
│   ├── memtable.h / memtable.cpp # MemTable wrapper
│   ├── skiplist.h                # Concurrent lock-free read skip list
│   ├── sstable_builder.h / .cpp  # SSTable binary block encoder
│   ├── sstable_reader.h / .cpp   # SSTable reader with index & filter caching
│   └── wal.h / wal.cpp           # Write-Ahead Log writer & reader
├── tests/
│   ├── test_main.cpp             # Test runner
│   ├── test_skiplist.cpp         # Skip list unit tests
│   ├── test_wal.cpp              # WAL framing, CRC, and replay tests
│   ├── test_sstable.cpp          # SSTable block index and bloom tests
│   ├── test_recovery.cpp         # Process crash and WAL replay tests
│   ├── test_compaction.cpp       # Multi-run compaction & range scan tests
│   └── test_concurrency.cpp      # Concurrent readers and writers stress tests
└── benchmarks/
    └── bench_main.cpp            # Microbenchmark suite (latency percentiles)
```

---

## Building and Testing

### Prerequisites
- C++20 compliant compiler (`clang++` or `g++`)
- CMake 3.20+

### Build Instructions
```bash
mkdir -p build && cd build
cmake ..
make -j4
```

### Running Test Suite
```bash
./lsmkv_tests
```
Test categories verified:
- `SkipListBasic`: In-order iteration, binary comparisons, seek validity.
- `WalAppendAndRead`: Record serialization, framing, and corruption detection.
- `SSTableBuildAndRead`: Data block packing, Bloom filter matching, binary search.
- `CrashRecovery`: Crash injection recovery after 2000 random writes and deletes.
- `CompactionAndRangeScan`: Multi-level compaction deduplication and full range scan.
- `ConcurrentReadersAndWriters`: Multi-threaded write throughput concurrent with active reads.

---

## Benchmarks

Microbenchmark on Apple Silicon (50,000 operations, 100-byte value payload):

| Workload | Throughput | p50 Latency | p95 Latency | p99 Latency |
| :--- | :--- | :--- | :--- | :--- |
| **Sequential PUT** | **211,955 ops/s** | 4.33 µs | 6.17 µs | 9.71 µs |
| **Random PUT** | **165,275 ops/s** | 4.75 µs | 6.58 µs | 9.54 µs |
| **Random GET (Hits)** | **520,659 ops/s** | 0.92 µs | 3.75 µs | 19.50 µs |
| **Random GET (Misses)** | **1,865,074 ops/s** | 0.42 µs | 0.50 µs | 0.96 µs |
| **Range SCAN (All Keys)** | **1,069,032 ops/s** | - | - | - |

> Bloom filters eliminate unnecessary disk reads on non-existent keys, delivering **1.86M ops/sec** (~0.42 µs p50) on negative lookups.
