# Security design

This document describes the security design of esp32-zapret: assets, threat
model, secure design principles and input handling.

## Assets

| Asset | Where it lives |
|---|---|
| Wi-Fi credentials, Telegram bot token, admin chat id, WoL target MAC | `main/secrets.h` (compile-time, git-ignored; never tracked) |
| TLS session keys / material | RAM only (mbedTLS), never persisted |
| Firmware images | GitHub Releases, built by CI with SLSA provenance |
| Repository contents and history | Public GitHub repository |

## Threat model

- **Primary adversary: network DPI.** The tool is designed to desynchronize a
  DPI that inspects TLS ClientHello messages. The adversary can observe and
  inject packets but does not control endpoints.
- **Supply chain.** Release artifacts must be traceable to the source. Mitigated
  by SHA-pinned GitHub Actions, protected branches, CodeQL, and SLSA build
  provenance attached to releases.
- **Credential leakage.** Secrets must never reach the repository. Mitigated by
  GitHub secret scanning with push protection, a CI hygiene job, and a
  git-ignored `secrets.h` (only `secrets_example.h` is tracked).
- **Input from untrusted sources.** The device parses TLS responses from the
  server and text from Telegram users. Both are treated as untrusted.

## Secure design principles

- **No custom cryptography.** All cryptography is delegated to mbedTLS
  (`crypto_call`, `crypto_published`); only modern primitives are used
  (ECDHE, AES-GCM, SHA-256).
- **Certificate verification is mandatory.** The client uses the ESP-IDF CA
  bundle and sets `MBEDTLS_SSL_VERIFY_REQUIRED` with the actual hostname
  (`mbedtls_ssl_set_hostname("api.telegram.org")`), even when connecting to a
  pinned IP address.
- **Least privilege.** The bot accepts commands only from the configured admin
  chat id (when set). Repository automation uses read-only tokens except where
  explicitly required (release publish, attestation).
- **Fail-safe defaults.** If desync injection fails, the connection falls back
  to normal TLS and logs a warning; no plaintext fallback exists.
- **Minimal attack surface.** The firmware exposes no listening services; it is
  an outbound-only client. Bypass probes are outbound-only as well and use the
  same certificate-verified TLS stack as normal operation.
- **Anonymous telemetry, on by default.** Anonymous statistics are enabled by
  default (opt-out via `/stats off`, the web UI toggle, or
  `APP_STATS_DEFAULT_ON=n` at build time) and carry no
  identifiers (no device/chat IDs, no SSIDs, no IP addresses); reports use the
  same verified TLS stack and are silently skipped when unavailable.

## Input validation

- The TLS ClientHello parser (`desync_tls.c`) is strictly bounds-checked:
  every length field is validated against the buffer size before use, and the
  reported SNI range is guaranteed to lie inside the input.
- The parser and decoy builder are continuously fuzzed with libFuzzer under
  AddressSanitizer/UBSan (`fuzz/fuzz_tls.c`, 90 seconds per CI run).
- Telegram updates are parsed with strict length checks; only the configured
  commands are accepted.
- Network responses are consumed through mbedTLS with bounded buffers.

## Known limitations

- The desync techniques are heuristic and provider-specific
  (see `DISCLAIMER.md`); they do not provide confidentiality by themselves —
  TLS does.
- The device trusts the local network for Wake-on-LAN broadcast.
- IPv6 and IP-level blocking are out of scope.
