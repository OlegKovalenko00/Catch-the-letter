# Architecture

Catch the Letter is a local single-user service. The C++ app owns mail polling, rules, workflow state, Telegram dialogs, Web UI, LLM integration, and SQLite storage. Browser automation is isolated in a Python FastAPI service backed by Playwright.

```text
IMAP mailbox(es)
  -> MailClientImap
  -> RuleEngine
  -> WorkflowEngine
  -> BrowserWorkerClient -> browser-worker/FastAPI/Playwright
  -> TelegramBot / Web UI
  -> SQLite
  -> optional Ollama or NoopLlmClient
```

## Modules

- `Config`: JSON config loader with old `imap` compatibility and new `mailboxes` array.
- `MailClientImap`: generic IMAP over libcurl with MIME subject/body/link extraction.
- `RuleEngine`: rule matching over sender, subject, body, provider, dates, links, and attachments.
- `WorkflowEngine`: important notification, form detection, browser inspect/fill/submit, auth, 2FA, manual/cancel.
- `BrowserWorkerClient`: JSON HTTP client for the browser-worker.
- `TelegramDialogManager`: polling, callbacks, guided missing-field answers, batch edit fallback, Remap callback, 2FA dialog flow.
- `HttpServer`: local Web UI static file serving plus REST API. It loads `web/index.html`, `web/app.js`, and `web/styles.css` from `/app/web` in Docker or `./web` in local runs, with a tiny fallback page if files are missing.
- `OllamaClient` / `NoopLlmClient`: local Ollama-first LLM layer with deterministic fallback when Ollama/model/resources are unavailable.
- `SqliteStorage`: checkpoints, dedup, active sessions, Telegram dialogs, event log, runtime keys.

## Field Mapping Pipeline

The browser-worker returns an extended field model:

- field identity: `id`, `selector`, `label`, `normalized_label`, `question_block_text`, `placeholder`, `aria_label`, `nearby_text`;
- type: text, email, tel, textarea, select, checkbox, radio group, checkbox group;
- option objects: `label`, `value`, `selector`, `id`;
- mapping metadata: `semantic_key`, `mapped_profile_key`, `confidence`, `source`, `reason`;
- safety flags: `requires_user_input`, `can_auto_fill`, `unsupported_reason`, `user_modified`, `validation_error`.

Mapping order:

1. `FormUnderstandingEngine` normalizes labels, question text, placeholders and nearby text.
2. Deterministic aliases map common HSE/student fields without any network call.
3. Type and option compatibility checks reject unsafe radio/select/checkbox suggestions.
4. Optional Ollama semantic mapping runs in JSON mode with safe prompt data only.
5. App-side schema validation clamps confidence, ignores unknown field ids and rejects bad options.
6. User override protection preserves `user_modified` fields unless `force=true`.
7. Final fillability validation reports missing required, invalid options, unsupported required fields and warnings.

`Remap with AI` reruns mapping but preserves fields edited by the user. `Validate` checks missing required fields, invalid options and unsupported required controls before fill.

The default config enables Ollama with `qwen3:4b`, but startup and `/api/test/llm` perform best-effort resource and JSON-mode probes. If RAM is below `llm.min_memory_gb`, the endpoint is unavailable, or the model is still pulling, the app reports `llm_fallback` and continues with Noop. Docker profile `llm` includes `ollama-init` to preload the configured model.

## Workflow Statuses

- `ignored`
- `important_notified`
- `form_detected`
- `waiting_user_review`
- `waiting_auth`
- `waiting_2fa`
- `waiting_submit_confirm`
- `submitted`
- `manual_required`
- `cancelled`
- `failed`

Form submission is never automatic. The browser-worker can fill only after user review, and final submit requires `waiting_submit_confirm`.

## Storage

SQLite tables:

- `processed_message`: dedup by mailbox and UID.
- `mailbox_checkpoint`: per-mailbox UID baseline and last seen UID.
- `active_form_session`: form URL, type, status, fields, auth state, browser session id.
- `telegram_dialog`: one active dialog per chat id.
- `event_log`: capped by `app.events_limit`.
- `runtime_kv`: Telegram update offset and small runtime values.

`active_form_session` stores `fields_json`, `proposed_values_json`, and `unknown_fields_json` in sync from the current field list. `list_active_form_sessions(false)` returns only active statuses; `?all=true` exposes historical sessions.

