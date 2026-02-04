# API

Локальный Web UI и API доступны по адресу `http://127.0.0.1:8080`.

## Endpoints

- `GET /` — HTML интерфейс.
- `GET /api/status` — JSON со статусом работы.
- `GET /api/rules` — текущие правила в JSON.
- `POST /api/rules` — обновить правила (тело запроса — JSON).
