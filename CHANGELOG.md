# Changelog

Все значимые изменения проекта. Формат основан на
[Keep a Changelog](https://keepachangelog.com/ru/1.1.0/),
версионирование — [Semantic Versioning](https://semver.org/lang/ru/).

## [0.2.2] - 2026-10-05

### Добавлено
- Обязательное ревью перед релизом: protected environment `release` с
  обязательным одобрением владельца (админ-обход отключён), environment
  разрешён только из ветки `releases`. Паблиш-джоба ждёт Approve.
- Ветка `community` для PR от сообщества (баги/идеи): обязательное одобрение
  владельца, 5 проверок, прямые коммиты запрещены.
- Автоназначение ревьюера: PR в `community`/`releases` от сторонних авторов
  автоматически пингуют @Fairen8 и запрашивают его ревью.
- CodeQL (C/C++, `build-mode: none`) и OpenSSF Scorecard с публикацией в
  code scanning.
- `CODE_OF_CONDUCT.md` (Contributor Covenant 2.1), шаблоны issue (bug/feature),
  бейджи CodeQL/Scorecard.
- Ограничение разрешённых GitHub Actions: GitHub-owned + явно перечисленные
  действия (espressif, softprops, ossf).

### Изменено
- Шаблон PR упрощён: ручные чекбоксы убраны (проверки делает CI).
- Теги `v*` защищены от удаления и перезаписи (ruleset).
- Scorecard: `ossf/scorecard-action` запинен на `v2.4.4` (тега `v2` не существует).
- Требование «ветка не отстаёт» (strict) отключено для защищённых ветвей.

### Исправлено
- Scorecard падал из-за неразрешимого тега; CodeQL и Scorecard теперь зелёные.

## [0.2.1] - 2026-10-05

### Изменено
- GitHub Actions обновлены через Dependabot: `actions/checkout` v7,
  `actions/upload-artifact` v7, `actions/download-artifact` v8.
- Репозиторий открыт публично и защищён: branch protection для `releases`
  (5 обязательных проверок, изменения только через PR, force-push и удаление
  запрещены), правила защиты тегов `v*`, secret scanning с push protection,
  Dependabot alerts и private vulnerability reporting.

### Добавлено
- `SECURITY.md` и `.github/dependabot.yml`.

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
