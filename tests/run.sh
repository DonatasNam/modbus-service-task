#!/usr/bin/env bash
# The gate's and the bench's build and test entry: release and sanitizer builds, then their unit tests.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
jobs="$(nproc)"

build_and_test() {
    local dir="$1"
    shift
    cmake -S . -B "$dir" "$@"
    cmake --build "$dir" --clean-first -j "$jobs"
    ctest --test-dir "$dir" --output-on-failure --timeout 60
}

build_and_test build/release -DCMAKE_BUILD_TYPE=Release
build_and_test build/sanitize -DCMAKE_BUILD_TYPE=Debug -DMBP_SANITIZE=ON

echo "TESTS PASS"
