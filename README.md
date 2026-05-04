# Catch the Letter

Catch the Letter is a local, single-user service that watches email, detects important messages and form requests, opens forms through a browser worker, proposes field values from a local profile, lets the user edit every value, fills the form, and submits only after explicit confirmation.

The main service is C++. Browser automation is isolated in a Python FastAPI + Playwright service. LLM support is optional and local through Ollama; with `llm.enabled=false` or unavailable Ollama, the project works through the rule-based `NoopLlmClient`.

## Architecture

```text
C++ app -> IMAP -> RuleEngine -> WorkflowEngine
        -> BrowserWorker / Telegram / Web UI / local LLM / SQLite
```

SQLite stores deduplication, mailbox checkpoints, active form sessions, Telegram dialogs, runtime keys, and the last `app.events_limit` events.

## Quick Start

```bash
cp .env.example .env
cp config/app.example.json config/app.json
cp config/rules.example.json config/rules.json
cp config/profile.example.json config/profile.json
docker compose up --build
```

Open the local Web UI:

```text
http://127.0.0.1:8080
```

The compose file exposes Web UI, browser-worker, and Ollama only on `127.0.0.1`.

## Run With Local LLM

```bash
docker compose --profile llm up --build
docker compose exec ollama ollama pull qwen3:8b
```

Then set in `config/app.json`:

```json
{
  "llm": {
    "enabled": true,
    "provider": "ollama",
    "endpoint": "http://ollama:11434/api/chat",
    "model": "qwen3:8b"
  }
}
```

If Ollama is down or returns invalid JSON, the app falls back to rule-based behavior.

## Demo

After `docker compose up --build`, use Web UI buttons:

- `Create Demo Form`: creates a normal active form.
- `Create Demo Auth Form`: creates a form requiring login and 2FA.

Demo auth credentials:

```text
login: demo
password: demo
2FA: 123456
```

Expected flow:

```text
waiting_auth -> waiting_2fa -> waiting_user_review -> waiting_submit_confirm -> submitted
```

CLI one-shot smoke modes also exist:

```bash
./build/catch_the_letter --demo
./build/catch_the_letter --demo-auth
```

## Mail Setup

Configure one or more entries in `mailboxes`. Each mailbox has its own checkpoint and dedup state.

Yandex:

```json
{
  "id": "main_yandex",
  "provider": "yandex",
  "email": "user@yandex.ru",
  "auth_method": "password",
  "username_env": "IMAP_USERNAME",
  "password_env": "IMAP_PASSWORD"
}
```

Yandex uses `imap.yandex.com:993` with SSL by default. Use an application password.

Gmail:

```json
{
  "id": "gmail",
  "provider": "gmail",
  "auth_method": "password",
  "username_env": "IMAP_USERNAME",
  "password_env": "IMAP_PASSWORD"
}
```

Gmail support is generic IMAP/password mode and should be treated as experimental. `auth_method=xoauth2` is accepted by config loading, but that mailbox is marked with the runtime error `Gmail XOAUTH2 is not implemented yet` and is not polled.

## Profile And Rules

Edit `config/profile.json` through Web UI or directly. Empty profile fields are not auto-filled. Sensitive keys such as `password`, `token`, `secret`, `passport`, `snils`, `birth`, `code`, and `cookie` are not sent to the LLM in safe mode.

Rules live in `config/rules.json`. Rules can classify, detect forms, and notify over Telegram or console. A plain `classify` action does not start form automation; form workflow starts only for detected form requests, high-confidence form links, or known form domains.

## Telegram

Telegram is the primary notification channel when configured. Review messages show form type, URL, proposed values, unknown fields, and buttons:

```text
Fill / Edit / Manual / Cancel / Reinspect
```

Edit format:

```text
1: value
field_id: value
```

Passwords are never accepted through Telegram. If a password-like message is sent outside a configured 2FA dialog, the bot replies with a Web UI instruction and does not log the text. 2FA codes can be accepted through Telegram only when `auth.two_factor_via_telegram=true`.

## Web UI

The local Web UI provides:

- Dashboard/status and recent events.
- Active forms with edit, fill, submit-confirm, auth, 2FA, manual, cancel, reinspect.
- Profile editor.
- Rules editor.
- Config view.
- Browser, LLM, Telegram test buttons.
- Demo form creation buttons.

If `WEB_AUTH_TOKEN` is set, API calls require `?token=...`, `X-Auth-Token`, or `Authorization: Bearer ...`. If it is not set, the UI runs in local-only unauthenticated mode.

## Supported Forms

Best-effort browser automation supports:

- Google Forms.
- Yandex Forms.
- Microsoft Forms.
- HSE portal polls (`portal.hse.ru/poll`).
- HSE LMS, SmartLMS, Moodle-like auth/manual flows.
- Generic HTML forms.

Personal-data consent checkboxes are treated as user-input fields and are not silently accepted.

## Security Modes

- `open`: public HTTP/HTTPS allowed, localhost/private/file/javascript/data URLs blocked.
- `strict`: only `allowed_domains` are allowed, private/local addresses still blocked.
- `paranoid`: browser-worker is not opened; detected forms become manual.

All URL logs redact query strings. Passwords, 2FA codes, tokens, cookies, and session secrets must not be written to SQLite event logs or shown in the UI.

## Troubleshooting

- Build fails on C++ headers: default build uses vendored `third_party/nlohmann`, `third_party/httplib`, `third_party/curl`, and `third_party/sqlite3.h`; use `-DUSE_SYSTEM_DEPS=ON` only if system headers are installed.
- Browser worker unavailable: check `docker compose logs browser-worker` and `http://127.0.0.1:8090/health`.
- Ollama unavailable: set `llm.enabled=false` or start `docker compose --profile llm up`.
- IMAP auth failed: verify app passwords and that env variables are visible inside compose.
- Telegram not sending: verify bot token, chat id, and `/api/test/telegram`.
- Auth failed: use Web UI for username/password; Telegram will not accept passwords.
- 2FA failed: retry through Web UI or Telegram if enabled.
- No forms detected: check extracted links in event log and rule conditions.
