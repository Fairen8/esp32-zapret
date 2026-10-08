// SPDX-License-Identifier: MIT
//
// libFuzzer target for the DoH JSON parser (doh_parse.c).
// Build (CI): clang -fsanitize=fuzzer-no-link,address,undefined ... then link
// with clang++ -fsanitize=fuzzer,address,undefined.
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "doh_parse.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    char json[4096];
    if (size >= sizeof(json)) {
        size = sizeof(json) - 1;
    }
    memcpy(json, data, size);
    json[size] = '\0';

    uint32_t addrs[8];
    int n = doh_parse_a_records(json, addrs, 8);
    /* Invariant: the parser never reports more than max_addrs. */
    if (n > 8) {
        __builtin_trap();
    }
    return 0;
}
