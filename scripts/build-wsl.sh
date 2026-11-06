#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
if [[ "$(uname -s)" != Linux ]]; then
  echo 'Run this script in Ubuntu 24.04 / WSL2.' >&2
  exit 1
fi
cmake -S backend -B backend/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++
cmake --build backend/build --parallel "${BUILD_JOBS:-2}"
ctest --test-dir backend/build --output-on-failure
python3 -m unittest discover -s python-worker/tests -v
