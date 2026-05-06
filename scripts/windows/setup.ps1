# Windows setup script for Catch-the-letter
# Copies config templates and guides user through configuration
# Usage: .\scripts\windows\setup.ps1

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $ScriptDir)
Set-Location $ProjectRoot

Write-Host "=== Catch-the-letter Windows Setup ===" -ForegroundColor Green

# Copy .env
if (-Not (Test-Path ".env")) {
    if (Test-Path ".env.windows.example") {
        Copy-Item ".env.windows.example" ".env"
        Write-Host "[OK] Created .env from .env.windows.example" -ForegroundColor Green
    }
    else {
        Write-Host "[WARNING] .env.windows.example not found" -ForegroundColor Yellow
    }
}
else {
    Write-Host "[OK] .env already exists" -ForegroundColor Green
}

# Copy config files
$configs = @(
    @{src = "config/app.example.json"; dst = "config/app.json"},
    @{src = "config/rules.example.json"; dst = "config/rules.json"},
    @{src = "config/profile.example.json"; dst = "config/profile.json"}
)

foreach ($config in $configs) {
    if (-Not (Test-Path $config.dst)) {
        if (Test-Path $config.src) {
            Copy-Item $config.src $config.dst
            Write-Host "[OK] Created $($config.dst)" -ForegroundColor Green
        }
        else {
            Write-Host "[WARNING] $($config.src) not found" -ForegroundColor Yellow
        }
    }
    else {
        Write-Host "[OK] $($config.dst) already exists" -ForegroundColor Green
    }
}

Write-Host ""
Write-Host "Setup complete!" -ForegroundColor Green
Write-Host ""
Write-Host "Next steps:" -ForegroundColor Cyan
Write-Host "1. Edit .env with your credentials:"
Write-Host "   - IMAP_USERNAME / IMAP_PASSWORD"
Write-Host "   - TELEGRAM_BOT_TOKEN / TELEGRAM_CHAT_ID"
Write-Host "   - GMAIL_USERNAME / GMAIL_PASSWORD (optional)"
Write-Host "   - WEB_AUTH_TOKEN (optional)"
Write-Host ""
Write-Host "2. Edit config/profile.json with user settings"
Write-Host ""
Write-Host "3. Choose launch mode:"
Write-Host "   A) Without Ollama:"
Write-Host "      .\scripts\windows\start.ps1"
Write-Host ""
Write-Host "   B) With native Ollama (recommended for powerful PC):"
Write-Host "      ollama pull qwen3:4b"
Write-Host "      .\scripts\windows\start-llm-native.ps1"
Write-Host ""
Write-Host "   C) With Docker Ollama:"
Write-Host "      `$env:LLM_ENDPOINT='http://ollama:11434/api/chat'"
Write-Host "      .\scripts\windows\start-llm-docker.ps1"
Write-Host ""
Write-Host "4. After startup, access:"
Write-Host "   Web UI: http://127.0.0.1:8080"
Write-Host "   API: http://127.0.0.1:8080/api/status"
Write-Host ""
