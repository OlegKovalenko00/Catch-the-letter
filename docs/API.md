# API

Base URL: `http://127.0.0.1:8080`.

If `WEB_AUTH_TOKEN` is configured, pass it as `?token=...`, `X-Auth-Token`, or `Authorization: Bearer ...`.

All POST endpoints return JSON:

```json
{"ok": true}
```

or:

```json
{"ok": false, "error": "message"}
```

## Status

`GET /api/status`

Returns app state, mailbox statuses, event limit, Web/Telegram/browser/LLM config summary.

`GET /api/events?limit=100`

Returns recent event log records. The configured `app.events_limit` caps stored events.

`GET /api/config`

Returns sanitized config. Secrets are not included.

## Rules And Profile

`GET /api/rules`

Returns current rules JSON.

`POST /api/rules`

Body is rules JSON.

`GET /api/profile`

Returns current profile JSON.

`POST /api/profile`

Body is profile JSON.

## Forms

`GET /api/forms/active`

Returns only active statuses: `waiting_user_review`, `waiting_auth`, `waiting_2fa`, `waiting_submit_confirm`, `failed`.

`GET /api/forms/active?all=true`

Returns all sessions, including `submitted`, `manual_required`, and `cancelled`.

`GET /api/forms/{id}`

Returns one form session.

`POST /api/forms/{id}/field`

Update one or many field values:

```json
{"field_id": "email", "value": "student@example.com"}
```

```json
{"fields": [{"id": "full_name", "value": "Ivan Ivanov"}]}
```

`POST /api/forms/{id}/fill`

Fills the browser form after review. Required fields with empty values block filling.

`POST /api/forms/{id}/submit`

Submits only after the session is in `waiting_submit_confirm`.

`POST /api/forms/{id}/manual`

Marks a form as manual and closes the browser session best-effort.

`POST /api/forms/{id}/cancel`

Cancels a form and closes the browser session best-effort.

`POST /api/forms/{id}/reinspect`

Re-inspects the current browser session after manual login or navigation.

## Auth

`POST /api/forms/{id}/auth/credentials`

```json
{"username": "demo", "password": "demo", "remember": false}
```

Passwords are sent only to the local C++ service and browser-worker. They are not accepted through Telegram and are not persisted.

`POST /api/forms/{id}/auth/2fa`

```json
{"code": "123456"}
```

2FA codes are not persisted.

## Tests And Demo

`POST /api/test/browser`

Checks browser-worker health.

`POST /api/test/llm`

Runs a small LLM/noop classification check.

`POST /api/test/telegram`

Sends a Telegram test message if Telegram is configured.

`POST /api/demo/create`

Creates a demo form session through the running browser-worker.

`POST /api/demo/create-auth`

Creates a demo auth form session. Use `demo/demo`, then 2FA `123456`.

## Browser Worker API

Base URL: `http://127.0.0.1:8090`.

- `GET /health`
- `GET /demo-form`
- `GET /demo-auth-form`
- `POST /inspect-form` with `{"url":"..."}`
- `POST /fill-form` with `{"session_id":"...","fields":[...]}`
- `POST /submit-form` with `{"session_id":"..."}`
- `POST /auth/credentials` with `{"session_id":"...","username":"...","password":"..."}`
- `POST /auth/2fa` with `{"session_id":"...","code":"..."}`
- `POST /reinspect-form` with `{"session_id":"..."}`
- `POST /close-session` with `{"session_id":"..."}`
