# esp32-zapret — установка

Мини-обход DPI/ТСПУ для Telegram-бота на ESP32 (логика zapret, встроенная в само
устройство). Бот принимает команды из Telegram и шлёт Wake-on-LAN magic packet на ПК.
Сторонние серверы/VPN не нужны.

Начиная с v1.1.0 прошивка настраивается **без пересборки**: готовый образ при
первом включении поднимает точку доступа с веб-страницей. ESP-IDF нужен только
для сборки из исходников.

## Что в архиве

```
main/           исходники бота (ESP-IDF)
components/     esp_desync — ядро обхода DPI
tools/          win-build.ps1 — сборка на Windows из пути с кириллицей
firmware/       готовые сборки (см. вариант 1)
README.md       описание архитектуры, режимов и тюнинга
README.en.md    то же на английском
CHANGELOG.md    история версий
DISCLAIMER.md   юридическая информация
```

## Вариант 1. Прошить готовое и настроить с телефона (без тулчейна)

1. Скачайте архив релиза и выберите папку под свой чип:
   - `firmware/esp32/` — ESP32 (WROOM-32);
   - `firmware/esp32s3/` — ESP32-S3;
   - `firmware/esp32c3/` — ESP32-C3;
   - `firmware/esp32-nobot/` — ESP32 без Telegram-бота (периодический TLS
     self-test, удобно проверить обход сети без токена).

2. Прошейте образ:
   - `firmware/<чип>/flash.bat COM5` (нужен `esptool`: `pip install esptool`), либо
   - `esp32-zapret-merged.bin` любым флешером (Espressif Flash Download Tool,
     ESP Web Tools и т.п.) по адресу `0x0`.

3. При первом включении плата поднимет точку доступа:
   - **SSID:** `esp32-zapret-XXXX` (XXXX — из MAC),
   - **пароль:** `zapret12345`.

4. Подключитесь к этой сети и откройте **http://192.168.4.1**. Заполните:
   - Wi-Fi вашей сети (из списка или вручную);
   - токен бота от @BotFather (можно оставить пустым для no-bot сборки);
   - admin chat id (узнать: @userinfobot) — **не оставляйте 0**: с 0 команды
     принимает любой, кто найдёт бота;
   - MAC ПК для Wake-on-LAN;
   - новый пароль веб-интерфейса (мин. 8 символов).

   «Сохранить и перезагрузить» — устройство подключится к Wi-Fi и начнёт работу.

5. Дальше настройки доступны без перепрошивки:
   - в Telegram: `/status`, `/desync`, `/ttl`, `/fool`, `/rndsni`, `/scan`,
     `/strategy`, `/heap`, `/ip`, `/reboot`;
   - в браузере по IP устройства (логин `admin`, ваш пароль): статус, Wi-Fi,
     токен, WoL, режим обхода, автоподбор, статистика, сброс настроек. IP виден
     в `/status` или командой `/ip`.

Альтернатива шагам 3-4: настройка через USB-консоль (терминал 115200 или
`idf.py monitor`, для C3/S3 — USB Serial/JTAG). В setup-режиме доступны команды
`setwifi <ssid> <pass>`, `settoken <token>`, `setadmin <id>`, `setmac <mac>`,
`setwebpass <pass>`, `status`, `reboot`, `erase`.

## Вариант 2. Собрать самому (для разработки)

Нужен ESP-IDF v5.1+ (CI собирает на v5.3.6; Windows/Linux/macOS).

1. Заполни конфиг (значения из `secrets.h` остаются значениями по умолчанию,
   их можно потом переопределить через веб-интерфейс/консоль):

   ```
   copy main\secrets_example.h main\secrets.h
   ```

   В `main/secrets.h` укажи:
   - `CFG_WIFI_SSID` / `CFG_WIFI_PASS` — твоя Wi-Fi сеть (2.4 ГГц)
   - `CFG_TG_TOKEN` — токен бота от @BotFather
   - `CFG_TG_ADMIN_ID` — твой числовой chat_id, **не оставляй 0**
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

## Команды бота

```
/wake [AA:BB:CC:DD:EE:FF]   послать magic packet (без аргумента — MAC из настроек)
/status                     состояние (uptime, heap, RSSI, режим, rndsni, IP, HTTP-статус Telegram)
/desync <mode>              off | split | disorder | fake | fake_split | tlsrec | seqovl
/ttl <1..255>               TTL фейкового пакета — главная ручка тюнинга
/fool <mode>                ttl | md5sig | badsum | badseq | datanoack | ts | none
/rndsni on|off              случайный decoy-SNI (и размер) для каждого фейка
/scan                       перезапустить автоподбор (снимает ручную настройку)
/strategy                   текущая стратегия и статистика проб
/heap                       свободная/минимальная heap и крупнейший блок
/ip                         IP, шлюз, SSID, RSSI
/reboot                     перезагрузка
```

Тюнинг TTL: начинай с 3. Если Telegram по-прежнему недоступен — увеличивай (фейк не
долетает до DPI); если рукопожатие виснет/рвётся — уменьшай (фейк доходит до сервера).
Если TTL не помогает — перебери режимы `/desync`, fooling через `/fool` (например
`md5sig` или `ts`), затем включи `/rndsni on` и повтори, затем ручной выбор IP (ниже).

Ручные `/desync`, `/ttl`, `/fool`, `/rndsni` сохраняются в NVS (переживают
перезагрузку) и имеют приоритет над автоподбором; сбрасываются `/scan` или после
двух неудачных health-проверок подряд.

## Сброс настроек

- Веб-интерфейс → «Сброс настроек» (устройство перезагрузится в setup-режим);
- или serial-консоль: команда `erase`;
- или полное стирание: `esptool erase_flash` (снесёт и прошивку).

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
3. **NTP заблокирован.** Прошивка берёт время с нескольких NTP-серверов, а после
   таймаута — из даты сборки, поэтому загрузка не зависает.

## Дисклеймер

Используй только на своих устройствах и в своей сети. Лицензия MIT.
