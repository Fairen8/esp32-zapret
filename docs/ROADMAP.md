# Roadmap

The roadmap describes the intended direction of esp32-zapret. Priorities may
change based on field feedback (DPI behavior differs between providers/regions).

## Current state (v0.2.x)

- `esp_desync` component: `fake` / `split` / `disorder` / `tlsrec` modes,
  `TTL` / `MD5SIG` / `BADSUM` / `BADSEQ` fooling.
- Telegram bot with Wake-on-LAN, runtime tuning (`/desync`, `/ttl`, `/fool`).
- Telegram endpoint IP failover, `409 Conflict` diagnostics.
- CI: unit tests, static analysis, free of external CI dependencies, fuzzing
  (libFuzzer + ASan/UBSan), CodeQL, Scorecard, releases with SLSA provenance.
- OpenSSF Baseline-1 and Best Practices Passing badges.

## Near term

- [x] Release builds for ESP32-S3 and ESP32-C3.
- [x] No-bot firmware variant (periodic TLS self-test).
- [ ] Field testing on ESP32-S3/C3 hardware.
- [ ] `fake` with multiple decoy SNIs and `rndsni` (randomized SNI).
- [ ] `seqovl` overlap mode (split with sequence overlap).
- [ ] `ts` fooling (TCP timestamps), for providers where TTL tuning is fragile.
- [ ] Publish `esp_desync` to the ESP Component Registry.

## Medium term

- [ ] DoH / DNS anti-spoofing support.
- [ ] Settings in NVS + configuration via bot commands instead of `secrets.h`.
- [ ] Optional Web UI for provisioning.

## Long term / exploratory

- [ ] ESP32-S3/C3 verification (the LwIP part is portable; needs testing).
- [ ] Additional transports for blocked services (research).

## Non-goals

- Acting as a general-purpose proxy or VPN.
- Supporting IPv6 (out of scope for the current design).
- Full TCP-stack reconstruction of the device's traffic (the tool targets its
  own connection only).
