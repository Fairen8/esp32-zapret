# Roadmap

The roadmap describes the intended direction of esp32-zapret. Priorities may
change based on field feedback (DPI behavior differs between providers/regions).

## Current state (v1.1.x)

- `esp_desync` component: `fake` / `split` / `disorder` / `tlsrec` / `seqovl`
  modes, `TTL` / `MD5SIG` / `BADSUM` / `BADSEQ` / `DATANOACK` / `TS` fooling,
  `rndsni` (random decoy SNI) and per-connection state.
- Runtime configuration in NVS (Wi-Fi, bot token, WoL, web password) with a
  setup AP + web UI + serial console; prebuilt images need no toolchain.
- Telegram bot with Wake-on-LAN, runtime tuning, `/heap`, `/ip`, `/reboot`.
- Telegram endpoint IP failover, `409 Conflict` diagnostics, BSSID pinning,
  static IP fallback, DoH resolver.
- CI: unit tests, static analysis, fuzzing, CodeQL, Scorecard, SLSA releases.

## Near term

- [x] Release builds for ESP32-S3 and ESP32-C3.
- [x] No-bot firmware variant (periodic TLS self-test).
- [x] Boot-time strategy auto-detection and periodic health monitoring.
- [x] DoH (DNS-over-HTTPS) fallback resolver.
- [x] Field testing on ESP32-S3/C3 hardware.
- [x] `fake` with multiple decoy SNIs and `rndsni` (randomized SNI).
- [x] `seqovl` overlap mode (split with sequence overlap).
- [x] `ts` fooling (TCP timestamps), for providers where TTL tuning is fragile.
- [ ] Publish `esp_desync` to the ESP Component Registry.

## Medium term

- [x] Settings in NVS + configuration via bot commands instead of `secrets.h`.
- [x] Optional Web UI for provisioning (setup AP + settings page).

## Long term / exploratory

- [ ] ESP32-S3/C3 verification (the LwIP part is portable; needs testing).
- [ ] Additional transports for blocked services (research).

## Non-goals

- Acting as a general-purpose proxy or VPN.
- Supporting IPv6 (out of scope for the current design).
- Full TCP-stack reconstruction of the device's traffic (the tool targets its
  own connection only).
