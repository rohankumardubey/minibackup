#!/usr/bin/env bash
# Build C++/C/Rust (from this repo layout) and benchmark them.

set -euo pipefail
IFS=$'\n\t'

# ------------------------------ paths ------------------------------
ROOT="${ROOT:-$PWD/bench_workspace}"

CPP_SRC="$PWD/minibackup/minibackup.cpp"
CPP_BIN="${CPP_BIN:-$PWD/minibackup/minibackup}"

C_SRC="$PWD/minibackup_c/minibackup.c"
C_BIN="${C_BIN:-$PWD/minibackup_c/minibackup_c}"

RS_DIR="$PWD/minibackup-rs"
RS_BIN="${RS_BIN:-$RS_DIR/target/release/minibackup-rs}"

FILES_COMP="${FILES_COMP:-2}"
FILES_RAND="${FILES_RAND:-2}"
FILE_SIZE_MB="${FILE_SIZE_MB:-256}"
RUNS="${RUNS:-2}"

DATA_V1="$ROOT/data_v1"
DATA_V2="$ROOT/data_v2"
REPO_CPP="$ROOT/repo_cpp"
REPO_RS="$ROOT/repo_rs"
REPO_C="$ROOT/repo_c"

CC_BIN="${CC_BIN:-cc}"
CXX_BIN="${CXX_BIN:-c++}"

log()  { printf '\033[1;34m[bench]\033[0m %s\n' "$*"; }
have() { command -v "$1" >/dev/null 2>&1; }

# ------------------------------ build steps ------------------------------
build_cpp() {
  log "Building C++ → $CPP_BIN"
  [[ -f "$CPP_SRC" ]] || { echo "missing: $CPP_SRC"; exit 1; }

  local os; os="$(uname)"
  local -a CXXFLAGS=( -std=c++17 -O3 -march=native -flto -DNDEBUG -pthread )
  local -a INC=()
  local -a LIBS=( -lcrypto -lzstd )

  if [[ "$os" == "Darwin" ]]; then
    local OPENSSL_PREFIX="$(brew --prefix openssl@3 2>/dev/null || true)"
    local ZSTD_PREFIX="$(brew --prefix zstd 2>/dev/null || true)"
    local JSON_PREFIX="$(brew --prefix nlohmann-json 2>/dev/null || true)"  # header-only

    [[ -n "$OPENSSL_PREFIX" ]] && INC+=( -I"$OPENSSL_PREFIX/include" ) && LIBS+=( -L"$OPENSSL_PREFIX/lib" )
    [[ -n "$ZSTD_PREFIX"    ]] && INC+=( -I"$ZSTD_PREFIX/include"    ) && LIBS+=( -L"$ZSTD_PREFIX/lib" )
    [[ -n "$JSON_PREFIX"    ]] && INC+=( -I"$JSON_PREFIX/include"    )
  else
    if have pkg-config; then
      local s
      s="$(pkg-config --cflags openssl libzstd nlohmann_json 2>/dev/null || true)"; if [[ -n "$s" ]]; then
        local oldIFS="$IFS"; IFS=' '; read -r -a tmp <<<"$s"; IFS="$oldIFS"; INC+=( "${tmp[@]}" )
      fi
      s="$(pkg-config --libs openssl libzstd nlohmann_json 2>/dev/null || true)"; if [[ -n "$s" ]]; then
        local oldIFS="$IFS"; IFS=' '; read -r -a tmp2 <<<"$s"; IFS="$oldIFS"; LIBS+=( "${tmp2[@]}" )
      fi
    fi
  fi

  mkdir -p "$(dirname "$CPP_BIN")"
  "$CXX_BIN" "${CXXFLAGS[@]}" "${INC[@]}" "$CPP_SRC" -o "$CPP_BIN" "${LIBS[@]}"
}

