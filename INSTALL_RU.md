# esp32-zapret — установка

Мини-обход DPI/ТСПУ для Telegram-бота на ESP32 (логика zapret, встроенная в само
устройство). Бот принимает команды из Telegram и шлёт Wake-on-LAN magic packet на ПК.
Сторонние серверы/VPN не нужны.

## Что в архиве

```
main/           исходники бота (ESP-IDF)
components/     esp_desync — ядро обхода DPI
tools/          win-build.ps1 — сборка на Windows из пути с кириллицей
firmware/       готовая сборка для ESP32 (см. вариант 2)
README.md       описание архитектуры, режимов и тюнинга
README.en.md    то же на английском
CHANGELOG.md    история версий
DISCLAIMER.md   юридическая информация
```

## Вариант 1. Собрать самому (так заработает по-настоящему)

Нужен ESP-IDF v5.3+ (Windows/Linux/macOS).

1. Заполни конфиг:

   ```
   copy main\secrets_example.h main\secrets.h
   ```

   В `main/secrets.h` укажи:
   - `CFG_WIFI_SSID` / `CFG_WIFI_PASS` — твоя Wi-Fi сеть (2.4 ГГц)
   - `CFG_TG_TOKEN` — токен бота от @BotFather
   - `CFG_TG_ADMIN_ID` — твой числовой chat_id (узнать: написать @userinfobot), 0 = без ограничений
   - `CFG_WOL_MAC` — MAC ПК, который надо будить
   - `CFG_WOL_BROADCAST` — обычно `255.255.255.255`
   - `CFG_TG_API_IPS` — опционально: список IP Telegram через запятую (см. ниже)

2. Сборка и прошивка:

   ```
   idf.py set-target esp32
   idf.py build flash monitor
   ```

   Если путь к папке содержит русские буквы (IDF это не любит), на Windows
   используй `tools\win-build.ps1` — он собирает проект в ASCII-каталоге:

   ```
   powershell -ExecutionPolicy Bypass -File tools\win-build.ps1 -Action build
   powershell -ExecutionPolicy Bypass -File tools\win-build.ps1 -Action flash-monitor -Port COM5
   ```

   (по умолчанию хелпер ожидает раскладку `D:\esp32-zapret\{esp-idf,python,tools}`;
   пути переопределяются параметрами `-IdfPath`, `-ToolsPath`, `-PythonDir`).

## Вариант 2. Прошить готовое (без тулчейна)

В `firmware/` лежит собранная прошивка, но **с пустыми данными** (SSID/токен-заглушки) —
для реальной работы её нужно пересобрать по варианту 1. Готовую можно залить, чтобы
проверить, что плата жива, и посмотреть лог по serial 115200.

- `firmware/flash.bat COM5` — прошивка (нужен `esptool`: `pip install esptool`)
- `firmware/esp32-zapret-merged.bin` — единый образ для любого флешера
  (Espressif Flash Download Tool и т.п.), адрес `0x0`

## Команды бота

```
/wake [AA:BB:CC:DD:EE:FF]   послать magic packet (без аргумента — MAC из secrets.h)
/status                     состояние (uptime, heap, RSSI, режим, IP и HTTP-статус Telegram)
/desync <mode>              off | split | disorder | fake | fake_split | tlsrec
/ttl <1..255>               TTL фейкового пакета — главная ручка тюнинга
/fool <mode>                ttl | md5sig | badsum | badseq | none
```

Тюнинг TTL: начинай с 3. Если Telegram по-прежнему недоступен — увеличивай (фейк не
долетает до DPI); если рукопожатие виснет/рвётся — уменьшай (фейк доходит до сервера).
Если TTL не помогает — перебери режимы `/desync`, fooling через `/fool` (например
`md5sig`), затем ручной выбор IP (ниже).

## Если Telegram недоступен (важно)

1. **Один токен — один потребитель.** Если этот же бот опрашивается где-то ещё
   (скрипт, второй экземпляр, Desktop-клиент с Bot API), Telegram отдаёт
   `409 Conflict`, и сообщения «разбирает» то один, то другой клиент. В логе ESP будет
   `HTTP 409 Conflict`. Решение: сделай отдельного бота у @BotFather только для ESP.
2. **Блокировка по IP.** Иногда DNS отдаёт мёртвый адрес `api.telegram.org`
   (SYN уходит в таймаут). Прошивка сама перебирает список известных живых IP, а потом
   DNS. Посмотреть, куда подключилась: `/status` → `tg <ip>`. Свой список можно задать
   в `main/secrets.h`:

   ```c
   #define CFG_TG_API_IPS "149.154.167.220,149.154.167.191"
   ```

   Проверить адреса: `nslookup api.telegram.org` (и `ping` из своей сети).

## Дисклеймер

Используй только на своих устройствах и в своей сети. Лицензия MIT.
