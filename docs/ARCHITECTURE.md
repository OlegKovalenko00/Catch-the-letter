# Архитектура

Модули:

- `infra/MailClientImap` — IMAP/IMAPS клиент через libcurl.
- `domain/RuleEngine` — проверка правил (contains/equals/regex, all/any).
- `infra/TelegramNotifier` и `infra/TwilioNotifier` — каналы уведомлений.
- `infra/SqliteStorage` — дедупликация и журнал уведомлений.
- `infra/HttpServer` — локальный Web UI и API.
- `app/App` — оркестрация: опрос, правила, уведомления, ретраи.

Поток данных:

- IMAP получает новые письма.
- Правила применяются к письмам.
- Для совпадений создаются уведомления.
- Результаты пишутся в SQLite.
- Статус и правила доступны в Web UI.