build_c() {
  log "Building C → $C_BIN"
  [[ -f "$C_SRC" ]] || { echo "missing: $C_SRC"; exit 1; }

  local os; os="$(uname)"
  local -a CFLAGS=( -std=c11 -O3 -march=native -flto -DNDEBUG -pthread )
  local -a INC=()
  local -a LIBS=( -lcrypto -lzstd -ljansson )

  if [[ "$os" == "Darwin" ]]; then
    local OPENSSL_PREFIX="$(brew --prefix openssl@3 2>/dev/null || true)"
    local ZSTD_PREFIX="$(brew --prefix zstd 2>/dev/null || true)"
    local JANSSON_PREFIX="$(brew --prefix jansson 2>/dev/null || true)"
    CFLAGS+=( -D_DARWIN_C_SOURCE )

    [[ -n "$OPENSSL_PREFIX" ]] && INC+=( -I"$OPENSSL_PREFIX/include" ) && LIBS+=( -L"$OPENSSL_PREFIX/lib" )
    [[ -n "$ZSTD_PREFIX"    ]] && INC+=( -I"$ZSTD_PREFIX/include"    ) && LIBS+=( -L"$ZSTD_PREFIX/lib" )
    [[ -n "$JANSSON_PREFIX" ]] && INC+=( -I"$JANSSON_PREFIX/include" ) && LIBS+=( -L"$JANSSON_PREFIX/lib" )
  else
    if have pkg-config; then
      local s
      s="$(pkg-config --cflags openssl libzstd jansson 2>/dev/null || true)"; if [[ -n "$s" ]]; then
        local oldIFS="$IFS"; IFS=' '; read -r -a tmp <<<"$s"; IFS="$oldIFS"; INC+=( "${tmp[@]}" )
      fi
      s="$(pkg-config --libs openssl libzstd jansson 2>/dev/null || true)"; if [[ -n "$s" ]]; then
        local oldIFS="$IFS"; IFS=' '; read -r -a tmp2 <<<"$s"; IFS="$oldIFS"; LIBS+=( "${tmp2[@]}" )
      fi
    fi
  fi

  mkdir -p "$(dirname "$C_BIN")"
  "$CC_BIN" "${CFLAGS[@]}" "${INC[@]}" "$C_SRC" -o "$C_BIN" "${LIBS[@]}"
}

build_rs() {
  log "Building Rust → $RS_BIN"
  [[ -d "$RS_DIR" ]] || { echo "missing: $RS_DIR"; exit 1; }
  ( cd "$RS_DIR" && RUSTFLAGS="${RUSTFLAGS:-"-C target-cpu=native"}" cargo build --release )
}

# --------------------------- dataset generators -----------------------------
filesize() { if [[ "$(uname)" == "Darwin" ]]; then stat -f%z "$1"; else stat -c%s "$1"; fi; }

gen_compressible() { dd if=/dev/zero of="$1" bs=1m count="$2" status=none; }

gen_pseudorand() {
  local outfile="$1"; local size_mb="$2"; local seed="$3"
  dd if=/dev/zero bs=1m count="$size_mb" status=none \
    | openssl enc -aes-256-ctr -pbkdf2 -pass pass:"$seed" -nosalt > "$outfile"
}

clone_tree() {
  local src="$1" dst="$2"
  mkdir -p "$dst"
  if cp -cR "$src/." "$dst/" 2>/dev/null; then return 0; fi
  if have ditto; then ditto "$src" "$dst"; return 0; fi
  (cd "$src" && tar -cf - .) | (cd "$dst" && tar -xf -)
}

