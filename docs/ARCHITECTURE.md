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
- `TelegramDialogManager`: polling, callbacks, edit flow, 2FA dialog flow.
- `HttpServer`: local Web UI and REST API.
- `OllamaClient` / `NoopLlmClient`: local optional LLM and deterministic fallback.
- `SqliteStorage`: checkpoints, dedup, active sessions, Telegram dialogs, event log, runtime keys.

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
