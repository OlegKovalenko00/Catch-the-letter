# API

Base URL: `http://127.0.0.1:8080`.

If `WEB_AUTH_TOKEN` is set, pass it as `?token=...`, `X-Auth-Token`, or `Authorization: Bearer ...`.

POST success:

```json
{"ok": true, "message": "...", "data": {}}
```

POST error:

```json
{"ok": false, "error": "...", "details": {}}
```

Some legacy handlers return `{"ok": true}` with top-level fields.

## Status

`GET /api/status`

Returns app status, mailbox status, Web/Telegram/browser/LLM summaries. Telegram token is never returned; proxy credentials are redacted.

`GET /api/dashboard`

Dashboard summary. Currently aliases status data.

`GET /api/events?limit=N`

Recent capped event log.

`GET /api/config`

Sanitized config summary.

## Rules And Profile

`GET /api/rules`

`POST /api/rules`

Body is full rules JSON.

`GET /api/profile`

`POST /api/profile`

Body is full profile JSON. New sessions use the updated profile.

## Forms

`GET /api/forms/active`

Returns active statuses: `waiting_user_review`, `waiting_auth`, `waiting_2fa`, `waiting_submit_confirm`, `failed`.

`GET /api/forms/active?all=true`

Includes submitted/manual/cancelled sessions.

`GET /api/forms/{id}`

Returns one session.

### Field Model

```json
{
  "id": "rating",
  "selector": "fieldset:nth-of-type(1)",
  "label": "Оценка",
  "normalized_label": "оценка",
  "type": "radio_group",
  "required": true,
  "options": [
    {"label": "5", "value": "5", "selector": "input[value='5']", "id": ""}
  ],
  "value": "5",
  "values": [],
  "semantic_key": "rating",
  "mapped_profile_key": "",
  "suggested_value": "5",
  "option_value": "5",
  "confidence": 0.86,
  "source": "rule|llm|profile|user",
  "reason": "matched deterministic profile alias",
  "risk": "low|medium|high",
  "requires_user_input": false,
  "can_auto_fill": true,
  "unsupported_reason": "",
  "user_modified": false,
  "validation_error": "",
  "question_block_text": "...",
  "placeholder": "",
  "aria_label": "",
  "nearby_text": ""
}
```

For backward compatibility, browser-worker and storage accept old `options: ["5","4","3"]` and new option objects.

### Update Fields

`POST /api/forms/{id}/field`

`POST /api/forms/{id}/fields`

```json
{"field_id": "email", "value": "student@example.com"}
```

```json
{"fields": [{"id": "full_name", "value": "Ivan Ivanov"}]}
```

User edits set `user_modified=true`; remap preserves them.

### Actions

`POST /api/forms/{id}/remap`

Reruns semantic mapping, preserves user edits unless `force=true`, returns diff, updated fields, validation and summary.

```json
{"force": false, "use_llm": true}
```

```json
{
  "ok": true,
  "data": {
    "summary": {"ready": 4, "needs_input": 2, "unsupported": 1, "low_confidence": 1},
    "diff": [
      {
        "field_id": "email",
        "label": "Email",
        "old_value": "",
        "new_value": "student@example.com",
        "old_source": "empty",
        "new_source": "rule",
        "confidence": 0.9,
        "reason": "generic email field matched profile email"
      }
    ]
  }
}
```

`POST /api/forms/{id}/validate`

Checks missing required values, invalid options, unsupported required fields and warnings.

```json
{
  "ok": true,
  "data": {
    "can_fill": false,
    "missing_required": [{"field_id": "full_name", "label": "ФИО", "error": "required field is empty"}],
    "invalid_options": [],
    "unsupported_required": [],
    "warnings": []
  }
}
```

`POST /api/forms/{id}/explain-field`

```json
{"field_id": "rating"}
```

Returns mapping source, reason, confidence, risk and suggested next action for one field.

`POST /api/forms/{id}/fill`

Validates and fills the browser form. Required empty/unsupported fields block.

`POST /api/forms/{id}/submit`

Submits only from `waiting_submit_confirm`. If browser-worker returns `needs_next`, fields are updated and status becomes `waiting_user_review`.

`POST /api/forms/{id}/manual`

Marks manual and closes browser session best-effort.

`POST /api/forms/{id}/cancel`

Cancels and closes browser session best-effort.

`POST /api/forms/{id}/reinspect`

Re-inspects after manual login/navigation.

## Auth

`POST /api/forms/{id}/auth/credentials`

```json
{"username": "demo", "password": "demo", "remember": false}
```

