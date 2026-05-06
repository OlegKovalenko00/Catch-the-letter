#!/usr/bin/env bash
set -euo pipefail

JOBS="${JOBS:-2}"
rm -rf build
cmake -S . -B build -DUSE_FETCHCONTENT=OFF -DUSE_SYSTEM_DEPS=OFF
cmake --build build --parallel "$JOBS"
python3 -m py_compile browser-worker/main.py
./build/catch_the_letter --config config/app.example.json --test-config
./build/catch_the_letter --config config/app.example.json --test-llm
