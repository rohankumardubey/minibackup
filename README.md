# minibackup – tiny, deduplicating backup in C++ / C / Rust

This repo contains three minimal implementations of a content‑addressed backup tool:

- **C++** (`minibackup/minibackup.cpp`) – fast, multithreaded
- **C (pthreads)** (`minibackup_c/minibackup.c`) – fast, multithreaded
- **Rust** (`minibackup-rs/`) – multithreaded

All three use the **same on‑disk format**, so you can back up with one and restore with another.

> ⚠️ **Educational prototype** — not hardened for production, no encryption/authentication, no retention or GC.

---

## On‑disk format (shared)

- Fixed‑size **1 MiB** chunks
- Chunk key: **SHA‑256** of the raw chunk (hex)
- Chunk compression: **Zstandard (zstd)** level 1
- Repository layout:

```
REPO/
  chunks/
    00/00...hash.zst
    01/01...hash.zst
    ...
  manifests/
    20250101-123456.json    # per-snapshot manifest
```

- Manifests are **JSON** with per‑file chunk lists and basic metadata (mode, size, mtime).
- First two hex chars of the hash are used as the subdirectory prefix (00–ff).

---

## Repository layout

```
.
├─ bench.sh                 # builds C++/C/Rust and runs the benchmark
├─ minibackup/
│  └─ minibackup.cpp        # C++ implementation
├─ minibackup_c/
│  └─ minibackup.c          # C implementation (pthreads)
└─ minibackup-rs/
   ├─ Cargo.toml
   └─ src/main.rs           # Rust implementation
```

---

## Quick start

### macOS (Homebrew)

```bash
brew install openssl@3 zstd jansson nlohmann-json
# Build & run benchmark (this compiles all 3 and runs v1/v2 tests):
bash ./bench.sh
```

### Ubuntu/Debian

```bash
sudo apt-get update
sudo apt-get install -y build-essential pkg-config \
  libssl-dev libzstd-dev libjansson-dev nlohmann-json3-dev \
  curl git
# Rust toolchain if needed:
curl https://sh.rustup.rs -sSf | sh -s -- -y
source "$HOME/.cargo/env"

# Build & run benchmark:
bash ./bench.sh
```

The script will:

1. **Build** C++, C, and Rust binaries with native optimizations.
2. **Generate** a dataset (`bench_workspace/data_v1`) and a slightly mutated copy (`data_v2`).
3. **Run** backups for each implementation against both datasets.
4. **Print** a table like:

```
[bench] Benchmarking (best of 2)
impl     phase  seconds      bytes        MiB/s        dedupe
rust     v1     8.97         8589934592   913.27       100.0%
cpp      v1     2.83         8589934592   2894.70      100.0%
c        v1     2.90         8589934592   2824.83      100.0%
rust     v2     7.85         8589934592   1043.57      100.0%
cpp      v2     2.67         8589934592   3068.16      99.9%
c        v2     2.79         8589934592   2936.20      99.9%
```

- **phase `v1`** = first backup (cold repository)
- **phase `v2`** = second backup of a slightly changed copy (shows dedupe)
- **MiB/s** computed from total bytes / elapsed seconds
- **dedupe** = `reused / (stored + reused)` from program output

---

## Benchmark details

`bench.sh` creates a controlled dataset:

- **`data_v1`**: 
  - `FILES_COMP` zero‑filled files (highly compressible) and 
  - `FILES_RAND` pseudo‑random files (via `openssl aes-256-ctr` over zeros; fast and deterministic).
- **`data_v2`**: clone of `v1`, but **1 MiB** in the middle of each file is mutated — so chunk dedupe should be very high on `v2`.

### Tuning knobs (env vars)

```bash
# Size of each file (MiB)
export FILE_SIZE_MB=1024

# Number of compressible & pseudo-random files
export FILES_COMP=4
export FILES_RAND=4

# Repetitions per phase (best time is kept)
export RUNS=3
```

You can also override binary paths if you’re testing variants:

```bash
export CPP_BIN=/path/to/custom/cpp/minibackup
export C_BIN=/path/to/custom/c/minibackup_c
export RS_BIN=/path/to/custom/rust/minibackup-rs
bash ./bench.sh
```

---

## Manual builds (optional)

> Normally **not required** — `bench.sh` compiles everything for you.  
> These commands are for reference/CI.

### C++

