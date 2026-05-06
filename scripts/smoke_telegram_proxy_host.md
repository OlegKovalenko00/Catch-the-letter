# Telegram proxy / host-network smoke

Use this when Telegram works on the Linux host through a local VPN/proxy, but times out from Docker bridge.

```bash
TOKEN="..."

curl -x "http://127.0.0.1:10809" \
  -m 15 "https://api.telegram.org/bot${TOKEN}/getMe"

docker compose -f docker-compose.yml -f docker-compose.vpn.yml config \
  | grep -E "network_mode|TELEGRAM_PROXY_URL|BROWSER_WORKER_ENDPOINT"

docker compose -f docker-compose.yml -f docker-compose.vpn.yml up --build

curl -X POST http://127.0.0.1:8080/api/test/telegram
```

Expected compose config contains:

```text
network_mode: host
BROWSER_WORKER_ENDPOINT: http://127.0.0.1:8090
TELEGRAM_PROXY_URL: http://127.0.0.1:10809
```
