# Quick start

This is the English quick-start guide. A Russian step-by-step guide is
available in [INSTALL_RU.md](../INSTALL_RU.md).

## 1. Requirements

- ESP32 board (tested on ESP32-WROOM-32, 4 MB flash)
- ESP-IDF v5.1+ (CI uses v5.3.6)
- A Telegram bot token from @BotFather
- The MAC address of the PC to wake

## 2. Configure

```bash
git clone https://github.com/Fairen8/esp32-zapret.git
cd esp32-zapret
cp main/secrets_example.h main/secrets.h
```

Edit `main/secrets.h`:

- `CFG_WIFI_SSID` / `CFG_WIFI_PASS` — 2.4 GHz Wi-Fi network
- `CFG_TG_TOKEN` — bot token
- `CFG_TG_ADMIN_ID` — your numeric chat id (0 = accept anyone)
- `CFG_WOL_MAC` — target PC MAC (`AA:BB:CC:DD:EE:FF`)
- `CFG_TG_API_IPS` — optional Telegram endpoint IP list (comma-separated)

> Keep `CFG_TG_ADMIN_ID` at 0 only for a quick bench test: anyone who finds
> the bot can wake the PC and change the bypass settings. Use your numeric
> chat id (see @userinfobot) — it is effectively the device password.

## 3. Build and flash

```bash
idf.py set-target esp32
idf.py build flash monitor
```

Windows with a non-ASCII project path: use `tools\win-build.ps1` (mirrors the
project to an ASCII path and builds there).

## 4. Use

By default the device auto-detects the optimal bypass at boot and re-checks it
every 10 minutes. `/scan` re-runs the detection, `/strategy` shows the selected
strategy and probe statistics. Manual `/desync`, `/ttl` and `/fool` take
precedence over auto-detection, are persisted in NVS (survive reboot) and are
cleared by `/scan` or after two failed health checks in a row; `/status` shows
whether the current settings are `manual` or `auto`.

Message the bot:

```
/wake [AA:BB:CC:DD:EE:FF]   send a magic packet
/status                     device state (uptime, heap, RSSI, mode, Telegram IP)
/desync <mode>              off | split | disorder | fake | fake_split | tlsrec | seqovl
/ttl <1..255>               fake packet TTL (start with 3, tune up/down)
/fool <mode>                ttl | md5sig | badsum | badseq | datanoack | ts | none
/rndsni on|off              random decoy SNI (and fake size) per packet
/scan                       re-run auto-detection (drops manual tuning)
/strategy                   current strategy and probe statistics
/heap, /ip, /reboot         diagnostics and restart
```

## Build variants

CI and releases build four firmware variants:

| Variant | Target | Notes |
|---|---|---|
| `default` | esp32 | Telegram bot |
| `esp32s3` | esp32s3 | Telegram bot |
| `esp32c3` | esp32c3 | Telegram bot |
| `nobot` | esp32 | no Telegram bot: periodic TLS self-test through esp_desync |

Build for another chip:

```bash
idf.py set-target esp32s3   # or esp32c3
idf.py build
```

ESP32-C3/S3 builds use the native USB Serial/JTAG console by default
(`sdkconfig.defaults.esp32c3` / `.esp32s3`): connect the cable to the chip's
own USB port. On boards with an external USB-UART bridge, select
`CONFIG_ESP_CONSOLE_UART_DEFAULT=y` in `menuconfig`.

Disable the bot (self-test firmware):

```bash
idf.py menuconfig   # esp32-zapret application -> Telegram bot (disable)
```

## 5. Prebuilt firmware

Release archives contain per-target directories (`esp32/`, `esp32s3/`,
`esp32c3/`, `esp32-nobot/`) with individual binaries, a `flash.bat` and a merged
image (`esp32-zapret-merged.bin`, flash at `0x0`).

> **Note:** the merged image (and `flash.bat`) includes a blank NVS partition,
> so flashing it over a configured device **wipes Wi-Fi, bot token and WoL
> settings**. To keep the configuration, update only the application binary
> (`esp32-zapret.bin` at `0x10000`).

Since v1.1.0 no toolchain is needed: on first boot the device starts a setup
access point `esp32-zapret-XXXX` (password `zapret12345`). Connect to it, open
`http://192.168.4.1` and set Wi-Fi, the bot token, the PC MAC and a new web
password. The same settings page then works on the device IP (login `admin`);
the serial console (`setwifi`, `settoken`, `setmac`, ...) is an alternative.

## Troubleshooting

- Telegram unreachable → try `/ttl 4..8`, then `/desync` modes, then `/fool`
  variants; check `/status` for the endpoint in use.
- `HTTP 409` in logs → another client is polling the same bot token; create a
  separate bot for the ESP32.
- Build issues with non-ASCII paths → see `tools/win-build.ps1`.
