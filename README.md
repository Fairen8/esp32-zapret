# esp32-zapret

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![tests](https://github.com/Fairen8/esp32-zapret/actions/workflows/tests.yml/badge.svg)](https://github.com/Fairen8/esp32-zapret/actions/workflows/tests.yml)
[![release](https://img.shields.io/github/v/release/Fairen8/esp32-zapret?include_prereleases&label=release)](https://github.com/Fairen8/esp32-zapret/releases)

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
| `split` | `multisplit` | режет ClientHello на TCP-сегменты (по умолчанию внутри SNI), с паузой |
| `disorder` | `multidisorder` | хвост ClientHello уходит raw-пакетом **до** головы (в сокет-поток он досылается дублем, чтобы не ломать LwIP) |
| `fake` | `fake` | инжектит фейковый ClientHello с исходным seq |
| `fake_split` | `fake,split2` | **дефолт**: фейк + нарезка реального |
| `tlsrec` | `tlsrec` (tpws) | переписывает ClientHello в две TLS-записи, чтобы DPI не собрал SNI |

Fooling для фейка: `TTL` (дефолт), `MD5SIG` (Linux-серверы молча дропают пакет с
TCP MD5 option), `BADSUM` (не проходит через домашние NAT с conntrack-проверкой
чексумм), `BADSEQ` (seq выводится из окна). По умолчанию Kconfig, на лету —
команда `/fool`.

## Быстрый старт

```bash
git clone https://github.com/Fairen8/esp32-zapret.git
cd esp32-zapret
cp main/secrets_example.h main/secrets.h   # Wi-Fi, токен, MAC ПК
idf.py set-target esp32
idf.py menuconfig                          # Component config -> esp_desync anti-DPI
idf.py build flash monitor
```

Готовые сборки — в [Releases](https://github.com/Fairen8/esp32-zapret/releases):
`esp32-zapret-merged.bin` (прошивать на `0x0`) и архив. Бинарники в релизах
собраны **с заглушками** (без вашего Wi-Fi/токена) — для реальной работы нужна
сборка со своим `secrets.h`.

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

## Команды бота

```
/wake [AA:BB:CC:DD:EE:FF]   отправить magic packet (без аргумента — MAC из secrets.h)
/status                     uptime, heap, RSSI, режим/TTL/fooling, IP и HTTP-статус Telegram
/desync <mode>              off | split | disorder | fake | fake_split | tlsrec
/ttl <1..255>               TTL фейка
/fool <mode>                ttl | md5sig | badsum | badseq | none
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

Если TTL не помогает — попробуйте `/fool md5sig`, затем `fake` без split, затем
`disorder`, `tlsrec`. Набор перебирается за пару минут через Telegram.

## Структура

```
components/esp_desync/
  esp_desync.c        оркестратор: connect / write(desync) / read, режимы
  desync_tls.c        парсер ClientHello, поиск SNI, генерация фейка
  desync_pcb.c        snd_nxt/rcv_nxt сокета через tcp_active_pcbs + tcpip_callback
  desync_inject.c     сборка TCP-сегмента + ip4_output_if (TTL/badsum/md5sig/datanoack)
main/
  main.c              Wi-Fi, SNTP, long-poll, команды
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
- Один активный «вооружённый» коннект за раз (бот работает последовательно).
- Цель — DPI, интерпретирующий поток ограниченно; полноценный TCP-стек
  (прозрачный прокси/Squid) не обмануть.

## Roadmap

- [x] Переключение режима/TTL/fooling на лету (Telegram)
- [x] Pin/failover серверов Telegram по IP
- [ ] `fake` с несколькими SNI и `rndsni`
- [ ] seqovl-перекрытие
- [ ] fooling `ts` (TCP timestamps, как ALT1 в zapret)
- [ ] DoH / DNS-антиподмена
- [ ] настройки в NVS + Web UI
- [ ] ESP32-S3/C3 (LwIP-часть работает без изменений)

## CI/CD и релизы

- Пуш в `main` запускает набор задач: юнит-тесты (хостовые), статанализ, гигиена
  репозитория (нет секретов/бинарников, `VERSION` = `CHANGELOG`) и сборка ESP-IDF
  в двух конфигурациях.
- Релиз идёт через PR `main` → `releases`: те же проверки выполняются как
  обязательные, а после мержа автоматически публикуется релиз по файлу `VERSION`.
- Публикация идемпотентна: если тег `vX.Y.Z` уже существует, шаг релиза
  пропускается. Артефакты: `esp32-zapret-merged.bin` и архив с исходниками,
  `INSTALL_RU.md` и прошивкой.
- Выпуск версии: поднять `VERSION`, добавить секцию в `CHANGELOG.md`, открыть PR
  в `releases`, дождаться зелёных проверок и смержить.

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
