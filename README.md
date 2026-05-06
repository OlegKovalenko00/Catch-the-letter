# Catch the Letter

Catch the Letter watches new IMAP mail, detects important messages and form links, opens forms in a Playwright browser-worker, proposes field values from `profile.json`, lets you review/edit them in Web UI or Telegram, fills the form, and submits only after explicit confirmation.

The app works without paid APIs. Local Ollama is enabled by default for better semantic mapping, but every LLM call has a deterministic `NoopLlmClient` fallback, so the service still works when Ollama or the model is unavailable.

## Quick Start

```bash
cp .env.example .env
cp config/app.example.json config/app.json
cp config/rules.example.json config/rules.json
cp config/profile.example.json config/profile.json
```

Fill `.env`:

```env
IMAP_USERNAME=
IMAP_PASSWORD=
TELEGRAM_BOT_TOKEN=
TELEGRAM_CHAT_ID=
TELEGRAM_PROXY_URL=
WEB_AUTH_TOKEN=
LLM_ENABLED=true
LLM_MODEL=qwen3:4b
LLM_ENDPOINT=http://ollama:11434/api/chat
LLM_TIMEOUT_SECONDS=300
LLM_HEALTHCHECK_TIMEOUT_SECONDS=30
LLM_AUTO_PULL=true
LLM_AUTO_FALLBACK=true
```

Fill `config/profile.json`, then run:

```bash
docker compose up --build
```

Open:

```text
http://127.0.0.1:8080
```

The Web UI has buttons for Test Browser, Test Telegram, Test IMAP, Test LLM, Create Demo Form, Create Demo Auth Form, and Manual / Yandex Form Test.
The UI is served from `web/index.html`, `web/app.js`, and `web/styles.css`; Docker copies these files into `/app/web`.

## Windows 10/11 + Docker Desktop

Prerequisites:

- Docker Desktop with WSL2 backend
- Git for Windows
- PowerShell 5.0+
- Optional: Ollama for Windows for native GPU LLM (recommended for powerful machines)

### Setup

```powershell
.\scripts\windows\setup.ps1
notepad .env
notepad config\profile.json
```

The script creates `.env`, `config/app.json`, `config/rules.json`, `config/profile.json` from examples.

### Mode A: Basic (no Docker Ollama)

```powershell
.\scripts\windows\start.ps1
```

Access: `http://127.0.0.1:8080`

### Mode B: With Native Ollama (recommended for powerful PC)

Prerequisites:

1. Install Ollama for Windows from `https://ollama.ai`
2. Keep Ollama running on Windows
3. Pull the model: `ollama pull qwen3:4b`

Set `.env`:

```env
LLM_ENABLED=true
LLM_MODEL=qwen3:4b
LLM_ENDPOINT=http://host.docker.internal:11434/api/chat
LLM_TIMEOUT_SECONDS=300
```

Start:

```powershell
.\scripts\windows\start-llm-native.ps1
```

Access: `http://127.0.0.1:8080`

### Mode C: With Docker Ollama

```powershell
$env:LLM_ENDPOINT="http://ollama:11434/api/chat"
.\scripts\windows\start-llm-docker.ps1
```

Access: `http://127.0.0.1:8080`

### Tests

```powershell
.\scripts\windows\test.ps1
.\scripts\windows\test-ollama.ps1
```

### Stop

```powershell
.\scripts\windows\stop.ps1
```

### Telegram Proxy on Windows

If Telegram works directly, leave `TELEGRAM_PROXY_URL` empty in `.env`.

If you have a VPN/proxy running on Windows localhost (e.g., `127.0.0.1:10809`), set:

```env
TELEGRAM_PROXY_URL=http://host.docker.internal:10809
```

Do not use `127.0.0.1:10809` from inside Docker bridge; use `host.docker.internal` instead.

### Troubleshooting on Windows

- Docker Desktop not running: start Docker Desktop.
- WSL2 disabled: enable in Windows Features.
- Port 8080 already in use: stop other services or change firewall.
- Ollama native not reachable from Docker: ensure Ollama is running, check `http://127.0.0.1:11434/api/tags` on Windows.
- `host.docker.internal` timeout: try with Docker Ollama service or disable LLM.
- Telegram timeout: try `TELEGRAM_PROXY_URL=http://host.docker.internal:10809` or disable proxy.

## Linux: Normal Mode

```bash
./scripts/linux/start.sh
```

This uses `docker compose --profile llm up --build` with Ollama inside Docker. Access: `http://127.0.0.1:8080`.

## Linux: VPN / Proxy Mode

If Telegram / Ollama require host network routing:

```bash
./scripts/linux/start-vpn.sh
```

This uses `docker-compose.vpn.yml` with `network_mode: host`. Requires:

- Ollama running on `127.0.0.1:11434`
- Telegram proxy on `127.0.0.1:10809` (if needed)

