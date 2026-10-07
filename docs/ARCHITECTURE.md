# Architecture

## Overview

esp32-zapret is an ESP-IDF application that runs on an ESP32 (WROOM-32). It
maintains a Telegram bot connection over HTTPS and, on demand, sends Wake-on-LAN
magic packets to a PC on the local network. To reach Telegram through DPI-based
blocking, the outgoing TLS ClientHello is desynchronized by the `esp_desync`
component.

```
┌────────────────────────── ESP32 ──────────────────────────┐
│  main/                                                    │
│    main.c          Wi-Fi STA, SNTP, bot loop, commands    │
│    desync_scan.c   strategy auto-scan + health monitoring │
│    scan_candidates.c  candidate list (pure, unit-tested)  │
│    https_client.c  HTTPS/TLS client (mbedTLS + esp_desync)│
│    telegram.c      Telegram Bot API over https_client     │
│    doh.c           DNS-over-HTTPS fallback resolver       │
│    stats.c         anonymous statistics (opt-in, silent)  │
│    stats_payload.c pure JSON builder (unit-tested)        │
│    net_utils.c     IP/MAC/URL helpers                     │
│    wol.c           Wake-on-LAN magic packets              │
│                                                           │
│  components/esp_desync/                                   │
│    esp_desync.c   orchestration, connect/write/read       │
│    desync_tls.c   ClientHello parsing, decoy generation   │
│    desync_pcb.c   socket seq/ack introspection (LwIP)     │
│    desync_inject.c raw TCP segment injection (ip4_output_if)│
└───────────────────────────────────────────────────────────┘
```

## Boot and health flow

1. Wi-Fi connects; SNTP synchronizes the clock (needed for TLS certificate
   dates).
2. `desync_scan.c` loads the last working strategy from NVS and probes it.
3. If it fails (or nothing is saved), the candidate list from
   `scan_candidates.c` is probed in order (`off` → fake+split with ascending
   TTL → alternative decoy SNIs → other fooling → split/disorder/tlsrec). The
   first working strategy is stored in NVS. Probes are ordinary TLS requests
   to api.telegram.org through `https_client.c`.
4. The bot loop runs with the selected strategy. Every
   `APP_HEALTH_CHECK_INTERVAL_S` the strategy is probed again; after
   `APP_HEALTH_FAIL_THRESHOLD` consecutive failures the scan is re-run.
5. If DNS resolution fails, `doh.c` resolves the Telegram addresses over
   HTTPS (Cloudflare / Google).
6. If anonymous statistics are enabled (opt-in), a small report is POSTed to
   statistics.fairen8.ru; failures are ignored (`stats.c`).

## Data flow (Telegram request)

1. `telegram.c` resolves the Telegram endpoint (pinned IP list, then DNS) and
   opens a TCP connection through `esp_desync_connect_ip()`.
2. mbedTLS is configured with certificate verification (CA bundle) and a custom
   BIO whose write callback is `esp_desync_write()`.
3. On the first write (TLS ClientHello), `esp_desync.c`:
   - parses the ClientHello (`desync_tls.c`) to locate the SNI;
   - builds a decoy ClientHello with a benign SNI (`www.iana.org` by default);
   - reads the socket's `snd_nxt`/`rcv_nxt` from the TCP PCB
     (`desync_pcb.c`, via `tcp_active_pcbs` in the tcpip task);
   - injects the decoy packet outside the TCP stream
     (`desync_inject.c`, `ip4_output_if`) with the configured fooling
     (TTL / MD5SIG / BADSUM / BADSEQ) so the DPI sees it but the server
     does not;
   - sends the real ClientHello, optionally split into segments / TLS records.
4. The TLS session continues normally; bot commands are parsed from
   `getUpdates` responses and replies are sent via `sendMessage`.

## Key design decisions

- **Endpoint-based desync.** On Linux, zapret operates as a transparent proxy.
  On the ESP32 the device is the endpoint itself, so sequence numbers can be
  obtained from its own TCP PCB and packets can be injected through the normal
  network interface — no packet sniffing is required.
- **`ip4_output_if` instead of raw 802.11 frames.** Injected packets travel the
  normal Wi-Fi TX path (hardware encryption, NAT), unlike raw 802.11 injection
  which is limited in frame types and encryption behavior.
- **Dependency-free CI merge.** The merged firmware image is produced by a
  documented Python script (byte-identical to `esptool merge_bin`) so CI has no
  unpinned package dependencies.

## Interfaces

- **Telegram commands:** `/wake`, `/status`, `/desync`, `/ttl`, `/fool`,
  `/rndsni`, `/scan`, `/strategy`, `/stats`, `/heap`, `/ip`, `/reboot`.
- **Web UI / setup mode:** HTTP on port 80; in setup mode the device runs its
  own AP (`esp32-zapret-XXXX`) and serves an unauthenticated provisioning page,
  otherwise the settings page requires HTTP Basic auth (`admin` + web password).
  Settings are stored in NVS via `app_settings.c`.
- **Component API:** `components/esp_desync/include/esp_desync.h`
  (`esp_desync_init`, `esp_desync_connect[_ip]`, `esp_desync_resolve`,
  `esp_desync_write`, `esp_desync_read`, `esp_desync_close`).

## External dependencies

All dependencies are ESP-IDF components pinned by the ESP-IDF release used in
CI (see `components/esp_desync/idf_component.yml`): LwIP (networking), mbedTLS
(TLS), FreeRTOS. There are no third-party runtime libraries.
