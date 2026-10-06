// SPDX-License-Identifier: MIT
//
// libFuzzer target for the TLS ClientHello parser and decoy builder.
// Build (CI): clang -fsanitize=fuzzer-no-link,address,undefined ... then link
// with clang++ -fsanitize=fuzzer,address,undefined.
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "desync_tls.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    size_t off = 0;
    size_t len = 0;

    if (desync_tls_find_sni(data, size, &off, &len) == 0) {
        /* Invariant: a reported SNI range must be fully inside the input. */
        if (len == 0 || off >= size || len > size - off) {
            __builtin_trap();
        }
    }

    uint8_t fake[DESYNC_FAKE_MAX];
    char sni[96];
    size_t n = size < sizeof(sni) - 1 ? size : sizeof(sni) - 1;
    memcpy(sni, data, n);
    sni[n] = '\0';

    size_t fake_len = desync_tls_build_fake(fake, sizeof(fake), sni, data, size, true);
    if (fake_len > 0) {
        size_t foff = 0;
        size_t flen = 0;
        if (desync_tls_find_sni(fake, fake_len, &foff, &flen) == 0) {
            if (flen == 0 || foff >= fake_len || flen > fake_len - foff) {
                __builtin_trap();
            }
        }
    }

    return 0;
}
