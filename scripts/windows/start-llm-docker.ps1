# Windows start script for Catch-the-letter with Docker Ollama
# Usage: .\scripts\windows\start-llm-docker.ps1

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $ScriptDir)
Set-Location $ProjectRoot

Write-Host "=== Starting Catch-the-letter with Docker Ollama ===" -ForegroundColor Green
Write-Host ""
Write-Host "Setting LLM_ENDPOINT to http://ollama:11434/api/chat" -ForegroundColor Cyan

# Set environment variable for this session
$env:LLM_ENDPOINT = "http://ollama:11434/api/chat"

Write-Host ""
Write-Host "Starting Docker compose with llm profile..." -ForegroundColor Cyan
Write-Host ""

docker compose -f docker-compose.yml -f docker-compose.windows.yml --profile llm up --build

Write-Host ""
Write-Host "=== Stopped ===" -ForegroundColor Yellow
