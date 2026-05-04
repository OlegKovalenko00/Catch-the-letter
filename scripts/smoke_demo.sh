#!/usr/bin/env bash
set -euo pipefail

TOKEN_ARG=""
if [[ -n "${WEB_AUTH_TOKEN:-}" ]]; then
  TOKEN_ARG="?token=${WEB_AUTH_TOKEN}"
fi

echo "Start services first:"
echo "  docker compose up --build"
echo
echo "Create normal demo form:"
curl -fsS -X POST "http://127.0.0.1:8080/api/demo/create${TOKEN_ARG}"
echo
echo "Active forms should include waiting_user_review:"
curl -fsS "http://127.0.0.1:8080/api/forms/active${TOKEN_ARG}"
echo
echo "Create auth demo form:"
curl -fsS -X POST "http://127.0.0.1:8080/api/demo/create-auth${TOKEN_ARG}"
echo
echo "Active forms should include waiting_auth. Complete demo/demo and 123456 in Web UI."
