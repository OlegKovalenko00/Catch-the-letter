#!/usr/bin/env bash


set -euo pipefail

DB="${1:-data/state.db}"
OUTPUT="${2:-}"

if [[ ! -f "$DB" ]]; then
  echo "ERROR: database not found: $DB" >&2
  exit 1
fi

if ! command -v sqlite3 >/dev/null 2>&1; then
  echo "ERROR: sqlite3 is required but not installed" >&2
  exit 1
fi

if [[ -z "$OUTPUT" ]]; then
  DB_DIR="$(dirname "$DB")"
  OUTPUT="${DB_DIR}/export_sanitized.json"
fi

echo "Source DB : $DB" >&2
echo "Output    : $OUTPUT" >&2


sqlite_json() {
  sqlite3 -json "$DB" "$1" 2>/dev/null || echo "[]"
}


EMAIL_QUERY="
SELECT
  id,
  mailbox_id,
  uid,
  message_id,
  '[REDACTED]'          AS from_addr,
  '[REDACTED]'          AS to_addr,
  subject,
  date_iso,
  importance_level,
  ROUND(importance_score, 3) AS importance_score,
  category,
  status,
  CASE WHEN read_at    != '' THEN 'yes' ELSE '' END AS read,
  CASE WHEN archived_at!= '' THEN 'yes' ELSE '' END AS archived,
  CASE WHEN muted_until!= '' THEN 'yes' ELSE '' END AS muted,
  created_at,
  updated_at
FROM email_message
ORDER BY created_at DESC;"


ATT_QUERY="
SELECT
  id,
  email_id,
  mailbox_id,
  uid,
  part_id,
  filename,
  mime_type,
  size_bytes,
  disposition,
  safe_to_preview,
  downloaded,
  ''  AS local_path,
  ''  AS sha256,
  created_at
FROM email_attachment
ORDER BY created_at DESC;"


CP_QUERY="SELECT * FROM mailbox_checkpoint ORDER BY mailbox_id;"


NL_QUERY="SELECT uid, channel, status, ts_iso FROM notification_log ORDER BY ts_iso DESC LIMIT 500;"


EV_QUERY="
SELECT
  id,
  level,
  type,
  message,
  '{}' AS data_json,
  created_at
FROM event_log
ORDER BY created_at DESC
LIMIT 500;"


{
  printf '{\n'
  printf '  "export_version": 1,\n'
  printf '  "exported_at": "%s",\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
  printf '  "source_db": "%s",\n' "$DB"
  printf '  "note": "PII fields (from, to, body, links, attachments, local paths, hashes, event data) have been redacted.",\n'

  printf '  "email_messages": '
  sqlite_json "$EMAIL_QUERY"
  printf ',\n'

  printf '  "email_attachments": '
  sqlite_json "$ATT_QUERY"
  printf ',\n'

  printf '  "mailbox_checkpoints": '
  sqlite_json "$CP_QUERY"
  printf ',\n'

  printf '  "notification_log": '
  sqlite_json "$NL_QUERY"
  printf ',\n'

  printf '  "event_log": '
  sqlite_json "$EV_QUERY"
  printf '\n'

  printf '}\n'
} > "$OUTPUT"

SIZE_KB=$(( $(wc -c < "$OUTPUT") / 1024 ))
echo "Done. Output: $OUTPUT (${SIZE_KB}KB)" >&2
