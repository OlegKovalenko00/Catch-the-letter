# Catch the Letter

Приложение для мониторинга входящих писем по IMAP/IMAPS, применения правил важности и отправки уведомлений (Telegram/SMS/звонок), с дедупликацией в SQLite и локальным Web UI.

## Возможности

- IMAP/IMAPS мониторинг входящих писем с периодическим опросом.
- Правила фильтрации: `contains`, `equals`, `regex`, режимы `all/any`.
- Уведомления: Telegram, SMS и звонок через Twilio, а также `console`.
- Дедупликация и журналирование результатов в SQLite.
- Локальный Web UI `http://127.0.0.1:8080` для статуса и редактирования правил.

## Требования

- CMake 3.16+.
- Компилятор C++17.
- libcurl (с TLS) и SQLite3 dev пакеты.
- Git и интернет для `FetchContent` (nlohmann/json и cpp-httplib).

## Сборка и запуск (Linux)

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake libcurl4-openssl-dev libsqlite3-dev

cmake -S . -B build
cmake --build build -j

./build/catch_the_letter --config config/app.json
```

## Сборка и запуск (Windows через vcpkg)

```powershell
git clone https://github.com/microsoft/vcpkg
.\vcpkg\bootstrap-vcpkg.bat
.\vcpkg\vcpkg install curl[openssl] sqlite3

cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=$PWD\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Release

.\build\Release\catch_the_letter.exe --config config\app.json
```

Если используешь другой способ установки зависимостей, добавь флаг `-DUSE_SYSTEM_DEPS=ON`.

## Настройка

1. Скопируй пример конфига:

```bash
cp config/app.example.yaml config/app.json
cp config/rules.example.json config/rules.json
```

`app.example.yaml` содержит JSON и читается как JSON.

2. Укажи параметры IMAP и токены в `config/app.json`.
3. Секреты можно хранить в переменных окружения:

- `IMAP_PASSWORD`
- `TELEGRAM_BOT_TOKEN`
- `TELEGRAM_CHAT_ID`
- `TWILIO_ACCOUNT_SID`
- `TWILIO_AUTH_TOKEN`

Пример для PowerShell:

```powershell
$env:IMAP_PASSWORD="your_app_password"
$env:TELEGRAM_BOT_TOKEN="123:ABC"
$env:TELEGRAM_CHAT_ID="123456789"
```

### Пример IMAP для Gmail

- Включи IMAP в настройках Gmail.
- Если включена 2FA, создай App Password и используй его как `IMAP_PASSWORD`.
- Host: `imap.gmail.com`, Port: `993`, TLS: `true`.

## Правила

Файл `config/rules.json` поддерживает формат:

```json
{
  "rules": [
    {
      "id": "r1",
      "name": "University important",
      "enabled": true,
      "priority": "important",
      "match": "all",
      "conditions": [
        {"field": "from", "op": "contains", "value": "@university"},
        {"field": "subject", "op": "contains", "value": "Important"}
      ],
      "actions": [
        {"type": "notify", "channel": "telegram", "text": "Важное письмо: {{subject}}"}
      ]
    }
  ]
}
```

Доступные поля условия: `from`, `to`, `subject`, `snippet`, `body`.

## Web UI

После запуска открой `http://127.0.0.1:8080`. Там можно посмотреть статус и редактировать правила.

## Docker

```bash
docker compose up --build
```

Перед запуском создай `config/app.json` и `config/rules.json`, и задай переменные окружения в `docker-compose.yml`.

## Быстрый демо-режим

```bash
./build/catch_the_letter --demo
```

Запустит мок-данные без подключения к почте.