## Telegram Through VPN Or Proxy On Linux

If Telegram works on the host but times out from Docker bridge, use the VPN-friendly override. Example for a local HTTP/SOCKS proxy exposed on the host at `127.0.0.1:10809`:

```env
TELEGRAM_PROXY_URL=http://127.0.0.1:10809
```

Run:

```bash
docker compose -f docker-compose.yml -f docker-compose.vpn.yml up --build
```

In this mode the app uses `network_mode: host`, so Telegram traffic follows host VPN/proxy routing. The browser-worker stays in Docker and is reached through `BROWSER_WORKER_ENDPOINT=http://127.0.0.1:8090`.

The override sets `WEB_HOST=${WEB_HOST:-127.0.0.1}`. If your Docker host-network environment does not expose the app on localhost, run once with `WEB_HOST=0.0.0.0` and keep local firewall rules in mind.

Diagnostics:

```bash
TOKEN="..."
curl -m 15 "https://api.telegram.org/bot${TOKEN}/getMe"

docker run --rm --network catch-the-letter_default curlimages/curl:latest \
  -m 15 "https://api.telegram.org/bot${TOKEN}/getMe"

docker run --rm --network host curlimages/curl:latest \
  -m 15 "https://api.telegram.org/bot${TOKEN}/getMe"

docker compose -f docker-compose.yml -f docker-compose.vpn.yml config
curl -X POST http://127.0.0.1:8080/api/test/telegram
```

If `TELEGRAM_PROXY_URL` contains credentials, API responses redact the password.

## Mail Setup

Default `config/app.example.json` is configured for Yandex Mail:

- provider `yandex`
- host `imap.yandex.com`
- port `993`
- SSL enabled
- username from `IMAP_USERNAME`
- password from `IMAP_PASSWORD`

Use an app password for Yandex. On first run the app stores a max-UID checkpoint and only processes newer messages after that.

Gmail password mode is supported as generic IMAP:

- provider `gmail`
- host `imap.gmail.com`
- port `993`
- SSL enabled
- `auth_method=password`

Gmail XOAUTH2 is not implemented yet. If configured, the mailbox is reported as unavailable with `Gmail XOAUTH2 is not implemented yet`; the app does not crash.

## Rules

`config/rules.example.json` detects:

- education forms and surveys by keywords and known domains;
- important HSE notifications without auto-opening a form;
- generic important reminders.

An ordinary `@hse.ru` email does not start a form workflow by itself. Form workflow starts only from a classified form request, a confident form link, or a known form domain matched by rules.

## Profile

`config/profile.json` can contain:

- `full_name`, `last_name`, `first_name`, `middle_name`
- `hse_email`, `personal_email`, `phone`
- `student_group`, `faculty`, `programme`, `course_year`, `campus`
- `custom` keys

Empty values are ignored. Sensitive values such as passwords, tokens, cookies, codes, passport/SNILS-like fields are not sent to LLM prompts in safe mode.

## Web UI Flow

Use the dashboard buttons:

1. Test Browser: health plus demo form extraction, including radio `Оценка` options `5/4/3` and checkbox group.
2. Test Telegram: sends a message and reports token/chat/proxy status without exposing secrets.
3. Test IMAP: connects, logs in, selects folder, reads max UID, does not change checkpoints.
4. Test LLM: checks Noop or Ollama plus sample semantic mapping.
5. Create Demo Form: creates a review session.
6. Create Demo Auth Form: creates an auth session. Use login `demo`, password `demo`, code `123456`.

Form review shows labels, field type, required flag, editor, source, confidence, reason, risk, validation errors and actions:

- Save
- Remap with AI
- Validate
- Fill form
- Submit after confirmation
- Manual
- Cancel

Passwords are entered only in Web UI. Password and 2FA inputs are cleared after submit.

The mapping pipeline is deterministic first and LLM-assisted second: normalized labels/question text are matched to profile aliases, values are checked against field type/options, Ollama suggestions are schema-validated, low-confidence results ask for user input, and `user_modified` fields are not overwritten unless a force remap is requested.

## Manual / Yandex Form Test

Create a simple Yandex Form with:

- `ФИО` text
- `Email`
- `Группа`
- `Оценка` radio `5`, `4`, `3`
- `Интересы` checkbox
- `Комментарий` textarea

Paste the URL into the Web UI block “Manual / Yandex Form Test”:

1. Inspect.
2. Check fields/debug output.
3. Create session from URL.
4. Review/edit values.
5. Fill.
6. Submit after confirmation.

If extraction fails, the response includes debug data: final URL, visible text sample, control counts and screenshot path. The session can fall back to `manual_required`.
When DOM extraction finds no reliable controls but visible text looks like questions, the worker may show virtual diagnostic fields. These are marked as not auto-fillable until a selector is found, so they help explain the page but do not cause unsafe filling.

