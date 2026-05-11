


$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $ScriptDir)
Set-Location $ProjectRoot

Write-Host "=== Starting Catch-the-letter on Windows ===" -ForegroundColor Green
Write-Host "Mode: Docker bridge, no Docker Ollama profile"
Write-Host ""
Write-Host "Note: To use Ollama, configure LLM_ENDPOINT in .env:"
Write-Host "  - Native Ollama: LLM_ENDPOINT=http://host.docker.internal:11434/api/chat"
Write-Host "  - Docker Ollama: run .\scripts\windows\start-llm-docker.ps1"
Write-Host ""


docker compose -f docker-compose.windows-standalone.yml up --build

Write-Host ""
Write-Host "=== Stopped ===" -ForegroundColor Yellow