Passwords are accepted only through local Web UI/API and are not stored.

`POST /api/forms/{id}/auth/2fa`

```json
{"code": "123456"}
```

2FA codes are not stored.

## Manual URL Tests

`POST /api/forms/inspect-url`

```json
{"url": "https://forms.yandex.ru/...", "debug": true}
```

Response:

```json
{
  "ok": true,
  "form_type": "yandex_forms",
  "auth_required": false,
  "fields": [],
  "screenshot_path": "/data/browser/session-inspect.png",
  "debug": {
    "page_title": "...",
    "final_url": "...",
    "visible_text_sample": "...",
    "input_count": 3,
    "textarea_count": 1,
    "select_count": 0,
    "role_radio_count": 3,
    "role_checkbox_count": 2,
    "button_count": 1,
    "candidate_question_blocks": 5,
    "extraction_strategy_used": "dom_controls|visible_text_virtual_fields|auth_required",
    "extraction_errors": [],
    "form_type": "yandex_forms",
    "auth_required": false,
    "error": ""
  }
}
```

`POST /api/forms/create-from-url`

```json
{"url": "https://forms.yandex.ru/...", "title": "Manual test form", "debug": true}
```

Creates `waiting_auth`, `waiting_user_review`, or `manual_required` and returns:

```json
{"ok": true, "session_id": "...", "status": "waiting_user_review"}
```

## Tests

`POST /api/test/browser`

Checks `/health`, inspects `/demo-form`, verifies radio group `Оценка` with options `5/4/3`, and verifies checkbox group.

```json
{
  "ok": true,
  "worker_reachable": true,
  "demo_inspect_ok": true,
  "radio_group_ok": true,
  "checkbox_group_ok": true,
  "error": ""
}
```

`POST /api/test/imap`

Connects to every mailbox, logs in, selects folder and gets max UID without changing checkpoints.

```json
{
  "ok": true,
  "mailboxes": [
    {
      "id": "main_yandex",
      "provider": "yandex",
      "reachable": true,
      "auth_ok": true,
      "folder_ok": true,
      "max_uid": 123,
      "skipped": false,
      "error": ""
    }
  ]
}
```

Missing credentials are reported as skipped/error without crashing. Gmail XOAUTH2 returns `Gmail XOAUTH2 is not implemented yet`.

`POST /api/test/llm`

Checks Noop or Ollama endpoint/JSON mode, memory requirements and sample mapping. `ok` can be `true` while `fallback=true` when Noop fallback works:

```json
{
  "ok": true,
  "enabled": true,
  "provider": "ollama",
  "active_client": "OllamaClient",
  "model": "qwen3:4b",
  "endpoint": "http://127.0.0.1:11434/api/chat",
  "reachable": true,
  "model_ready": true,
  "fallback": false,
  "timeout_seconds": 300,
  "healthcheck_timeout_seconds": 30,
  "total_duration_ms": 55002,
  "warning": "",
  "next_action": "",
  "memory": {
    "detected": true,
    "total_gb": 15.4,
    "min_required_gb": 6,
    "recommended_gb": 8,
    "sufficient": true
  },
  "sample_classification": "form_request",
  "sample_mapping_ok": true,
  "mapped_fields": {
    "ФИО": {"mapped_profile_key": "full_name"},
    "Email": {"mapped_profile_key": "personal_email"},
    "Группа": {"mapped_profile_key": "student_group"}
  }
}
```

`POST /api/test/telegram`

Sends a Telegram test message and reports proxy details:

```json
{
  "ok": true,
  "token_configured": true,
  "chat_id_configured": true,
  "proxy_configured": true,
  "proxy_url_redacted": "http://127.0.0.1:10809",
  "ip_resolve": "ipv4",
  "timeout_seconds": 30,
  "message": "Telegram test message sent"
}
```

## Demo

`POST /api/demo/create`

Creates a demo form session.

`POST /api/demo/create-auth`

Creates a demo auth session. Use `demo/demo`, then 2FA `123456`.

## Browser Worker API

Base URL: `http://127.0.0.1:8090`.

- `GET /health`
- `GET /demo-form`
- `GET /demo-auth-form`
- `GET /demo-thanks`
- `POST /inspect-form` with `{"url":"...","debug":true}`
- `POST /fill-form` with `{"session_id":"...","fields":[...]}`
- `POST /submit-form` with `{"session_id":"..."}`
- `POST /auth/credentials`
- `POST /auth/2fa`
- `POST /reinspect-form`
- `POST /close-session`
