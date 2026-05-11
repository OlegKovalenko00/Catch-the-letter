#!/bin/bash


echo "=== Stopping Catch-the-letter ==="

docker compose down
docker compose -f docker-compose.yml -f docker-compose.vpn.yml --profile llm down

echo "=== Stopped ==="