## Real Email Flow

Send yourself an email with a Yandex Forms link. The app should:

1. Read only new mail after checkpoint.
2. Detect `forms.yandex.ru`.
3. Create an active form session.
4. Send Telegram review.
5. Propose profile values.
6. Ask for missing fields.
7. Fill after review.
8. Ask for submit confirmation.
9. Submit.

Important emails without forms are sent to Telegram as reminders.

## Telegram UX

Telegram review shows a concise summary: form title, type, auto-filled count, fields needing input and missing required fields. Buttons include “Открыть Web UI”, “Ответить здесь”, “Remap”, “Заполнить форму”, “Отложить”, “Вручную”, “Отмена”.

“Ответить здесь” starts a guided dialog. The bot asks the next missing field, shows why it is needed, shows options for radio/select fields, and accepts a normal text answer. For checkbox groups, send several values separated by commas. When required answers are complete, the bot offers “Заполнить форму”; submit still requires a separate confirmation after fill.

The old batch edit format also works:

```text
1: значение
field_id: значение
```

Passwords are rejected in Telegram. 2FA codes are accepted only when the session is in `waiting_2fa`.

Commands:

- `/help` — available actions and password policy.
- `/status` — short service status.
- `/forms` — active forms list.
- `/cancel` — clear the current Telegram dialog without cancelling active forms.

## Local LLM / Ollama By Default

`config/app.example.json` has `llm.enabled=true` and model `qwen3:4b`. If Ollama is not running, the model is still pulling, JSON mode fails, or the machine has less than the configured minimum RAM, the app does not crash. It reports `LLM FALLBACK` and uses rule-based Noop mapping.

Ordinary Docker mode with automatic model preload:

```bash
docker compose --profile llm up --build
```

VPN/proxy host-network mode:

```bash
docker compose -f docker-compose.yml -f docker-compose.vpn.yml --profile llm up --build
```

The `ollama-init` service waits for Ollama and runs `ollama pull "$LLM_MODEL"` unless `LLM_AUTO_PULL=false`. Watch preload logs:

```bash
docker compose logs -f ollama-init
docker compose logs -f ollama
```

Endpoint rules:

```env
# ordinary Docker bridge
LLM_ENDPOINT=http://ollama:11434/api/chat

# docker-compose.vpn.yml, app uses network_mode: host
LLM_ENDPOINT=http://127.0.0.1:11434/api/chat
```

Memory guidance: `qwen3:4b` needs roughly 6GB minimum and 8GB recommended. `qwen3:8b` can give better answers but needs more RAM.

CPU inference can be slow. The app uses a short `/api/tags` healthcheck timeout (`LLM_HEALTHCHECK_TIMEOUT_SECONDS`, default 30s) and a separate full chat timeout (`LLM_TIMEOUT_SECONDS`, default 300s) for `/api/chat`, form mapping and `Test LLM`.

`Test LLM` reports `LLM OK`, `LLM FALLBACK`, or `LLM DISABLED`, including endpoint, model, memory check, fallback reason and next action. In fallback mode, `Remap with AI` becomes rule-based remap and the product remains usable.

## Security

URL policy allows only public `http` and `https`. It blocks `file:`, `javascript:`, `data:`, localhost, private IPv4 ranges, link-local ranges, loopback, unspecified, IPv6 ULA/link-local/multicast and domains resolving to blocked IPs.

Modes:

- `open`: public HTTP/HTTPS except blocked domains and private networks.
- `strict`: only `allowed_domains`.
- `paranoid`: browser-worker never opens URLs; sessions become `manual_required`.

Logs and API responses redact Telegram tokens, IMAP passwords, password/code/cookie/authorization values and proxy credentials.

## Local Checks

```bash
scripts/smoke_build.sh
scripts/smoke_llm.sh
python3 -m py_compile browser-worker/main.py
```

Browser-worker smoke requires the worker running on `127.0.0.1:8090`:

```bash
scripts/smoke_browser_worker.sh
```

## Troubleshooting

- Telegram timeout: use `docker-compose.vpn.yml` and check `proxy_configured` in Test Telegram.
- BrowserWorker unavailable: check `curl http://127.0.0.1:8090/health` and Playwright image/logs.
- Yandex fields not found: use Manual Inspect with debug and screenshot; complex/captcha/file-upload forms require manual handling.
- Ollama unavailable: Test LLM should show fallback to Noop.
- IMAP auth failed: verify app password and IMAP access in provider settings.
- No forms detected: verify rules, link domain and event log.
- Auth failed: use Web UI only for password; 2FA can continue through Web UI or Telegram.

Known limitations: best-effort support for common Yandex/Google/Microsoft/HSE forms; no captcha/file upload automation; Gmail XOAUTH2 is not implemented.
