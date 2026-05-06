#!/bin/bash
# Linux stop script for Catch-the-letter
# Stops all Docker containers
# Usage: ./scripts/linux/stop.sh

echo "=== Stopping Catch-the-letter ===" 

docker compose down
docker compose -f docker-compose.yml -f docker-compose.vpn.yml --profile llm down

echo "=== Stopped ===" 