gen_dataset() {
  rm -rf "$ROOT"
  mkdir -p "$DATA_V1" "$DATA_V2" "$REPO_CPP" "$REPO_RS" "$REPO_C"

  local total_mb=$(( (FILES_COMP + FILES_RAND) * FILE_SIZE_MB ))
  log "Generating dataset v1 in '$DATA_V1' (~${total_mb} MiB, non-parallel)"

  for ((i=1; i<=FILES_COMP; i++)); do
    local f="$DATA_V1/comp_${i}.bin"
    log "  -> creating $(basename "$f") (${FILE_SIZE_MB} MiB zeros)"
    local t0=$(date +%s); gen_compressible "$f" "$FILE_SIZE_MB"; local t1=$(date +%s)
    log "     done size=$(filesize "$f") bytes elapsed=$((t1-t0))s"
  done

  for ((i=1; i<=FILES_RAND; i++)); do
    local f="$DATA_V1/rand_${i}.bin"
    log "  -> creating $(basename "$f") (${FILE_SIZE_MB} MiB AES-CTR pseudo-random)"
    local t0=$(date +%s); gen_pseudorand "$f" "$FILE_SIZE_MB" "$i"; local t1=$(date +%s)
    log "     done size=$(filesize "$f") bytes elapsed=$((t1-t0))s"
  done

  log "Cloning v1 -> v2"
  clone_tree "$DATA_V1" "$DATA_V2"

  log "Mutating 1 MiB at mid-file in each v2 file to show dedupe"
  for f in "$DATA_V2"/*.bin; do
    local mid=$(( FILE_SIZE_MB / 2 ))
    local patch="$ROOT/_patch_$(basename "$f")"
    gen_pseudorand "$patch" 1 "$(basename "$f")_patch"
    dd if="$patch" of="$f" bs=1m seek="$mid" conv=notrunc status=none
    rm -f "$patch"
  done
}

# ------------------------------ helpers -------------------------------------
dir_bytes() {
  local d="$1"
  if [[ "$(uname)" == "Darwin" ]]; then
    find "$d" -type f -exec stat -f %z {} \; | awk '{s+=$1} END{print s+0}'
  else
    find "$d" -type f -exec stat -c %s {} \; | awk '{s+=$1} END{print s+0}'
  fi
}
is_number() { local v="$1"; [[ "$v" =~ ^[0-9]+([.][0-9]+)?$ ]]; }

run_once() {
  local bin="$1"; local src="$2"; local repo="$3"
  local out err; out="$(mktemp)"; err="$(mktemp)"

  local start_ts end_ts real_s="n/a"
  if have python3; then
    start_ts="$(python3 - <<'PY'
import time; print(f"{time.time():.6f}")
PY
)"
  fi

  "$bin" backup --src "$src" --repo "$repo" >"$out" 2>"$err"
  local ec=$?

  if have python3; then
    end_ts="$(python3 - <<'PY'
import time; print(f"{time.time():.6f}")
PY
)"
    real_s="$(python3 - <<PY
s=float("${start_ts}"); e=float("${end_ts}"); print(f"{max(e-s,0.0):.2f}")
PY
)"
  else
    real_s="$(grep '^real ' "$err" | awk '{print $2}' | tail -1)"
    [[ -z "${real_s:-}" ]] && real_s="n/a"
  fi

  local ok="0"
  if [[ $ec -eq 0 ]] && grep -Eq 'stored=[0-9]+' "$out"; then ok="1"; fi

  local bytes stored reused mbps dedupe
  if [[ "$ok" == "1" ]]; then
    bytes="$(grep -Eo 'Bytes:[[:space:]]*[0-9]+' "$out" | awk '{print $2}' | tail -1)"
    stored="$(grep -Eo 'stored=[0-9]+' "$out" | tail -1 | cut -d= -f2)"
    reused="$(grep -Eo 'reused=[0-9]+' "$out" | tail -1 | cut -d= -f2)"
  fi
  [[ -z "${bytes:-}" ]] && bytes="$(dir_bytes "$src")"

  if [[ "$ok" == "1" ]] && [[ "$real_s" =~ ^[0-9]+([.][0-9]+)?$ ]] && awk "BEGIN{exit !($real_s>0)}"; then
    mbps="$(awk -v b="$bytes" -v t="$real_s" 'BEGIN{printf "%.2f", (b/1048576)/t}')"
  else
    real_s="n/a"; mbps="n/a"
  fi

  if [[ "$ok" == "1" ]] && [[ "$stored" =~ ^[0-9]+$ ]] && [[ "$reused" =~ ^[0-9]+$ ]]; then
    dedupe="$(awk -v s="$stored" -v r="$reused" 'BEGIN{ if(s+r>0) printf "%.1f%%", (r*100.0)/(s+r); else print "0.0%"; }')"
  else
    dedupe="n/a"
  fi

  if [[ "$ok" != "1" && -s "$err" ]]; then
    echo "[warn] $bin failed or printed no summary; stderr:" >&2
    tail -n 5 "$err" >&2
  fi

  rm -f "$out" "$err"
  printf '%s|%s|%s|%s|%s\n' "${real_s}" "${bytes}" "${mbps}" "${stored:-n/a}" "${dedupe}"
}

best_of() {
  local label="$1"; local bin="$2"; local src="$3"; local repo="$4"
  local best_line="" best_secs=""
  for ((i=1; i<=RUNS; i++)); do
    local line secs; line="$(run_once "$bin" "$src" "$repo")"
    secs="$(cut -d'|' -f1 <<<"$line")"
    if [[ "$secs" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
      if [[ -z "$best_line" ]]; then best_line="$line"; best_secs="$secs"; continue; fi
      if awk "BEGIN {exit !($secs < $best_secs)}"; then best_line="$line"; best_secs="$secs"; fi
    fi
  done
  if [[ -z "$best_line" ]]; then
    printf '%s|%s|%s|%s|%s\n' "n/a" "$(dir_bytes "$src")" "n/a" "n/a" "n/a"
  else
    echo "$best_line"
  fi
}

# ---------------------------------- main ------------------------------------
main() {
  log "Compiling all targets…"
  build_cpp
  build_c
  build_rs

  [[ -x "$CPP_BIN" ]] || { echo "C++ binary not found: $CPP_BIN"; exit 1; }
  [[ -x "$C_BIN"   ]] || { echo "C binary not found:   $C_BIN";   exit 1; }
  [[ -x "$RS_BIN"  ]] || { echo "Rust binary not found: $RS_BIN"; exit 1; }

  gen_dataset
  echo
  log "Benchmarking (best of ${RUNS})"
  printf '%-8s %-6s %-12s %-12s %-12s %-8s\n' "impl" "phase" "seconds" "bytes" "MiB/s" "dedupe"

  # v1 fresh
  rs_v1="$(best_of rust "$RS_BIN" "$DATA_V1" "$REPO_RS")"
  cpp_v1="$(best_of cpp  "$CPP_BIN" "$DATA_V1" "$REPO_CPP")"
  c_v1="$(best_of c      "$C_BIN"   "$DATA_V1" "$REPO_C")"

  # v2 incremental (same repos)
  rs_v2="$(best_of rust "$RS_BIN" "$DATA_V2" "$REPO_RS")"
  cpp_v2="$(best_of cpp  "$CPP_BIN" "$DATA_V2" "$REPO_CPP")"
  c_v2="$(best_of c      "$C_BIN"   "$DATA_V2" "$REPO_C")"

  pr() {
    local impl="$1"; local phase="$2"; local line="$3"
    local secs bytes mbps dedupe
    secs="$(cut -d'|' -f1 <<<"$line")"
    bytes="$(cut -d'|' -f2 <<<"$line")"
    mbps="$(cut -d'|' -f3 <<<"$line")"
    dedupe="$(cut -d'|' -f5 <<<"$line")"
    printf '%-8s %-6s %-12s %-12s %-12s %-8s\n' "$impl" "$phase" "$secs" "$bytes" "$mbps" "$dedupe"
  }

  pr rust v1 "$rs_v1"
  pr cpp  v1 "$cpp_v1"
  pr c    v1 "$c_v1"
  pr rust v2 "$rs_v2"
  pr cpp  v2 "$cpp_v2"
  pr c    v2 "$c_v2"

  echo
  log "Notes: v2 reuses the same repo so dedupe should be high (reused ≫ stored)."
}

main "$@"
