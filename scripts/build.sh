#!/usr/bin/env bash
set -euo pipefail

bash scripts/fetch_deps.sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2

echo
echo "Built:"
file build/rehlds_netdiag_mm_i386.so
ls -lh build/rehlds_netdiag_mm_i386.so
