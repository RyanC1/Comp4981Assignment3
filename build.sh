#!/usr/bin/env bash
# build.sh — build the last configured CMake build directory
# Supports: macOS, modern Linux, modern FreeBSD
set -euo pipefail

# ----------------- defaults -----------------
jobs="${JOBS:-${CMAKE_BUILD_PARALLEL_LEVEL:-}}"
target=""
build_dir=""

usage() {
  echo "Usage: $0 [-j N] [-t <target>]"
  echo "  -j N        Parallel build with N jobs (or set JOBS / CMAKE_BUILD_PARALLEL_LEVEL)"
  echo "  -t target   Build a specific target (e.g. -t main)"
  exit 1
}

# ----------------- parse options -----------------
while getopts ":j:t:h" opt; do
  case "$opt" in
    j) jobs="$OPTARG" ;;
    t) target="$OPTARG" ;;
    h|*) usage ;;
  esac
done

# ----------------- determine build dir -----------------
# Preferred: read the last configured build dir written by change-compiler.sh
if [[ -f ".last-build-dir" ]]; then
  build_dir="$(< .last-build-dir)"
else
  # Fallback for legacy/manual setups
  build_dir="build"
fi

# ----------------- sanity checks -----------------
if [[ ! -d "$build_dir" || ! -f "$build_dir/CMakeCache.txt" ]]; then
  echo "Error: build directory '$build_dir' is not configured." >&2
  echo "Run ./change-compiler.sh first." >&2
  exit 1
fi

# ----------------- assemble build command -----------------
cmd=(cmake --build "$build_dir" --clean-first --verbose)
[[ -n "$target" ]] && cmd+=(--target "$target")
[[ -n "$jobs" ]] && cmd+=(--parallel "$jobs")

# ----------------- run -----------------
echo "Using build directory: $build_dir"
echo "Running: ${cmd[*]}"
exec "${cmd[@]}"
