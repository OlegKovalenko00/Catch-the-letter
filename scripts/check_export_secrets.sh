#!/usr/bin/env bash


set -euo pipefail


PATTERNS=(

  '[0-9]{9,10}:[A-Za-z0-9_-]{35,}'

  '"(bot_token|auth_token|password|passwd|secret|api_key|oauth_token|access_token|refresh_token|IMAP_PASSWORD|TELEGRAM_BOT_TOKEN|YANDEX_FORMS_OAUTH_TOKEN)"\s*:\s*"[^"]{4,}'

  'token=[A-Za-z0-9_\-]{8,}'

  'Authorization:\s*(Bearer|Basic)\s+[A-Za-z0-9+/=_\-]{8,}'

  '(Set-Cookie|Cookie):[^\n]{8,}'

  '[A-Za-z0-9+/]{48,}={0,2}'
)


targets=()
if [[ $
  for arg in "$@"; do
    if [[ -f "$arg" ]]; then
      targets+=("$arg")
    elif [[ -d "$arg" ]]; then
      while IFS= read -r -d '' f; do targets+=("$f"); done \
        < <(find "$arg" -type f \( -name '*.json' -o -name '*.db' -o -name '*.sqlite' -o -name '*.log' \) -print0)
    else
      echo "WARNING: $arg not found, skipping" >&2
    fi
  done
else

  DB_DIR="data"
  if [[ -f "${DB_DIR}/export_sanitized.json" ]]; then
    targets+=("${DB_DIR}/export_sanitized.json")
  fi
  while IFS= read -r -d '' f; do targets+=("$f"); done \
    < <(find "${DB_DIR}" -maxdepth 1 -type f \( -name '*.json' -o -name '*.db' -o -name '*.sqlite' \) -print0 2>/dev/null || true)
fi

if [[ ${
  echo "SKIPPED: no files to scan" >&2
  exit 0
fi

echo "Scanning ${#targets[@]} file(s) for credential patterns..." >&2

HITS_FILE="$(mktemp /tmp/ctl-export-check-XXXXXX.txt)"
trap 'rm -f "$HITS_FILE"' EXIT

found=0
for pattern in "${PATTERNS[@]}"; do
  if grep -aEo "$pattern" "${targets[@]}" >> "$HITS_FILE" 2>/dev/null; then
    found=1
  fi
done

if [[ $found -eq 1 ]] && [[ -s "$HITS_FILE" ]]; then
  echo "FAIL: credential-like patterns detected:" >&2

  head -20 "$HITS_FILE" >&2
  total=$(wc -l < "$HITS_FILE")
  if [[ $total -gt 20 ]]; then
    echo "  ... and $((total - 20)) more. Full list: $HITS_FILE (copied before exit)" >&2
    cp "$HITS_FILE" /tmp/ctl-export-check-last.txt
  fi
  echo "" >&2
  echo "If these are false positives, review the export logic in scripts/export_sanitized.sh" >&2
  echo "and ensure all PII columns are redacted before sharing the export." >&2
  exit 1
fi

echo "OK: no credential patterns found in ${#targets[@]} file(s)" >&2
exit 0
