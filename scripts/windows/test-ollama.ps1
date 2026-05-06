# Windows test script for Ollama
# Tests native Ollama and Docker connectivity
# Usage: .\scripts\windows\test-ollama.ps1

Write-Host "=== Testing Ollama ===" -ForegroundColor Green
Write-Host ""

# Test native Ollama
Write-Host "1. Native Ollama (Windows localhost):" -ForegroundColor Cyan
$NativeOllamaUrl = "http://127.0.0.1:11434/api/tags"

try {
    Write-Host "   Connecting to $NativeOllamaUrl..." -ForegroundColor Gray
    $response = Invoke-RestMethod -Uri $NativeOllamaUrl -TimeoutSec 5 -ErrorAction Stop
    Write-Host "   [OK] Native Ollama is reachable" -ForegroundColor Green
    if ($response.models) {
        Write-Host "   Models available:" -ForegroundColor Gray
        $response.models | ForEach-Object { Write-Host "     - $($_.name)" -ForegroundColor Gray }
    }
}
catch {
    Write-Host "   [ERROR] Native Ollama not reachable: $_" -ForegroundColor Red
}

Write-Host ""
Write-Host "2. Docker access to host.docker.internal (Windows native Ollama from container):" -ForegroundColor Cyan

try {
    Write-Host "   Running docker test..." -ForegroundColor Gray
    $result = docker run --rm curlimages/curl:latest -m 10 http://host.docker.internal:11434/api/tags 2>&1
    if ($LASTEXITCODE -eq 0) {
        Write-Host "   [OK] Docker can reach host.docker.internal:11434" -ForegroundColor Green
    }
    else {
        Write-Host "   [WARNING] Docker curl returned: $result" -ForegroundColor Yellow
    }
}
catch {
    Write-Host "   [ERROR] $_" -ForegroundColor Red
}

Write-Host ""
Write-Host "3. Ollama chat test (if model available):" -ForegroundColor Cyan

$chatBody = @{
    model = "qwen3:4b"
    stream = $false
    messages = @(@{ 
        role = "user"
        content = 'Return JSON only: {"ok":true}'
    })
} | ConvertTo-Json -Depth 5

try {
    Write-Host "   Sending chat request to native Ollama..." -ForegroundColor Gray
    $response = Invoke-RestMethod `
        -Uri "http://127.0.0.1:11434/api/chat" `
        -Method Post `
        -ContentType "application/json" `
        -Body $chatBody `
        -TimeoutSec 60 `
        -ErrorAction Stop
    
    Write-Host "   [OK] Chat request successful" -ForegroundColor Green
    Write-Host "   Model: $($response.model)" -ForegroundColor Gray
    Write-Host "   Response: $($response.message.content)" -ForegroundColor Gray
}
catch {
    Write-Host "   [ERROR] Chat request failed: $_" -ForegroundColor Red
}

Write-Host ""
Write-Host "Done!" -ForegroundColor Green
