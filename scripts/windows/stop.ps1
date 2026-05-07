# Windows stop script for Catch-the-letter
# Stops all Docker containers
# Usage: .\scripts\windows\stop.ps1

Write-Host "=== Stopping Catch-the-letter ===" -ForegroundColor Green

docker compose -f docker-compose.windows-standalone.yml down
docker compose -f docker-compose.windows-standalone.yml --profile llm down

Write-Host "=== Stopped ===" -ForegroundColor Green
