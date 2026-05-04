#!/usr/bin/env bash
set -euo pipefail

python3 -m py_compile browser-worker/main.py

if curl -fsS http://127.0.0.1:8090/health >/tmp/catch-letter-browser-health.json; then
  cat /tmp/catch-letter-browser-health.json
else
  echo "browser-worker is not running on 127.0.0.1:8090; start docker compose or uvicorn first"
fi
