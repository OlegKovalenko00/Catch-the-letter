#!/usr/bin/env bash
set -euo pipefail

patterns='(123456|password[[:space:]_:-]*demo|demo[[:space:]_:-]*password|TELEGRAM_BOT_TOKEN|IMAP_PASSWORD|authorization:|cookie:|token=|token:)'
targets=()

if [[ -d data ]]; then
  while IFS= read -r -d '' file; do targets+=("$file"); done < <(find data -type f \( -name '*.db' -o -name '*.sqlite' -o -name '*.log' \) -print0)
fi

if ((${#targets[@]})); then
  if grep -aE "$patterns" "${targets[@]}" >/tmp/catch-letter-secret-hits.txt; then
    echo "secret-like values found:"
    cat /tmp/catch-letter-secret-hits.txt
    exit 1
  fi
else
  echo "SKIPPED data scan: no local SQLite/log files under data/"
fi

if docker compose ps >/dev/null 2>&1; then
  if docker compose logs --no-color app browser-worker 2>/dev/null | grep -aE "$patterns" >/tmp/catch-letter-log-secret-hits.txt; then
    echo "secret-like values found in docker logs:"
    cat /tmp/catch-letter-log-secret-hits.txt
    exit 1
  fi
else
  echo "SKIPPED docker logs scan: docker compose is unavailable or project is not running"
fi

echo "secrets smoke ok"
