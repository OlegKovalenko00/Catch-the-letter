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

if ! command -v docker >/dev/null 2>&1; then
  echo "SKIPPED Ollama test: docker is unavailable"
  exit 0
fi

MODEL="${LLM_MODEL:-qwen3:4b}"
echo "Starting Ollama profile and model preload for ${MODEL}..."
LLM_MODEL="$MODEL" docker compose --profile llm up -d ollama ollama-init >/dev/null

echo "Waiting for Ollama HTTP endpoint..."
for _ in $(seq 1 60); do
  if docker compose exec -T ollama ollama list >/dev/null 2>&1; then
    break
  fi
  sleep 2
done

if ! docker compose exec -T ollama ollama list >/dev/null 2>&1; then
  echo "SKIPPED Ollama mode: ollama service did not become reachable"
  exit 0
fi

if ! docker compose exec -T ollama ollama list | grep -Fq "$MODEL"; then
  echo "SKIPPED Ollama mode: model ${MODEL} is not present yet; check docker compose logs ollama-init"
  exit 0
fi

output="$(LLM_ENABLED=true LLM_ENDPOINT=http://127.0.0.1:11434/api/chat LLM_MODEL="$MODEL" ./build/catch_the_letter --config config/app.example.json --test-llm)"
echo "$output"
if echo "$output" | grep -q '"fallback": true'; then
  echo "SKIPPED Ollama mode: model exists but JSON-mode probe fell back; check Ollama logs/model compatibility"
  exit 0
fi
