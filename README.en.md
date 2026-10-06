# esp32-zapret

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![tests](https://github.com/Fairen8/esp32-zapret/actions/workflows/tests.yml/badge.svg)](https://github.com/Fairen8/esp32-zapret/actions/workflows/tests.yml)
[![release](https://img.shields.io/github/v/release/Fairen8/esp32-zapret?include_prereleases&label=release)](https://github.com/Fairen8/esp32-zapret/releases)
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
| Chip | ESP32 (tested on ESP32-WROOM-32; the LwIP part is portable to S3/C3) |
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
| `split` | `multisplit` | splits the ClientHello into TCP segments (inside the SNI by default) with a delay |
| `disorder` | `multidisorder` | the tail of the ClientHello is sent raw **before** the head (it is then re-sent through the socket to keep LwIP sequencing consistent) |
| `fake` | `fake` | injects a decoy ClientHello with the original seq |
| `fake_split` | `fake,split2` | **default**: fake + split of the real hello |
| `tlsrec` | `tlsrec` (tpws) | rewrites the ClientHello into two TLS records so the DPI cannot reassemble the SNI |

Fake fooling methods: `TTL` (default), `MD5SIG` (Linux servers silently drop a
packet with the TCP MD5 option), `BADSUM` (does not pass home NATs with conntrack
checksum validation), `BADSEQ` (pushes seq out of the window). Defaults live in
Kconfig; runtime switch via `/fool`.

## Quick start

```bash
git clone https://github.com/Fairen8/esp32-zapret.git
cd esp32-zapret
cp main/secrets_example.h main/secrets.h   # Wi-Fi, bot token, PC MAC
idf.py set-target esp32
idf.py menuconfig                          # Component config -> esp_desync anti-DPI
idf.py build flash monitor
```

Prebuilt images are available in
[Releases](https://github.com/Fairen8/esp32-zapret/releases):
`esp32-zapret-merged.bin` (flash at `0x0`) and an archive. Release binaries are
built **with placeholder credentials** (no Wi-Fi/token) — build from source with
your own `secrets.h` for real use.

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

## Bot commands

```
/wake [AA:BB:CC:DD:EE:FF]   send a magic packet (no argument — MAC from secrets.h)
/status                     uptime, heap, RSSI, mode/TTL/fooling, Telegram IP and HTTP status
/desync <mode>              off | split | disorder | fake | fake_split | tlsrec
/ttl <1..255>               fake packet TTL
/fool <mode>                ttl | md5sig | badsum | badseq | none
```

### Tuning TTL

TTL is the main knob. The fake must reach the DPI but must not reach the server:

1. Start with `fake_split` and `/ttl 3`.
2. If the connection is still cut (DPI not fooled) — **increase** TTL until the
   bypass works: the fake was not reaching the DPI.
3. If the handshake breaks/hangs (the fake reached the server and corrupted the
   handshake) — **decrease** TTL.
4. The minimal working TTL ≈ the hop number of your DPI (methodology from the
   zapret documentation).

If TTL does not help, try `/fool md5sig`, then `fake` without split, then
`disorder`, `tlsrec`.

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
- One active "armed" connection at a time (the bot works sequentially).
- The target is a DPI that interprets the stream in a limited way; a full TCP
  stack (transparent proxy/Squid) cannot be fooled.

## Roadmap

- [x] Runtime mode/TTL/fooling switching over Telegram
- [x] Telegram endpoint pin/failover by IP
- [ ] `fake` with multiple SNIs and `rndsni`
- [ ] seqovl overlap
- [ ] `ts` fooling (TCP timestamps, like ALT1 in zapret)
- [ ] DoH / DNS anti-spoofing
- [ ] NVS settings + Web UI
- [ ] ESP32-S3/C3 (the LwIP part works unchanged)

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
