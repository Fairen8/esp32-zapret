# Changelog

Все значимые изменения проекта. Формат основан на
[Keep a Changelog](https://keepachangelog.com/ru/1.1.0/),
версионирование — [Semantic Versioning](https://semver.org/lang/ru/).

## [0.2.0] - 2026-10-05

### Добавлено
- Автосборка релизов: PR `main` → `releases` прогоняет полный набор проверок, а
  после мержа публикуется релиз по файлу `VERSION` (идемпотентно по тегу).
- GitHub Actions: четыре задачи — юнит-тесты на хосте, статанализ
  (`gcc -fanalyzer`), гигиена репозитория и матрица сборок ESP-IDF
  (default и disorder+md5sig).
- Хостовые тесты: парсер/генератор TLS ClientHello, разбор списка IP, MAC и
  URL-кодирование (`tests/host/run.sh`).
- `VERSION` — единый источник версии для релизов.

### Изменено
- Разбор IP/MAC/URL вынесен из `telegram.c`/`wol.c` в тестируемый `net_utils`.
- TLS API вынесен в `desync_tls.h` без зависимости от LwIP (для хостовых тестов).
- Релизный архив собирается в CI и включает исходники, `INSTALL_RU.md` и прошивку.

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