```bash
# macOS/Homebrew
OPENSSL_PREFIX="$(brew --prefix openssl@3)"
ZSTD_PREFIX="$(brew --prefix zstd)"
JSON_PREFIX="$(brew --prefix nlohmann-json)"  # header-only

c++ -std=c++17 minibackup/minibackup.cpp -o minibackup/minibackup \
  -O3 -march=native -flto -DNDEBUG -pthread \
  -I"$OPENSSL_PREFIX/include" -I"$ZSTD_PREFIX/include" -I"$JSON_PREFIX/include" \
  -L"$OPENSSL_PREFIX/lib" -L"$ZSTD_PREFIX/lib" \
  -lcrypto -lzstd
```

### C (pthreads)

```bash
# macOS/Homebrew
OPENSSL_PREFIX="$(brew --prefix openssl@3)"
ZSTD_PREFIX="$(brew --prefix zstd)"
JANSSON_PREFIX="$(brew --prefix jansson)"

cc -std=c11 minibackup_c/minibackup.c -o minibackup_c/minibackup_c \
  -O3 -march=native -flto -DNDEBUG -pthread -D_DARWIN_C_SOURCE \
  -I"$OPENSSL_PREFIX/include" -I"$ZSTD_PREFIX/include" -I"$JANSSON_PREFIX/include" \
  -L"$OPENSSL_PREFIX/lib" -L"$ZSTD_PREFIX/lib" -L"$JANSSON_PREFIX/lib" \
  -lcrypto -lzstd -ljansson
```

### Rust

```bash
cd minibackup-rs
RUSTFLAGS="-C target-cpu=native" cargo build --release
# Binary at: minibackup-rs/target/release/minibackup-rs
```

---

## CLI usage (all three implementations)

```
backup  --src <path> --repo <repo-dir>
list    --repo <repo-dir>
restore --repo <repo-dir> --snapshot <id> --dst <out-dir>
verify  --repo <repo-dir> --snapshot <id>
```

Examples:

```bash
# C++
./minibackup/minibackup backup --src ~/data --repo ~/backups/minirepo
./minibackup/minibackup list --repo ~/backups/minirepo
./minibackup/minibackup restore --repo ~/backups/minirepo --snapshot 20250101-123456 --dst ./restore
./minibackup/minibackup verify  --repo ~/backups/minirepo --snapshot 20250101-123456

# C
./minibackup_c/minibackup_c backup --src ~/data --repo ~/backups/minirepo

# Rust
./minibackup-rs/target/release/minibackup-rs backup --src ~/data --repo ~/backups/minirepo
```

---

## Implementation notes

- **Chunking**: fixed 1 MiB (simple and predictable).
- **Hash**: SHA‑256; only hex‑encode when forming filenames. First two hex chars are used to shard chunk directories (`00/..`, `ff/..`).
- **Compression**: Zstd level 1. C and C++ reuse a **compression context per worker** and a single output buffer to avoid per‑chunk setup/allocs.
- **Parallelism**:
  - **C++**: file‑level thread pool.
  - **C**: pthreads, file‑level, plus simple `.lock` files to avoid duplicate compression on chunk races.
  - **Rust**: file‑level pool; dedupe index is striped to reduce contention (see code).
- **Restore**: reads the manifest, concatenates decompressed chunks, restores mode and mtime.

---

## Troubleshooting

- **Linker can’t find OpenSSL/Zstd/Jansson (macOS)**  
  Ensure Homebrew packages are installed:
  ```bash
  brew install openssl@3 zstd jansson nlohmann-json
  ```
  The build step in `bench.sh` queries `brew --prefix` and passes include/lib paths automatically.

- **`unknown type name 'u_int'` / macOS sysctl errors (C)**  
  We compile with `-D_DARWIN_C_SOURCE` so BSD types are visible. If compiling manually, add that define.

- **`n/a` rows in the table**  
  The script treats a run as successful only if the program exits with code 0 **and** prints a summary line with `stored=`/`reused=`. Otherwise it prints `n/a` and the last few lines of stderr to help debug.

- **Cloud‑synced folders (OneDrive/iCloud)**  
  Some synced folders restrict certain syscalls. If you see permission errors, try a local folder outside those trees (e.g., `~/tmp`).

---

## Roadmap / ideas

- Content‑defined chunking (rolling hash) for better dedupe on inserts.
- AEAD encryption per chunk; repository key management.
- Parallel restore & verify.
- Retention policies + garbage collection.
- Compact binary manifests or SQLite side index.
- Cross‑platform CI (macOS + Ubuntu).

---

## License

Add a `LICENSE` file (MIT/Apache‑2.0/BSD‑3‑Clause are common).  
State your choice here.
