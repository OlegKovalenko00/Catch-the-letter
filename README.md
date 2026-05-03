# Catch the Letter

Local C++ service for monitoring a mailbox and notifying about important messages.

Implemented foundation:

- JSON app config with environment expansion.
- IMAP UID checkpoint: on first start the app stores current max UID and later reads only newer messages.
- SQLite dedup via `processed_message`.
- Capped `event_log` controlled by `app.events_limit` or `--events-limit`.
- URL extraction from message bodies into `links.url` / `links.domain`.
- Rule operations: `contains`, `not_contains`, `equals`, `not_equals`, `regex`, `regex_i`, `contains_any`, `exists`, `domain_in`.
- Local HTTP endpoints:
  - `GET /api/status`
  - `GET /api/events?limit=200`
  - `GET /api/rules`
  - `POST /api/rules`

## Quick Start

Create runtime files:

```bash
cp .env.example .env
cp config/app.example.json config/app.json
cp config/rules.example.json config/rules.json
```

Edit `.env`, then run:

```bash
docker compose up --build
```

Local UI:

```text
http://127.0.0.1:8080
```

## CLI

```bash
./catch_the_letter --config config/app.json --once --events-limit 200 --log-level info
```

## Yandex IMAP

The example config uses:

```json
{
  "host": "imap.yandex.com",
  "port": 993,
  "tls": true
}
```

Use an app password in `IMAP_PASSWORD`.
