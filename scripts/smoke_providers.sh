#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

mkdir -p "$TMP_DIR/config" "$TMP_DIR/data"
cp "$ROOT/config/yandex_forms.map.example.json" "$TMP_DIR/config/yandex_forms.map.json"
cp "$ROOT/config/google_forms.map.example.json" "$TMP_DIR/config/google_forms.map.json"

BIN="${BIN:-$ROOT/build/catch_the_letter}"
if [[ ! -x "$BIN" ]]; then
  echo "Binary not found at $BIN. Run cmake build first." >&2
  exit 1
fi

export YANDEX_FORMS_MAP_FILE="$TMP_DIR/config/yandex_forms.map.json"
export GOOGLE_FORMS_MAP_FILE="$TMP_DIR/config/google_forms.map.json"
export YANDEX_FORMS_DRY_RUN=true
export GOOGLE_FORMS_DRY_RUN=true
export KNOWN_FORMS_BROWSER_FALLBACK=false

yandex_url="https://forms.yandex.ru/u/69f8f79049af47b200b98c44/"
google_url="https://docs.google.com/forms/d/e/FORM_ID_OR_PUBLIC_ID/viewform"

cd "$ROOT"
yandex_out="$("$BIN" --config config/app.example.json --inspect-form-url "$yandex_url")"
echo "$yandex_out" | grep -q '"provider_type": "yandex_forms"'
echo "$yandex_out" | grep -q '"extraction_strategy": "yandex_forms_mapping"'
echo "$yandex_out" | grep -q '"submit_strategy": "yandex_forms_api"'

google_out="$("$BIN" --config config/app.example.json --inspect-form-url "$google_url")"
echo "$google_out" | grep -q '"provider_type": "google_forms"'
echo "$google_out" | grep -q '"extraction_strategy": "google_forms_mapping"'
echo "$google_out" | grep -q '"submit_strategy": "google_forms_response_endpoint"'

echo "provider smoke ok"
