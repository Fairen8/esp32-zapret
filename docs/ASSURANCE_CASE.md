# Assurance case

This document presents the security assurance case for esp32-zapret: the claim,
the argument, and the evidence available for independent verification.

## Claim

The esp32-zapret firmware is adequately secure for its intended use: an
outbound-only Telegram bot with a DPI-circumvention component, handling no
long-lived secrets beyond the device configuration, and distributed with
traceable artifacts.

## Sub-claims and evidence

### C1. Memory safety of untrusted-input parsing

- The TLS ClientHello parser is fully bounds-checked and validates every length
  field before use (`components/esp_desync/desync_tls.c`).
- The parser and decoy builder are fuzzed with libFuzzer under ASan/UBSan on
  every CI run (`fuzz/fuzz_tls.c`, job `fuzz smoke test`).
- Host unit tests cover malformed/truncated inputs (`tests/host/test_tls.c`).

### C2. No secrets in the repository

- GitHub secret scanning with push protection is enabled.
- CI job `repo hygiene` fails if credential-like files or token-shaped strings
  are tracked.
- The full git history was audited; real credentials live only in the
  git-ignored `main/secrets.h`.

### C3. Trustworthy transport

- TLS via mbedTLS with mandatory certificate verification (CA bundle) and
  hostname verification (`api.telegram.org`), even when a pinned IP is used.
- No custom cryptography; only modern primitives (ECDHE, AES-GCM, SHA-256).

### C4. Supply chain integrity

- All GitHub Actions are pinned to full commit SHAs; Dependabot proposes
  updates.
- Branch protection: `main` requires PRs with 6 checks; `releases` requires
  PRs with checks and a deploy approval (protected environment); tags `v*` are
  protected against deletion/rewrite.
- Releases are built by CI from the tagged commit and published with signed
  SLSA build provenance (`*.intoto.jsonl`).

### C5. Dependency hygiene

- Direct dependencies are declared in `components/esp_desync/idf_component.yml`
  and pinned by the ESP-IDF version used in CI.
- Dependabot alerts and automated security fixes are enabled; CodeQL runs on
  every push/PR and weekly.

### C6. Access control

- Repository access requires a 2FA-enabled GitHub account.
- The only account with write access is the maintainer's.
- CI default token permissions are read-only; write scopes are granted only to
  the release/attestation jobs on the protected `releases` branch.

### C7. Incident response

- `SECURITY.md` documents the private reporting channel (GitHub Security
  Advisories), the maintainer contact, and expected response times.
- Fixes are shipped through the automated release pipeline.

### C8. Adaptive probing is safe

- All strategy probes are outbound TLS connections to api.telegram.org using
  the same verified stack (CA bundle + hostname verification) as normal
  operation; no inbound port is opened.
- Probe cadence is bounded (boot-time scan plus one probe per
  `APP_HEALTH_CHECK_INTERVAL_S`), and the selected strategy is cached in NVS.

## Verification

All evidence is reproducible from the public repository:
CI runs are visible under *Actions*, release provenance is attached to every
release, and the fuzz/test jobs can be run locally (`bash tests/host/run.sh`,
see `.github/workflows/tests.yml` for the fuzz build commands).

## Residual risk

- DPI circumvention effectiveness is provider-specific and not guaranteed
  (documented in `DISCLAIMER.md` and the README limitations section).
- Builds are not claimed to be bit-for-bit reproducible; provenance proves the
  artifact was produced by this repository's CI workflow from the tagged commit.
