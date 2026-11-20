#!/bin/bash
# builds the vpk inside WSL. usage: ./build.sh [clean]
set -e
export VITASDK=${VITASDK:-/usr/local/vitasdk}
export PATH="$VITASDK/bin:$PATH"
cd "$(dirname "$0")"

if [ "$1" = "clean" ]; then rm -rf build; fi
mkdir -p build
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo > /dev/null
cmake --build build -j"$(nproc)"
ls -la build/*.vpk
