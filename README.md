# esp32-zapret

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![tests](https://github.com/Fairen8/esp32-zapret/actions/workflows/tests.yml/badge.svg)](https://github.com/Fairen8/esp32-zapret/actions/workflows/tests.yml)
[![release](https://img.shields.io/github/v/release/Fairen8/esp32-zapret?include_prereleases&label=release)](https://github.com/Fairen8/esp32-zapret/releases)
[![OpenSSF Baseline](https://www.bestpractices.dev/projects/15251/baseline)](https://www.bestpractices.dev/projects/15251)
[![OpenSSF Best Practices](https://www.bestpractices.dev/projects/15251/badge)](https://www.bestpractices.dev/projects/15251)
[![CodeQL](https://github.com/Fairen8/esp32-zapret/actions/workflows/codeql.yml/badge.svg)](https://github.com/Fairen8/esp32-zapret/actions/workflows/codeql.yml)
[![OpenSSF Scorecard](https://api.securityscorecards.dev/projects/github.com/Fairen8/esp32-zapret/badge)](https://securityscorecards.dev/viewer/?uri=github.com/Fairen8/esp32-zapret)

Мини-аналог [zapret](https://github.com/bol-van/zapret) для ESP32: обход
SNI-блокировок DPI/ТСПУ **самим устройством**, без сторонних серверов и VPN.
Проект вырос из практической задачи: телеграм-бот на ESP32 должен отправлять
Wake-on-LAN magic packet на ПК, но Telegram блокируется провайдером.

Компонент `esp_desync` перехватывает TLS ClientHello исходящего соединения и
«дурит» DPI теми же приёмами, что и zapret: `fake`/`split`/`disorder`/`tlsrec` +
fooling (`TTL`, `MD5SIG`, `BADSUM`, `BADSEQ`). Бот умеет `/wake`, `/status` и
тюнинг на лету, без перепрошивки.

> Это независимая реализация, **не форк кода** zapret и не аффилирована с его
> автором. См. [DISCLAIMER.md](DISCLAIMER.md).

```
ESP32 (WROOM-32) ── Wi-Fi ── роутер ── ISP/TSPU (DPI) ── api.telegram.org
       │                        │
       │  ClientHello:          │  Фейк с TTL=3 виден DPI, но умирает по пути
       │  [fake(iana.org)]──────▶│  и до сервера не доходит
       │  [реальный hello]──────▶│  DPI уже «разрешил» поток
```

## Возможности

- Обход SNI-блокировок на самом ESP32 (LwIP + mbedTLS), без прокси и VPN.
- 5 режимов десинхронизации + 4 способа fooling, переключение через Telegram.
- Перебор известных IP серверов Telegram — лечит мёртвые ответы DNS и блокировку по IP.
- Wake-on-LAN по команде из Telegram.
- Минимальный HTTPS-клиент Bot API на чистом mbedTLS поверх кастомного BIO.
- Открытый исходный код, лицензия MIT.

## Совместимость

| | |
|---|---|
| Чип | ESP32 / ESP32-S3 / ESP32-C3 (релизы собираются для всех трёх; полевые тесты на ESP32-WROOM-32) |
| ESP-IDF | ≥ 5.1 (CI собирает на v5.3.6) |
| Flash | 4 МБ (`SINGLE_APP_LARGE`) |
| Wi-Fi | 2.4 ГГц, WPA2 |
| Сеть | только IPv4, TLS через mbedTLS |

Компонент `esp_desync` содержит манифест `idf_component.yml` — его можно
подключить как обычный компонент ESP-IDF или опубликовать в
[ESP Component Registry](https://components.espressif.com).

## Как это работает

Обычный способ применения zapret на Linux — прозрачный прокси/nfqueue. На ESP32 мы
**сами являемся endpoint'ом**, поэтому всё проще и изящнее:

1. Обычный TCP-коннект к `api.telegram.org:443` через LwIP.
2. Перед отправкой ClientHello компонент через `tcp_active_pcbs` (в контексте
   tcpip-таска) читает `snd_nxt`/`rcv_nxt` сокета — точные sequence/ack.
   Никакого sniffing'а не нужно.
3. Собирает TLS ClientHello-фейк с подставным SNI (`www.iana.org` по умолчанию)
   и инжектит его **вне TCP-потока** через `ip4_output_if()` — пакет уходит
   нормальным Wi-Fi TX-трактом (аппаратное шифрование WPA2, NAT) с исходным seq,
   как у zapret (`--dpi-desync=fake`).
4. Fooling не даёт фейку дойти до сервера: по умолчанию TTL=3 (пакет умирает по
   пути, но DPI его видит). DPI считает поток разрешённым, сервер получает только
   настоящий ClientHello.
5. Далее TLS-сессия идёт через mbedTLS с кастомным BIO поверх `esp_desync_write()`.

### Почему не сырые 802.11-кадры

Изначальная идея (`esp_wifi_80211_tx`) имеет два неизвестных: драйвер официально
ограничен типами кадров, а шифрование data-кадров под WPA2 не документировано —
AP может просто дропнуть незашифрованный fake. Инъекция через `ip4_output_if()`
лишена этих проблем: это тот же путь, которым LwIP отправляет обычный TCP, плюс
полный контроль seq/ack/TTL/чексуммы.

## Режимы

| Режим | Аналог в zapret | Что делает |
|---|---|---|
| `off` | — | без модификаций |
| `split` | `multisplit` | режет ClientHello на TCP-сегменты (по умолчанию в начале и середине SNI), с паузой |
| `disorder` | `multidisorder` | хвост ClientHello уходит raw-пакетом **до** головы (в сокет-поток он досылается дублем, чтобы не ломать LwIP) |
| `fake` | `fake` | инжектит фейковый ClientHello с исходным seq |
| `fake_split` | `fake,split2` | **дефолт**: фейк + нарезка реального |
| `tlsrec` | `tlsrec` (tpws) | переписывает ClientHello в две TLS-записи, чтобы DPI не собрал SNI |
| `seqovl` | `seqovl` | фейк со сдвинутым назад seq перекрывает голову потока (`CONFIG_ESP_DESYNC_SEQOVL_LEN`, по умолчанию 32 байта) |

Fooling для фейка: `TTL` (дефолт), `MD5SIG` (Linux-серверы молча дропают пакет с
TCP MD5 option), `BADSUM` (не проходит через домашние NAT с conntrack-проверкой
чексумм), `BADSEQ` (seq выводится из окна), `DATANOACK` (без флага ACK),
`TS` (TCP timestamps со случайным значением; комбинируется с TTL). По умолчанию
Kconfig, на лету — команда `/fool`. `rndsni` (`/rndsni on`) подставляет
случайный decoy-SNI в каждый фейк, так что перестаёт быть постоянным и размер
пакета.

## Авто-подбор параметров и мониторинг

Начиная с v1.0.0 устройство **само определяет оптимальные параметры работы**:

- при загрузке перебирает стратегии от простой к сложной (`off` → `fake_split`
  с TTL 3/5/8/12 → альтернативные decoy-SNI → `md5sig`/`badseq`/`datanoack` →
  `split`/`disorder`/`tlsrec`) и проверяет каждую реальным TLS-коннектом к
  api.telegram.org; первая рабочая сохраняется в NVS и проверяется первой при
  следующих загрузках;
- каждые 10 минут проверяет, что обход всё ещё работает; после 2 неудач подряд
  запускает перебор заново (провайдеры меняют фильтрацию);
- если DNS отдаёт мусор — резолвит адреса Telegram через **DoH**
  (Cloudflare/Google).

Ручное управление: `/scan` — перезапустить перебор, `/strategy` — текущая
стратегия и статистика. Ручные `/desync`, `/ttl`, `/fool`, `/rndsni`
сохраняются в NVS (переживают перезагрузку) и имеют приоритет над автоподбором:
health-проверки не сбрасывают их, они снимаются только командой `/scan` или
после двух неудачных проверок подряд. Текущий режим виден в `/status`
(`manual`/`auto`).

## Анонимная статистика

Начиная с v1.0.1 прошивка умеет отправлять небольшой анонимный отчёт на
`statistics.fairen8.ru` — чтобы автор видел, какие стратегии реально работают
у разных провайдеров. Начиная с v1.1.1 отправка **включена по умолчанию**.

Что отправляется: версия прошивки, чип, аптайм, RSSI, режим/фулинг/TTL,
сплит-позиции, выбранная стратегия и счётчики проб. Идентификаторы
(ID устройства или чата, SSID, IP-адреса) не отправляются.

Выключить: `/stats off` (сохраняется в NVS) или тумблер в веб-UI; включить
обратно — `/stats on`. Собрать с выключенной по умолчанию статистикой:
`CONFIG_APP_STATS_DEFAULT_ON=n`. Отчёт отправляется при загрузке
и не чаще раза в сутки; если сервер недоступен — попытка молча пропускается.

Формат отчёта и требования к серверу: [docs/STATS_BACKEND_SPEC.md](docs/STATS_BACKEND_SPEC.md).

## Быстрый старт

Готовые сборки — в [Releases](https://github.com/Fairen8/esp32-zapret/releases):
merged-образы для **esp32**, **esp32s3**, **esp32c3** и вариант **без бота**
(`esp32-nobot`, периодический TLS self-test через esp_desync), плюс архив с
исходниками.

Начиная с v1.1.0 **тулчейн не нужен**: прошейте образ, при первом включении
плата поднимет точку доступа `esp32-zapret-XXXX` (пароль `zapret12345`),
подключитесь к ней и откройте **http://192.168.4.1** — там задаются Wi-Fi,
токен бота, MAC ПК и новый пароль. Дальше те же настройки доступны по IP
устройства (логин `admin`). Подробно — [INSTALL_RU.md](INSTALL_RU.md).

Сборка из исходников (для разработки):

```bash
git clone https://github.com/Fairen8/esp32-zapret.git
cd esp32-zapret
cp main/secrets_example.h main/secrets.h   # значения по умолчанию
idf.py set-target esp32
idf.py menuconfig                          # Component config -> esp_desync anti-DPI
idf.py build flash monitor
```

> **Важно:** `CFG_TG_ADMIN_ID=0` означает «принимать команды от кого угодно» —
> любой, кто узнает вашего бота, сможет будить ПК и менять настройки обхода.
> Укажите свой числовой chat id (узнать: @userinfobot) — это фактически пароль
> устройства.

### Веб-интерфейс и первичная настройка

- Если устройство не настроено (или прошито готовым образом), оно вместо
  зависания поднимает setup-точку доступа и веб-страницу `http://192.168.4.1`;
  в no-bot сборке токен не требуется.
- После настройки сервер работает по IP устройства: статус, Wi-Fi (включая
  статический IP и сброс привязки BSSID), бот, Wake-on-LAN, режим/TTL/fooling,
  автоподбор, статистика, перезагрузка и сброс настроек. Защита — HTTP Basic
  (`admin` / пароль веб-интерфейса).
- Альтернатива — serial-консоль: `setwifi`, `settoken`, `setadmin`, `setmac`,
  `setwebpass`, `status`, `reboot`, `erase`.
- Отключается через `CONFIG_APP_WEB_UI=n`.

### Windows: кириллица в пути

ESP-IDF (kconfgen) падает, если путь к проекту содержит не-ASCII символы
(`C:\Users\Иван\Documents\...`). Если проект лежит в таком пути — используйте
хелпер, он зеркалит проект в ASCII-каталог и собирает там:

```
powershell -ExecutionPolicy Bypass -File tools\win-build.ps1 -Action build
powershell -ExecutionPolicy Bypass -File tools\win-build.ps1 -Action flash-monitor -Port COM5
```

По умолчанию хелпер ожидает раскладку `D:\esp32-zapret\{esp-idf,python,tools,work,build}`;
все пути переопределяются параметрами (`-IdfPath`, `-ToolsPath`, `-PythonDir`,
`-WorkDir`, `-BuildDir`).

Короткая инструкция для передачи проекта другому человеку — [INSTALL_RU.md](INSTALL_RU.md).

### ESP32-C3 / ESP32-S3: консоль

Для C3/S3 сборка использует нативный **USB Serial/JTAG** как основную консоль
(`sdkconfig.defaults.esp32c3` / `sdkconfig.defaults.esp32s3`): подключайте кабель
к USB-порту самого чипа — `idf.py monitor` покажет лог. На платах с внешним
USB-UART-мостом (CP210x/CH340) верните `CONFIG_ESP_CONSOLE_UART_DEFAULT=y` в
`menuconfig`.

## Команды бота

```
/wake [AA:BB:CC:DD:EE:FF]   отправить magic packet (без аргумента — MAC из настроек)
/status                     uptime, heap, RSSI, режим/TTL/fooling/rndsni, IP и HTTP-статус Telegram
/desync <mode>              off | split | disorder | fake | fake_split | tlsrec | seqovl
/ttl <1..255>               TTL фейка
/fool <mode>                ttl | md5sig | badsum | badseq | datanoack | ts | none
/rndsni on|off              случайный decoy-SNI (и размер) для каждого фейка
/scan                       перезапустить автоподбор (снимает ручные настройки)
/strategy                   текущая стратегия и статистика проб
/stats on|off               анонимная статистика (по умолчанию включена)
/heap                       свободная/минимальная heap и крупнейший блок
/ip                         IP, шлюз, SSID, RSSI
/reboot                     перезагрузка
```

### Как подобрать TTL

TTL — главный параметр. Фейк должен дойти до DPI, но не до сервера:

1. Начните с `fake_split` и `/ttl 3`.
2. Если соединение всё ещё рвётся (DPI не обманут) — **увеличивайте** TTL, пока
   обход не заработает: фейк не долетал до DPI.
3. Если рукопожатие ломается/виснет (фейк долетел до сервера и испортил
   handshake) — **уменьшайте** TTL.
4. Минимальный работающий TTL ≈ номер хопа вашего DPI (методика из документации
   zapret).

Если TTL не помогает — попробуйте `/fool md5sig`, затем `ts`, `fake` без split,
затем `disorder`, `tlsrec`, `seqovl`, затем `/rndsni on`. Набор перебирается за
пару минут через Telegram.

## Структура

```
components/esp_desync/
  esp_desync.c        оркестратор: connect / write(desync) / read, режимы
  desync_tls.c        парсер ClientHello, поиск SNI, генерация фейка
  desync_pcb.c        snd_nxt/rcv_nxt сокета через tcp_active_pcbs + tcpip_callback
  desync_inject.c     сборка TCP-сегмента + ip4_output_if (TTL/badsum/md5sig/datanoack)
main/
  main.c              Wi-Fi/SNTP/long-poll, команды, BSSID pinning, static IP
  app_settings.c      runtime-настройки в NVS (Wi-Fi/токен/WoL/веб-пароль)
  setup_mode.c        setup-режим: AP + serial-консоль (setwifi/…)
  webui.c             веб-сервер: первичная настройка и полные настройки
  desync_scan.c       автоподбор стратегий, health-check, NVS
  telegram.c          минимальный HTTPS-клиент на mbedTLS через esp_desync
  wol.c               magic packet
tools/win-build.ps1   сборка на Windows из пути с кириллицей
```

## Ограничения

- Только IPv4 и TLS (HTTP/QUIC не обрабатываются).
- Один потребитель на токен: если параллельно работает другой клиент Bot API
  (скрипт, второй бот), Telegram отдаёт `409 Conflict` через 4-7 секунд. Прошивка
  пишет это в лог и уходит в бэкофф — для ESP заведите отдельного бота у @BotFather.
- DNS может отдавать мёртвые адреса Telegram (блокировка по IP). Прошивка
  перебирает встроенный список известных IP (`149.154.167.220`, `.167.191`,
  `.175.50`, ...) с коротким таймаутом и только потом DNS; список переопределяется
  в `main/secrets.h` через `CFG_TG_API_IPS`. Текущий адрес виден в `/status`.
- `BADSUM` бесполезен за домашним роутером (conntrack дропает invalid-пакеты) —
  используйте TTL/MD5SIG.
- Некоторые стоковые прошивки роутеров фиксируют исходящий TTL — тогда TTL-фулинг
  не работает (см. документацию zapret).
- Блокировка по IP обходом на уровне DPI не лечится — нужен прокси/VPS или живой
  адрес из списка.
- Параллельные соединения поддерживаются (таблица «вооружённых» потоков на 4
  сокета), но бот по-прежнему работает последовательно.
- Цель — DPI, интерпретирующий поток ограниченно; полноценный TCP-стек
  (прозрачный прокси/Squid) не обмануть.

## Roadmap

- [x] Переключение режима/TTL/fooling на лету (Telegram)
- [x] Pin/failover серверов Telegram по IP
- [x] `fake` с несколькими SNI и `rndsni`
- [x] seqovl-перекрытие
- [x] fooling `ts` (TCP timestamps, как ALT1 в zapret)
- [x] DoH / DNS-антиподмена
- [x] настройки в NVS + Web UI (v1.1.0)
- [x] ESP32-S3/C3 (релизы и полевая проверка; консоль USB Serial/JTAG)
- [ ] Публикация `esp_desync` в ESP Component Registry (workflow готов, нужен токен)

## CI/CD и релизы

- Пуш в `main` запускает набор задач: юнит-тесты (хостовые), статанализ, гигиена
  репозитория (нет секретов/бинарников, `VERSION` = `CHANGELOG`) и сборка ESP-IDF
  в двух конфигурациях.
- Релиз идёт через PR `main` → `releases`: те же проверки выполняются как
  обязательные, а после мержа автоматически публикуется релиз по файлу `VERSION`.
- Готовые образы настраиваются без пересборки (setup-AP + веб-интерфейс).
- Если задан секрет `IDF_COMPONENT_API_TOKEN`, релиз дополнительно публикует
  компонент `esp_desync` в ESP Component Registry.
- Публикация идемпотентна: если тег `vX.Y.Z` уже существует, шаг релиза
  пропускается. Артефакты: `esp32-zapret-merged.bin` и архив с исходниками,
  `INSTALL_RU.md` и прошивкой.
- Выпуск версии: поднять `VERSION`, добавить секцию в `CHANGELOG.md`, открыть PR
  в `releases`, дождаться зелёных проверок и смержить.

## Документация

- [docs/QUICKSTART.md](docs/QUICKSTART.md) — быстрый старт (EN)
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — архитектура и поток данных
- [docs/SECURITY_DESIGN.md](docs/SECURITY_DESIGN.md) — модель угроз и защита
- [docs/ASSURANCE_CASE.md](docs/ASSURANCE_CASE.md) — assurance case
- [docs/GOVERNANCE.md](docs/GOVERNANCE.md) — управление, роли, континуитет
- [docs/CODING_STANDARDS.md](docs/CODING_STANDARDS.md) — стандарты кода
- [docs/MAINTENANCE.md](docs/MAINTENANCE.md) — релизы и поддержка
- [docs/ROADMAP.md](docs/ROADMAP.md) — планы развития

## Достижения

- OpenSSF Best Practices: **baseline-1** и **passing** (проект 15251).
- CodeQL (0 алертов), фаззинг TLS-парсера (libFuzzer + ASan/UBSan),
  статанализ, secret scanning с push protection.
- Релизы собираются в CI и публикуются с подписанным SLSA provenance.
- OpenSSF Scorecard автоматически оценивает репозиторий.

## Безопасность и право

Проект предназначен для использования на собственных устройствах и в собственных
сетях, а также в образовательных и исследовательских целях. Вы самостоятельно
отвечаете за соблюдение законов вашей юрисдикции, правил провайдера и условий
использования сервисов. Подробнее — [DISCLAIMER.md](DISCLAIMER.md).

## Благодарности

- [zapret](https://github.com/bol-van/zapret) (MIT, bol-van) — идейная основа,
  терминология режимов, методика подбора TTL.
- [tpws](https://github.com/bol-van/zapret) из состава zapret — идея `tlsrec`.
- LwIP, mbedTLS, ESP-IDF.

## Лицензия

MIT — см. [LICENSE](LICENSE). © 2026 esp32-zapret contributors.
