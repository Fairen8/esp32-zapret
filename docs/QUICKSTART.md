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

## 3. Build and flash

```bash
idf.py set-target esp32
idf.py build flash monitor
```

Windows with a non-ASCII project path: use `tools\win-build.ps1` (mirrors the
project to an ASCII path and builds there).

## 4. Use

Message the bot:

```
/wake [AA:BB:CC:DD:EE:FF]   send a magic packet
/status                     device state (uptime, heap, RSSI, mode, Telegram IP)
/desync <mode>              off | split | disorder | fake | fake_split | tlsrec
/ttl <1..255>               fake packet TTL (start with 3, tune up/down)
/fool <mode>                ttl | md5sig | badsum | badseq | none
```

## 5. Prebuilt firmware

Release archives contain a merged image (`esp32-zapret-merged.bin`, flash at
`0x0`) and individual binaries. Prebuilt images use **placeholder
credentials** — build from source with your own `secrets.h` for real use.

## Troubleshooting

- Telegram unreachable → try `/ttl 4..8`, then `/desync` modes, then `/fool`
  variants; check `/status` for the endpoint in use.
- `HTTP 409` in logs → another client is polling the same bot token; create a
  separate bot for the ESP32.
- Build issues with non-ASCII paths → see `tools/win-build.ps1`.
