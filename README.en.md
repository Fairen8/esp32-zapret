# esp32-zapret

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![tests](https://github.com/Fairen8/esp32-zapret/actions/workflows/tests.yml/badge.svg)](https://github.com/Fairen8/esp32-zapret/actions/workflows/tests.yml)
[![release](https://img.shields.io/github/v/release/Fairen8/esp32-zapret?include_prereleases&label=release)](https://github.com/Fairen8/esp32-zapret/releases)
[![OpenSSF Baseline](https://www.bestpractices.dev/projects/15251/baseline)](https://www.bestpractices.dev/projects/15251)
[![OpenSSF Best Practices](https://www.bestpractices.dev/projects/15251/badge)](https://www.bestpractices.dev/projects/15251)
[![CodeQL](https://github.com/Fairen8/esp32-zapret/actions/workflows/codeql.yml/badge.svg)](https://github.com/Fairen8/esp32-zapret/actions/workflows/codeql.yml)
[![OpenSSF Scorecard](https://api.securityscorecards.dev/projects/github.com/Fairen8/esp32-zapret/badge)](https://securityscorecards.dev/viewer/?uri=github.com/Fairen8/esp32-zapret)

A tiny [zapret](https://github.com/bol-van/zapret)-style anti-DPI tool for the
ESP32: TLS ClientHello desynchronization performed **by the device itself**, with
no external servers or VPN. It started from a practical need: a Telegram bot on
an ESP32 must send Wake-on-LAN magic packets, but Telegram is blocked/throttled
by ISP-level DPI (TSPU).

`esp_desync` intercepts the outgoing TLS ClientHello and desynchronizes the DPI
using the same tricks as zapret: `fake`/`split`/`disorder`/`tlsrec` plus fooling
(`TTL`, `MD5SIG`, `BADSUM`, `BADSEQ`). The bundled bot supports `/wake`,
`/status` and runtime tuning over Telegram.

> This is an independent implementation, **not a code fork** of zapret and not
> affiliated with its author. See [DISCLAIMER.md](DISCLAIMER.md).

```
ESP32 (WROOM-32) ── Wi-Fi ── router ── ISP/TSPU (DPI) ── api.telegram.org
       │                       │
       │  ClientHello:         │  fake with TTL=3 is seen by the DPI but dies
       │  [fake(iana.org)]────▶│  on the way and never reaches the server
       │  [real hello]────────▶│  the DPI has already allowed the flow
```

## Features

- SNI-blocking bypass on the ESP32 itself (LwIP + mbedTLS), no proxy/VPN needed.
- 5 desync modes + 4 fooling methods, switchable at runtime over Telegram.
- Telegram endpoint IP pin/failover list (fixes dead DNS answers and IP blocks).
- Wake-on-LAN on a Telegram command.
- Minimal Bot API HTTPS client built on raw mbedTLS with a custom BIO.
- Open source, MIT.

## Compatibility

| | |
|---|---|
| Chip | ESP32 / ESP32-S3 / ESP32-C3 (CI builds all three; field-tested on ESP32-WROOM-32) |
| ESP-IDF | ≥ 5.1 (CI builds with v5.3.6) |
| Flash | 4 MB (`SINGLE_APP_LARGE`) |
| Wi-Fi | 2.4 GHz, WPA2 |
| Network | IPv4 only, TLS via mbedTLS |

The `esp_desync` component ships with an `idf_component.yml` manifest, so it can
be consumed as a regular ESP-IDF component or published to the
[ESP Component Registry](https://components.espressif.com).

## How it works

On Linux, zapret is usually applied as a transparent proxy or via nfqueue. On
the ESP32 we **are the endpoint**, which makes everything simpler:

1. A normal TCP connection to `api.telegram.org:443` via LwIP.
2. Before sending the ClientHello, the component reads the socket's
   `snd_nxt`/`rcv_nxt` from `tcp_active_pcbs` (inside the tcpip task) — exact
   seq/ack numbers, no packet sniffing required.
3. It builds a decoy TLS ClientHello (SNI `www.iana.org` by default) and injects
   it **outside the TCP stream** via `ip4_output_if()`. The packet goes through
   the normal Wi-Fi TX path (WPA2 hardware encryption, NAT) with the original
   sequence number, exactly like zapret's `--dpi-desync=fake`.
4. Fooling keeps the fake from reaching the server: TTL=3 by default (it dies on
   the way, but the DPI sees it). The DPI marks the flow as allowed and the
   server only receives the real ClientHello.
5. The TLS session then runs via mbedTLS with a custom BIO on top of
   `esp_desync_write()`.

### Why not raw 802.11 frames

The original approach (`esp_wifi_80211_tx`) has two unknowns: the driver is
officially limited in frame types, and data-frame encryption under WPA2 is
undocumented — the AP may simply drop an unencrypted fake. Injection via
`ip4_output_if()` avoids both: it is the same path LwIP uses for regular TCP,
with full control over seq/ack/TTL/checksum.

## Modes

| Mode | zapret analogue | What it does |
|---|---|---|
| `off` | — | no modification |
| `split` | `multisplit` | splits the ClientHello into TCP segments (at the start and middle of the SNI by default) with a delay |
| `disorder` | `multidisorder` | the tail of the ClientHello is sent raw **before** the head (it is then re-sent through the socket to keep LwIP sequencing consistent) |
| `fake` | `fake` | injects a decoy ClientHello with the original seq |
| `fake_split` | `fake,split2` | **default**: fake + split of the real hello |
| `tlsrec` | `tlsrec` (tpws) | rewrites the ClientHello into two TLS records so the DPI cannot reassemble the SNI |
| `seqovl` | `seqovl` | fake with the sequence shifted back, overlapping the stream head (`CONFIG_ESP_DESYNC_SEQOVL_LEN`, 32 bytes by default) |

Desync is applied only to hosts in the allowlist (default `api.telegram.org`,
configurable via `CONFIG_ESP_DESYNC_HOSTS`): statistics, DoH and the web UI go
out as plain TLS, so an active bypass strategy cannot break them.

Fake fooling methods: `TTL` (default), `MD5SIG` (Linux servers silently drop a
packet with the TCP MD5 option), `BADSUM` (does not pass home NATs with conntrack
checksum validation), `BADSEQ` (pushes seq out of the window), `DATANOACK` (no
ACK flag), `TS` (TCP timestamps with a random value, combinable with TTL).
Defaults live in Kconfig; runtime switch via `/fool`. `/rndsni on` gives every
fake a random decoy SNI, so the packet size stops being a fingerprint too.

## Auto-tuning and health monitoring

Since v1.0.0 the device **detects the optimal operating parameters itself**:

- at boot it walks strategies from simple to complex (`off` → `fake_split` with
  TTL 3/5/8/12 → alternative decoy SNIs → `md5sig`/`badseq`/`datanoack` →
  `split`/`disorder`/`tlsrec`), probing each with a real TLS connection to
  api.telegram.org; the first working one is stored in NVS and probed first on
  subsequent boots;
- every 10 minutes it verifies that the bypass still works; after 2 consecutive
  failures it re-runs the scan (providers change their filtering);
- if DNS is poisoned, Telegram addresses are resolved over **DoH**
  (Cloudflare/Google).

Manual control: `/scan` re-runs the scan, `/strategy` shows the current strategy
and probe statistics. Manual `/desync`, `/ttl`, `/fool` are persisted in NVS
(survive reboot) and take precedence over auto-detection: health checks never
reset them; they are cleared by `/scan` or after two failed checks in a row.
`/status` shows whether the current settings are `manual` or `auto`.

## Anonymous statistics

Since v1.0.1 the firmware can send a small anonymous report to
`statistics.fairen8.ru` so the author can see which strategies actually work at
different providers. Since v1.1.1 sending is **enabled by default**.

What is sent: firmware version, chip, uptime, RSSI, mode/fooling/TTL, split
positions, the selected strategy and probe counters, plus (since v1.2.0)
diagnostics: strategies tried before success and time-to-strategy, heap
metrics, Wi-Fi disconnects, temperature, top error codes, DoH/SNTP usage,
failure stage, config and hardware. There are no stable identifiers: no device
or chat IDs, SSIDs, IP addresses. The only pseudonym is `rid` = HMAC(local
secret, current day); it rotates daily and cannot link a device across days.

Disable with `/stats off` (persisted in NVS) or the web UI toggle; re-enable
with `/stats on`. Build with `CONFIG_APP_STATS_DEFAULT_ON=n` to opt out by
default. A report is sent at boot and at most once a day; if the server is
unavailable the attempt is silently skipped.

Report format and server requirements: [docs/STATS_BACKEND_SPEC.md](docs/STATS_BACKEND_SPEC.md) (RU).

## Quick start

```bash
git clone https://github.com/Fairen8/esp32-zapret.git
cd esp32-zapret
cp main/secrets_example.h main/secrets.h   # Wi-Fi, bot token, PC MAC
idf.py set-target esp32
idf.py menuconfig                          # Component config -> esp_desync anti-DPI
idf.py build flash monitor
```

> **Important:** `CFG_TG_ADMIN_ID=0` accepts commands from any chat — anyone
> who finds the bot can wake the PC and change the bypass settings. Set your
> numeric chat id (see @userinfobot); it is effectively the device password.

Prebuilt images are available in
[Releases](https://github.com/Fairen8/esp32-zapret/releases): merged images for
**esp32**, **esp32s3**, **esp32c3** and a **no-bot** variant (`esp32-nobot`,
periodic TLS self-test through esp_desync), plus a source archive.

Since v1.1.0 **no toolchain is needed**: flash the image, and on first boot the
device starts a setup access point `esp32-zapret-XXXX` (password
`zapret12345`). Connect to it, open **http://192.168.4.1** and set Wi-Fi, the
bot token, the PC MAC and a new web password. The same settings page then works
on the device IP (login `admin`); see [INSTALL_RU.md](INSTALL_RU.md).

### Windows: non-ASCII project paths

ESP-IDF's kconfgen fails when the project path contains non-ASCII characters.
If your path does, use the helper script; it mirrors the project into an ASCII
directory and builds there:

```
powershell -ExecutionPolicy Bypass -File tools\win-build.ps1 -Action build
powershell -ExecutionPolicy Bypass -File tools\win-build.ps1 -Action flash-monitor -Port COM5
```

By default the helper expects the layout `D:\esp32-zapret\{esp-idf,python,tools,work,build}`;
all paths can be overridden with `-IdfPath`, `-ToolsPath`, `-PythonDir`,
`-WorkDir`, `-BuildDir`.

### ESP32-C3 / ESP32-S3 console

C3/S3 builds use the native **USB Serial/JTAG** console by default
(`sdkconfig.defaults.esp32c3` / `sdkconfig.defaults.esp32s3`): connect the cable
to the chip's own USB port — `idf.py monitor` will show the log. On boards with
an external USB-UART bridge (CP210x/CH340) select
`CONFIG_ESP_CONSOLE_UART_DEFAULT=y` in `menuconfig`.

## Bot commands

```
/wake [AA:BB:CC:DD:EE:FF]   send a magic packet (no argument — MAC from settings)
/status                     uptime, heap, RSSI, mode/TTL/fooling/rndsni, IP and HTTP status
/desync <mode>              off | split | disorder | fake | fake_split | tlsrec | seqovl
/ttl <1..255>               fake packet TTL
/fool <mode>                ttl | md5sig | badsum | badseq | datanoack | ts | none
/rndsni on|off              random decoy SNI (and size) for every fake
/scan                       re-run strategy auto-detection (drops manual tuning)
/strategy                   current strategy and probe statistics
/stats on|off               anonymous statistics (on by default)
/help                       help (also /start)
/heap                       free/minimum heap and largest block
/ip                         IP, gateway, SSID, RSSI
/reboot                     restart the device
```

`/start` opens an inline menu: status, scan, strategy, network, memory,
Wake-on-LAN, settings (mode/TTL/fooling/rndsni/statistics) and a confirmed
reboot — everything via buttons, screens update in place.

### Tuning TTL

TTL is the main knob. The fake must reach the DPI but must not reach the server:

1. Start with `fake_split` and `/ttl 3`.
2. If the connection is still cut (DPI not fooled) — **increase** TTL until the
   bypass works: the fake was not reaching the DPI.
3. If the handshake breaks/hangs (the fake reached the server and corrupted the
   handshake) — **decrease** TTL.
4. The minimal working TTL ≈ the hop number of your DPI (methodology from the
   zapret documentation).

If TTL does not help, try `/fool md5sig`, then `ts`, `fake` without split, then
`disorder`, `tlsrec`, `seqovl`, then `/rndsni on`. The whole set can be
re-tried over Telegram in a couple of minutes.

## Limitations

- IPv4 and TLS only (HTTP/QUIC are not handled).
- One consumer per bot token: a second concurrent Bot API client makes Telegram
  answer `409 Conflict` within 4-7 seconds. The firmware logs it and backs off —
  create a separate bot for the ESP32.
- DNS may return dead Telegram addresses (IP-level blocking). The firmware tries
  a built-in list of known addresses (`149.154.167.220`, `.167.191`, `.175.50`,
  ...) with a short timeout and only then falls back to DNS; the list can be
  overridden via `CFG_TG_API_IPS` in `main/secrets.h`. The current endpoint is
  shown in `/status`.
- `BADSUM` is useless behind a home router (conntrack drops invalid packets) —
  use TTL/MD5SIG.
- Some stock router firmwares pin the outgoing TTL; TTL fooling will not work
  through them (see zapret docs).
- IP-level blocking cannot be solved by DPI tricks — use a proxy/VPS or a live
  address from the list.
- Parallel connections are supported (armed-flow table of 4 sockets); the bot
  still works sequentially.
- The target is a DPI that interprets the stream in a limited way; a full TCP
  stack (transparent proxy/Squid) cannot be fooled.

## Roadmap

- [x] Runtime mode/TTL/fooling switching over Telegram
- [x] Telegram endpoint pin/failover by IP
- [x] `fake` with multiple SNIs and `rndsni`
- [x] seqovl overlap
- [x] `ts` fooling (TCP timestamps, like ALT1 in zapret)
- [x] DoH / DNS anti-spoofing
- [x] NVS settings + Web UI (v1.1.0)
- [x] ESP32-S3/C3 (releases and field testing; USB Serial/JTAG console)
- [ ] Publish `esp_desync` to the ESP Component Registry (workflow ready, needs a token)

## CI/CD and releases

- Every push to `main` runs a set of tasks: host unit tests, static analysis,
  repository hygiene (no secrets/binaries tracked, `VERSION` matches `CHANGELOG`)
  and ESP-IDF builds in two configurations.
- Releases go through a `main` → `releases` PR: the same checks are required,
  and after the merge a release is published automatically based on `VERSION`.
- Publishing is idempotent: if tag `vX.Y.Z` already exists, the release step is
  skipped. Assets: `esp32-zapret-merged.bin` and an archive with sources,
  `INSTALL_RU.md` and firmware.
- To cut a version: bump `VERSION`, add a `CHANGELOG.md` section, open a PR to
  `releases`, wait for green checks and merge.

## Documentation

- [docs/QUICKSTART.md](docs/QUICKSTART.md) — quick start
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — architecture and data flow
- [docs/SECURITY_DESIGN.md](docs/SECURITY_DESIGN.md) — threat model and security design
- [docs/ASSURANCE_CASE.md](docs/ASSURANCE_CASE.md) — assurance case
- [docs/GOVERNANCE.md](docs/GOVERNANCE.md) — governance, roles, continuity
- [docs/CODING_STANDARDS.md](docs/CODING_STANDARDS.md) — coding standards
- [docs/MAINTENANCE.md](docs/MAINTENANCE.md) — releases and maintenance
- [docs/ROADMAP.md](docs/ROADMAP.md) — roadmap

## Achievements

- OpenSSF Best Practices: **baseline-1** and **passing** badges (project 15251).
- CodeQL (0 alerts), TLS parser fuzzing (libFuzzer + ASan/UBSan), static
  analysis, secret scanning with push protection.
- Releases are built in CI and published with signed SLSA provenance.
- OpenSSF Scorecard evaluates the repository automatically.

## Security and legal

The project is intended for personal use on your own devices and networks, and
for educational/research purposes. You are solely responsible for complying with
the laws of your jurisdiction, your ISP's terms and the terms of service of any
network service. See [DISCLAIMER.md](DISCLAIMER.md) for details.

## Credits

- [zapret](https://github.com/bol-van/zapret) (MIT, bol-van) — the conceptual
  foundation, mode terminology and TTL tuning methodology.
- [tpws](https://github.com/bol-van/zapret) from the zapret project — the
  `tlsrec` idea.
- LwIP, mbedTLS, ESP-IDF.

## License

MIT — see [LICENSE](LICENSE). © 2026 esp32-zapret contributors.
