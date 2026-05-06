# Windows test script for Catch-the-letter
# Runs smoke tests against running app
# Usage: .\scripts\windows\test.ps1

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $ScriptDir)
Set-Location $ProjectRoot

Write-Host "=== Catch-the-letter Windows Tests ===" -ForegroundColor Green
Write-Host ""

$BaseUrl = "http://127.0.0.1:8080"
$BrowserWorkerUrl = "http://127.0.0.1:8090"
$Errors = 0

# Test function
function Test-Endpoint {
    param(
        [string]$Name,
        [string]$Url,
        [string]$Method = "Get",
        [object]$Body = $null
    )
    
    try {
        Write-Host "Testing: $Name" -ForegroundColor Cyan
        $params = @{
            Uri = $Url
            TimeoutSec = 30
            ErrorAction = "Stop"
        }
        
        if ($Method -eq "Post") {
            $params["Method"] = "Post"
        }
        
        if ($Body) {
            $params["ContentType"] = "application/json"
            $params["Body"] = $Body | ConvertTo-Json
        }
        
        $response = Invoke-RestMethod @params
        Write-Host "  [OK] Status: $($response.status ?? 'ok')" -ForegroundColor Green
        return $true
    }
    catch {
        Write-Host "  [ERROR] $_" -ForegroundColor Red
        $script:Errors++
        return $false
    }
}

# Run tests
Write-Host "API Endpoints:" -ForegroundColor Yellow
Test-Endpoint "GET /api/status" "$BaseUrl/api/status" | Out-Null
Test-Endpoint "GET /health (browser-worker)" "$BrowserWorkerUrl/health" | Out-Null
Test-Endpoint "POST /api/test/browser" "$BaseUrl/api/test/browser" "Post" | Out-Null
Test-Endpoint "POST /api/test/telegram" "$BaseUrl/api/test/telegram" "Post" | Out-Null
Test-Endpoint "POST /api/test/imap" "$BaseUrl/api/test/imap" "Post" | Out-Null
Test-Endpoint "POST /api/test/llm" "$BaseUrl/api/test/llm" "Post" | Out-Null

Write-Host ""
Write-Host "Demo flows:" -ForegroundColor Yellow
Test-Endpoint "POST /api/demo/create" "$BaseUrl/api/demo/create" "Post" | Out-Null
Test-Endpoint "POST /api/demo/create-auth" "$BaseUrl/api/demo/create-auth" "Post" | Out-Null

Write-Host ""
if ($Errors -eq 0) {
    Write-Host "=== All tests passed ===" -ForegroundColor Green
}
else {
    Write-Host "=== $Errors test(s) failed ===" -ForegroundColor Red
}

Write-Host ""
Write-Host "Web UI available at: $BaseUrl" -ForegroundColor Cyan
