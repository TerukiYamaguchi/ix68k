#include "peripheral/sha256.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void assert_hash(const char *input, const char *expected) {
    uint8_t hash[32];
    char actual[65];
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, input, strlen(input));
    sha256_final(&ctx, hash);
    for (unsigned i = 0; i < sizeof(hash); i++)
        snprintf(actual + i * 2, 3, "%02x", hash[i]);
    assert(strcmp(actual, expected) == 0);
}

int main(void) {
    assert_hash("", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    assert_hash("abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    assert_hash("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
                "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    sha256_ctx_t million_ctx;
    uint8_t million_hash[32];
    char million_actual[65];
    char chunk[1000];
    memset(chunk, 'a', sizeof(chunk));
    sha256_init(&million_ctx);
    for (int i = 0; i < 1000; i++)
        sha256_update(&million_ctx, chunk, sizeof(chunk));
    sha256_final(&million_ctx, million_hash);
    for (unsigned i = 0; i < sizeof(million_hash); i++)
        snprintf(million_actual + i * 2, 3, "%02x", million_hash[i]);
    assert(strcmp(million_actual,
                  "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0);
    puts("media_fp_test: OK");
    return 0;
}
