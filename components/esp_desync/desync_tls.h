// SPDX-License-Identifier: MIT
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define DESYNC_FAKE_MAX 512

/* Find the SNI hostname inside a TLS ClientHello. Returns 0 on success and
 * fills sni_off/sni_len, -1 if the buffer is not a parseable ClientHello
 * with SNI. */
int desync_tls_find_sni(const uint8_t *buf, size_t len, size_t *sni_off, size_t *sni_len);

/* Build a minimal decoy ClientHello with the given SNI. If clone_random is set
 * and real points to a valid hello, the client_random is copied from it.
 * Returns the number of bytes written, 0 if out is too small. */
size_t desync_tls_build_fake(uint8_t *out, size_t out_sz, const char *sni,
                             const uint8_t *real, size_t real_len, bool clone_random);
