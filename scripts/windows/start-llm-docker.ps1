


$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $ScriptDir)
Set-Location $ProjectRoot

Write-Host "=== Starting Catch-the-letter with Docker Ollama ===" -ForegroundColor Green
Write-Host ""
Write-Host "Setting LLM_ENDPOINT to http://ollama:11434/api/chat" -ForegroundColor Cyan


$env:LLM_ENDPOINT = "http://ollama:11434/api/chat"

Write-Host ""
Write-Host "Starting Docker compose with llm profile and standalone config..." -ForegroundColor Cyan
Write-Host ""


docker compose -f docker-compose.windows-standalone.yml --profile llm up --build

Write-Host ""
Write-Host "=== Stopped ===" -ForegroundColor Yellow
