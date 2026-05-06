#!/usr/bin/env bash
set -euo pipefail

if [[ ! -x ./build/catch_the_letter ]]; then
  echo "build/catch_the_letter is missing; run scripts/smoke_build.sh first"
  exit 1
fi

./build/catch_the_letter --config config/app.example.json --test-llm

if [[ "${RUN_OLLAMA_TEST:-0}" != "1" ]]; then
  echo "SKIPPED Ollama test: set RUN_OLLAMA_TEST=1 to require a local model check"
  exit 0
fi

tmp_config="$(mktemp)"
python3 - "$tmp_config" <<'PY'
import json, sys
cfg = json.load(open("config/app.example.json", encoding="utf-8"))
cfg["llm"]["enabled"] = True
cfg["llm"]["endpoint"] = "http://127.0.0.1:11434/api/chat"
json.dump(cfg, open(sys.argv[1], "w", encoding="utf-8"), ensure_ascii=False, indent=2)
PY

if ! ./build/catch_the_letter --config "$tmp_config" --test-llm; then
  echo "SKIPPED/FAILED Ollama mode: endpoint/model is not reachable or JSON-mode failed"
  exit 1
fi
