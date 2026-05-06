#!/usr/bin/env bash
set -euo pipefail

python3 -m py_compile browser-worker/main.py

if ! curl -fsS http://127.0.0.1:8090/health >/tmp/catch-letter-browser-health.json; then
  echo "browser-worker is not running on 127.0.0.1:8090; start docker compose or uvicorn first"
  exit 0
fi

python3 - <<'PY'
import json, urllib.request

def post(path, payload):
    data = json.dumps(payload).encode()
    req = urllib.request.Request("http://127.0.0.1:8090" + path, data=data, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=60) as resp:
        return json.load(resp)

inspect = post("/inspect-form", {"url": "http://127.0.0.1:8090/demo-form", "debug": True})
assert inspect.get("ok"), inspect
fields = inspect.get("fields", [])
rating = [f for f in fields if f.get("type") == "radio_group" and "Оценка" in f.get("label", "")]
assert rating, fields
labels = {o.get("label", o) if isinstance(o, dict) else o for o in rating[0].get("options", [])}
assert {"5", "4", "3"}.issubset(labels), labels
assert any(f.get("type") == "checkbox_group" for f in fields), fields
sid = inspect["session_id"]
by_label = {f.get("label", ""): f for f in fields}
rating_field = rating[0]
interests_field = next(f for f in fields if f.get("type") == "checkbox_group")
fill = post("/fill-form", {"session_id": sid, "fields": [
    {"id": by_label["ФИО"].get("id"), "selector": by_label["ФИО"].get("selector"), "value": "Иванов Иван"},
    {"id": by_label["Email"].get("id"), "selector": by_label["Email"].get("selector"), "value": "ivan@example.com"},
    {"id": by_label["Группа"].get("id"), "selector": by_label["Группа"].get("selector"), "value": "БПИ000"},
    {"id": rating_field.get("id"), "selector": rating_field.get("selector"), "value": "5"},
    {"id": interests_field.get("id"), "selector": interests_field.get("selector"), "value": "Мероприятия;Карьера"}
]})
assert fill.get("ok"), fill
submit = post("/submit-form", {"session_id": sid})
assert submit.get("ok") and submit.get("submitted"), submit

auth = post("/inspect-form", {"url": "http://127.0.0.1:8090/demo-auth-form", "debug": True})
assert auth.get("ok") and auth.get("auth_required"), auth
sid = auth["session_id"]
creds = post("/auth/credentials", {"session_id": sid, "username": "demo", "password": "demo"})
assert creds.get("ok") and creds.get("status") == "waiting_2fa", creds
twofa = post("/auth/2fa", {"session_id": sid, "code": "123456"})
assert twofa.get("ok") and twofa.get("status") == "authenticated", twofa
reinspect = post("/reinspect-form", {"session_id": sid})
assert reinspect.get("ok") and reinspect.get("fields"), reinspect
post("/close-session", {"session_id": sid})
print("browser-worker smoke ok")
PY
