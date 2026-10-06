# Security Policy / Политика безопасности

## Reporting a vulnerability / Как сообщить об уязвимости

Приватно, через GitHub Security Advisories:

https://github.com/Fairen8/esp32-zapret/security/advisories/new

(вкладка **Security** → **Report a vulnerability**).

Privately, via GitHub Security Advisories:

https://github.com/Fairen8/esp32-zapret/security/advisories/new

(**Security** tab → **Report a vulnerability**.)

## Response process / Процесс ответа

- Подтверждение получения отчёта — в течение **7 дней**.
- Оценка серьёзности и план исправления — в течение **14 дней**.
- Исправление выпускается через автоматический релизный конвейер
  (`VERSION` → PR в `releases` → Approve → релиз).
- Отчёт может быть опубликован после выпуска исправления.

- Acknowledgement of a report within **7 days**.
- Severity assessment and fix plan within **14 days**.
- Fixes are shipped through the automated release pipeline; the report may be
  published once a fix is available.

## Credit / Благодарности

Мы публично благодарим исследователей, сообщивших об уязвимостях (если они не
предпочли остаться анонимными) — в описании релиза и `CHANGELOG.md`.

Reporters are publicly credited (unless they prefer to remain anonymous) in the
release notes and `CHANGELOG.md`.

## Contact / Контакты

- Мейнтейнер: @Fairen8 — https://github.com/Fairen8
- Приватный канал для уязвимостей / private vulnerability channel:
  https://github.com/Fairen8/esp32-zapret/security/advisories/new

## Scope / Область

- Проект — инструмент обхода DPI для личного использования, распространяется
  «как есть» под лицензией MIT, без гарантий (см. https://github.com/Fairen8/esp32-zapret/blob/main/LICENSE).
- Не прикладывайте к issue реальные токены ботов, пароли Wi-Fi и MAC-адреса.
- Вопросы про блокировки (ТСПУ/DPI), сборку и настройку — это обычные issue,
  не security-отчёты.