## Security Model

URL policy allows only HTTP/HTTPS and blocks:

- localhost and loopback.
- RFC1918 private IPv4 ranges.
- link-local and unspecified addresses.
- IPv6 loopback, ULA, link-local, multicast, and unspecified.
- `file:`, `javascript:`, and `data:` URLs.

Domains are resolved with `getaddrinfo`; if DNS resolution fails, the URL is blocked. `open` allows public HTTP/HTTPS except blocked domains. `strict` allows only `allowed_domains`. `paranoid` never opens browser-worker and creates manual sessions.

Logs and event records redact URL query strings and must not contain passwords, 2FA codes, cookies, tokens, or session secrets.

## Telegram Proxy Model

`TELEGRAM_PROXY_URL` is read from the environment through `telegram.proxy_url_env`. Telegram HTTP requests use libcurl with:

- `CURLOPT_PROXY` when proxy URL is configured;
- IPv4 resolution;
- 15 second connect timeout;
- 30 second request timeout;
- SSL verification enabled.

For Linux host VPN routing, `docker-compose.vpn.yml` runs only the C++ app with `network_mode: host`, sets `BROWSER_WORKER_ENDPOINT=http://127.0.0.1:8090`, and leaves browser-worker in the normal Docker service with a host-bound port.

## Auth Model

Passwords may be entered only through the local Web UI or another local secure channel. Telegram explicitly refuses password-like text outside the 2FA state.

Auth flow:

```text
waiting_auth
  -> POST /api/forms/{id}/auth/credentials
  -> waiting_2fa or waiting_user_review
  -> POST /api/forms/{id}/auth/2fa
  -> reinspect
  -> waiting_user_review
```

2FA can be entered through Web UI or Telegram when enabled. Passwords and 2FA codes are not stored in SQLite.

## Browser Worker

The worker extracts generic HTML fields and best-effort Google/Yandex/Microsoft/HSE/LMS-like question blocks. It supports text inputs, textarea, select, checkbox, radio groups, checkbox groups, auth credentials, one-time code screens, multi-page next buttons, submit buttons, screenshots, and best-effort session cleanup.

## OS and Network Modes

| Mode | OS | Network | App | Browser-Worker | Telegram Proxy | Ollama |
|------|----|---------|----|-----------------|---|---------|
| **Linux Normal** | Linux | Docker bridge | bridge | bridge, `127.0.0.1:8090` | N/A | Docker service `ollama:11434/api/chat` |
| **Linux VPN/Proxy** | Linux | Host network | `network_mode: host` | bridge, `127.0.0.1:8090` | `http://127.0.0.1:10809` (optional) | Localhost `http://127.0.0.1:11434/api/chat` |
| **Windows Native Ollama** | Windows | Docker bridge | bridge | bridge, `127.0.0.1:8090` | `http://host.docker.internal:10809` (optional) | Windows native `http://host.docker.internal:11434/api/chat` |
| **Windows Docker Ollama** | Windows | Docker bridge | bridge | bridge, `127.0.0.1:8090` | `http://host.docker.internal:10809` (optional) | Docker service `ollama:11434/api/chat` |

### Environment Settings by Mode

**Linux Normal**:
```env
LLM_ENDPOINT=http://ollama:11434/api/chat
TELEGRAM_PROXY_URL=
```
```bash
docker compose --profile llm up --build
```

**Linux VPN/Proxy**:
```env
LLM_ENDPOINT=http://127.0.0.1:11434/api/chat
TELEGRAM_PROXY_URL=http://127.0.0.1:10809
```
```bash
docker compose -f docker-compose.yml -f docker-compose.vpn.yml --profile llm up --build
```

**Windows Native Ollama**:
```env
LLM_ENDPOINT=http://host.docker.internal:11434/api/chat
TELEGRAM_PROXY_URL=http://host.docker.internal:10809
```
```powershell
.\scripts\windows\start-llm-native.ps1
```

**Windows Docker Ollama**:
```env
LLM_ENDPOINT=http://ollama:11434/api/chat
TELEGRAM_PROXY_URL=http://host.docker.internal:10809
```
```powershell
$env:LLM_ENDPOINT="http://ollama:11434/api/chat"
.\scripts\windows\start-llm-docker.ps1
```
