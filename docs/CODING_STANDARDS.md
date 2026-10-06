# Coding standards

These standards apply to all C sources and project files. They are enforced in
CI where technically possible.

## Language and style

- **C17** (`-std=c17`), no compiler-specific extensions beyond what ESP-IDF
  requires. C++ is not used.
- 4-space indentation, no tabs. Lines should stay under ~100 columns.
- `snake_case` for functions and variables, `UPPER_SNAKE_CASE` for macros,
  `typedef struct { ... } name_t;` for types.
- Internal helpers are `static` and not exported.
- Every source file starts with `// SPDX-License-Identifier: MIT`.
- Comments and identifiers are in English.

## Error handling

- Functions return explicit error indicators (`int`, `esp_err_t`, `ssize_t`);
  silent failures are not allowed.
- All network/socket calls check their return values; error paths release
  resources (no leaks on failure).
- Buffer lengths are always carried alongside pointers and validated before
  every read/write.

## Memory and concurrency

- No dynamic allocation in hot paths; fixed-size buffers where possible.
- LwIP state (`tcp_active_pcbs`) is accessed only through the tcpip task via
  `tcpip_callback`; cross-task signalling uses FreeRTOS semaphores.
- Stack usage is kept small; large buffers are static or heap-allocated.

## Security-relevant rules

- Never log or persist secrets; credentials live only in the git-ignored
  `main/secrets.h`.
- Treat all external input as untrusted: validate lengths and bounds before use
  (see `docs/SECURITY_DESIGN.md`).
- Do not implement custom cryptography; use mbedTLS.

## Enforcement

- CI builds with warnings as errors (ESP-IDF `-Werror=all`; host tests and
  analyzer jobs use `-Wall -Wextra -Werror`).
- CodeQL static analysis runs on every push/PR; findings gate CI.
- The fuzz target exercises the parser on every CI run.
- All changes are merged via reviewed pull requests (`CONTRIBUTING.md`).
