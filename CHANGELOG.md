# Changelog

Все значимые изменения проекта. Формат основан на
[Keep a Changelog](https://keepachangelog.com/ru/1.1.0/),
версионирование — [Semantic Versioning](https://semver.org/lang/ru/).

## [0.1.1] - 2026-10-05

### Добавлено
- Перебор серверов Telegram по IP: встроенный список живых адресов с коротким
  таймаутом подключения (2.5 с), DNS — последний фолбэк. Свой список задаётся
  через `CFG_TG_API_IPS` в `main/secrets.h`.
- Разбор HTTP-статуса ответов Bot API: явное сообщение в лог и бэкофф 15 с при
  `409 Conflict` (второй потребитель на том же токене бота).
- Команда `/fool ttl|md5sig|badsum|badseq|none` — смена fooling без перепрошивки.
- `/status` показывает текущий сервер Telegram и последний HTTP-статус.
- `INSTALL_RU.md` — короткая инструкция для получателя архива.
- `DISCLAIMER.md`, `CHANGELOG.md`, `CONTRIBUTING.md`, GitHub Actions.

### Изменено
- `esp_desync_connect()` разделён на `esp_desync_resolve()` и
  `esp_desync_connect_ip()`: коннект на конкретный IPv4 при TLS-хостнейме
  `api.telegram.org`.
- `tools/win-build.ps1`: параметры путей без привязки к конкретной машине.

### Исправлено
- Многодесятковсекундное ожидание при мёртвом адресе из DNS.

## [0.1.0] - 2026-10-05

Первый выпуск.
- Компонент `esp_desync`: режимы `fake`, `split`, `disorder`, `fake_split`, `tlsrec`,
  fooling `TTL`/`MD5SIG`/`BADSUM`/`BADSEQ`, инъекция через `ip4_output_if()`.
- Telegram long-poll через mbedTLS поверх `esp_desync`, Wake-on-LAN, команды
  `/wake`, `/status`, `/desync`, `/ttl`.
- Сборка под ESP-IDF v5.3.6, цель esp32.
