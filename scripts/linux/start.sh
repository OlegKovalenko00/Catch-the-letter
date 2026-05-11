#!/bin/bash


set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$(dirname "$SCRIPT_DIR/../..")" && pwd)"
cd "$PROJECT_ROOT"

echo "=== Starting Catch-the-letter on Linux ==="
echo "Mode: Docker bridge, Docker Ollama with --profile llm"
echo ""

docker compose --profile llm up --build

echo ""
echo "=== Stopped ==="
