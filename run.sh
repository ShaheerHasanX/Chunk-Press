#!/usr/bin/env bash
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build="$root/build"

for tool in cmake g++; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        printf 'Install the Ubuntu build packages listed in README.md before running this script.\n' >&2
        exit 1
    fi
done

cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release -DCHUNKPRESS_GUI=ON
cmake --build "$build" --parallel 4

if [[ ${1:-} == --cli ]]; then
    shift
    exec "$build/chunkpress-cli" "$@"
fi
exec "$build/chunkpress" "$@"
