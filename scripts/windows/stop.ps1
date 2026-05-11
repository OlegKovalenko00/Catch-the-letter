



Write-Host "=== Stopping Catch-the-letter ===" -ForegroundColor Green

docker compose -f docker-compose.windows-standalone.yml down
docker compose -f docker-compose.windows-standalone.yml --profile llm down

Write-Host "=== Stopped ===" -ForegroundColor Green
