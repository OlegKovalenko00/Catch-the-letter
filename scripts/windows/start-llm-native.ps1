# Windows start script for Catch-the-letter with native Ollama
# Ollama must be installed and running on Windows
# Usage: .\scripts\windows\start-llm-native.ps1

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $ScriptDir)
Set-Location $ProjectRoot

Write-Host "=== Starting Catch-the-letter with native Windows Ollama ===" -ForegroundColor Green
Write-Host ""

# Check if Ollama is reachable
$OllamaUrl = "http://127.0.0.1:11434/api/tags"
Write-Host "Checking native Ollama at $OllamaUrl..." -ForegroundColor Cyan

try {
    $response = Invoke-RestMethod -Uri $OllamaUrl -TimeoutSec 3 -ErrorAction Stop
    Write-Host "[OK] Ollama is reachable" -ForegroundColor Green
}
catch {
    Write-Host "[ERROR] Ollama is not reachable at $OllamaUrl" -ForegroundColor Red
    Write-Host ""
    Write-Host "Solutions:" -ForegroundColor Yellow
    Write-Host "1. Install Ollama from https://ollama.ai"
    Write-Host "2. Start Ollama application on Windows"
    Write-Host "3. Run: ollama pull qwen3:4b"
    Write-Host "4. Retry this script"
    Write-Host ""
    exit 1
}

Write-Host ""
Write-Host "Ensuring .env has correct LLM_ENDPOINT..." -ForegroundColor Cyan

# Check if .env exists
if (-Not (Test-Path ".env")) {
    Write-Host "[ERROR] .env not found. Run .\scripts\windows\setup.ps1 first" -ForegroundColor Red
    exit 1
}

Write-Host "Starting Docker compose..." -ForegroundColor Cyan
Write-Host ""

docker compose -f docker-compose.yml -f docker-compose.windows.yml up --build

Write-Host ""
Write-Host "=== Stopped ===" -ForegroundColor Yellow
