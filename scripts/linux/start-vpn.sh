#!/bin/bash
# Linux start script for Catch-the-letter (VPN/proxy mode)
# Usage: ./scripts/linux/start-vpn.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$(dirname "$SCRIPT_DIR/../..")" && pwd)"
cd "$PROJECT_ROOT"

echo "=== Starting Catch-the-letter on Linux (VPN/proxy mode) ===" 
echo "Mode: host network, Ollama on localhost"
echo ""
echo "This mode requires:"
echo "  - Telegram proxy on 127.0.0.1:10809 (if needed)"
echo "  - Ollama running on 127.0.0.1:11434"
echo ""

docker compose -f docker-compose.yml -f docker-compose.vpn.yml --profile llm up --build

echo ""
echo "=== Stopped ===" 
