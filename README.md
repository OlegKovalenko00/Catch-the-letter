# Catch the Letter

Сервис читает письма через IMAP, находит важные уведомления и ссылки на формы, подставляет данные из профиля и даёт отправить форму только после проверки.

## Windows

Нужны Docker Desktop и PowerShell.

```powershell
.\scripts\windows\setup.ps1
notepad .env
notepad config\profile.json
.\scripts\windows\start.ps1
```

Открыть: `http://localhost:8080`.

Остановка:

```powershell
.\scripts\windows\stop.ps1
```

## Linux

Нужен Docker Compose.

```bash
cp .env.example .env
cp config/app.example.json config/app.json
cp config/rules.example.json config/rules.json
cp config/profile.example.json config/profile.json
nano .env
nano config/profile.json
./scripts/linux/start.sh
```

Открыть: `http://127.0.0.1:8080`.

Остановка:

```bash
./scripts/linux/stop.sh
```

## Тестирование

Windows:

```powershell
.\scripts\windows\test.ps1
```

Linux:

```bash
./scripts/linux/test.sh
```

Дополнительно в веб-интерфейсе есть проверки IMAP, Telegram, браузерного воркера и демо-формы.

## Функционал

- чтение писем через IMAP;
- поиск важных уведомлений и ссылок на формы;
- автоподстановка значений из `config/profile.json`;
- поддержка Yandex Forms, Google Forms и обычных веб-форм;
- ручная проверка перед отправкой;
- уведомления через веб-интерфейс и Telegram;
- хранение состояния в SQLite.
