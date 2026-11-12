#!/usr/bin/env bash
# The gate's and the bench's build and test entry: release and sanitizer builds with their unit tests, then the SDK
# tests and the behaviour suite against both binaries. Prints TESTS PASS only when everything passed.
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

export PYTHONPATH="$root/sdk/src"
python3 -m unittest discover -s sdk/tests -t sdk/tests
for binary in build/release/modbus-pollerd build/sanitize/modbus-pollerd; do
    echo "behaviour suite: $binary"
    MBP_BINARY="$root/$binary" python3 -m unittest discover -s tests/behaviour -t tests/behaviour -v
done

echo "TESTS PASS"
