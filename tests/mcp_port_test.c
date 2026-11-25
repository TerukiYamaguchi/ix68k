#include "mcp/port.h"

#include <assert.h>
#include <stdio.h>

static void test_env_missing_uses_fallback(void) {
    uint16_t out = 0;
    mcp_port_src_t src = mcp_port_resolve(NULL, MCP_DEFAULT_PORT, &out);
    assert(src == MCP_PORT_SRC_DEFAULT);
    assert(out == MCP_DEFAULT_PORT);
}

static void test_env_empty_uses_fallback(void) {
    uint16_t out = 0;
    mcp_port_src_t src = mcp_port_resolve("", MCP_DEFAULT_PORT, &out);
    assert(src == MCP_PORT_SRC_DEFAULT);
    assert(out == MCP_DEFAULT_PORT);
}

static void test_env_set_overrides(void) {
    uint16_t out = 0;
    mcp_port_src_t src = mcp_port_resolve("26801", MCP_DEFAULT_PORT, &out);
    assert(src == MCP_PORT_SRC_ENV);
    assert(out == 26801);
}

static void test_boundary_min_max(void) {
    uint16_t out = 0;
    assert(mcp_port_resolve("1024", MCP_DEFAULT_PORT, &out) == MCP_PORT_SRC_ENV);
    assert(out == 1024);
    assert(mcp_port_resolve("65535", MCP_DEFAULT_PORT, &out) == MCP_PORT_SRC_ENV);
    assert(out == 65535);
}

static void test_boundary_just_outside(void) {
    uint16_t out = 1;
    assert(mcp_port_resolve("1023", MCP_DEFAULT_PORT, &out) == MCP_PORT_SRC_INVALID);
    assert(out == 0);
    out = 1;
    assert(mcp_port_resolve("65536", MCP_DEFAULT_PORT, &out) == MCP_PORT_SRC_INVALID);
    assert(out == 0);
}

static void test_zero_is_invalid(void) {
    /* 0は「OSに任せる」の意味になり、Python側から発見不能なので不正とする。 */
    uint16_t out = 1;
    assert(mcp_port_resolve("0", MCP_DEFAULT_PORT, &out) == MCP_PORT_SRC_INVALID);
    assert(out == 0);
}

static void test_negative_does_not_wrap(void) {
    /* strtoulは負数を巨大な符号なし値へ折り返す。明示的に'-'を弾いていることの検査。 */
    uint16_t out = 1;
    assert(mcp_port_resolve("-1", MCP_DEFAULT_PORT, &out) == MCP_PORT_SRC_INVALID);
    assert(out == 0);
}

static void test_non_ascii_digit_garbage(void) {
    uint16_t out;
    out = 1;
    assert(mcp_port_resolve("abc", MCP_DEFAULT_PORT, &out) == MCP_PORT_SRC_INVALID);
    assert(out == 0);
    out = 1;
    assert(mcp_port_resolve("26800x", MCP_DEFAULT_PORT, &out) == MCP_PORT_SRC_INVALID);
    assert(out == 0);
    out = 1;
    assert(mcp_port_resolve(" 26800", MCP_DEFAULT_PORT, &out) == MCP_PORT_SRC_INVALID);
    assert(out == 0);
    out = 1;
    assert(mcp_port_resolve("26800 ", MCP_DEFAULT_PORT, &out) == MCP_PORT_SRC_INVALID);
    assert(out == 0);
    out = 1;
    assert(mcp_port_resolve("268 00", MCP_DEFAULT_PORT, &out) == MCP_PORT_SRC_INVALID);
    assert(out == 0);
}

static void test_huge_input_overflow(void) {
    uint16_t out = 1;
    mcp_port_src_t src =
        mcp_port_resolve("99999999999999999999", MCP_DEFAULT_PORT, &out);
    assert(src == MCP_PORT_SRC_INVALID);
    assert(out == 0);
}

static void test_mcp_port_cached(void) {
    uint16_t p1 = mcp_port();
    uint16_t p2 = mcp_port();
    mcp_port_src_t s1 = mcp_port_source();
    mcp_port_src_t s2 = mcp_port_source();
    assert(p1 == p2);
    assert(s1 == s2);
}

static void test_plus_sign_rejected(void) {
    /* strtoulは'+'を受理してしまう（Pythonのint()も同様）。両側で弾くことの明示検査。D14。 */
    uint16_t out = 1;
    assert(mcp_port_resolve("+26800", MCP_DEFAULT_PORT, &out) == MCP_PORT_SRC_INVALID);
    assert(out == 0);
}

static void test_underscore_separator_rejected(void) {
    /* C側はASCII数字チェックで自然に落ちるが、Pythonのint()は受理する（tests/test_mcp_port_py.py
     * と同じ入力表を共有する。D14）。 */
    uint16_t out = 1;
    assert(mcp_port_resolve("26_800", MCP_DEFAULT_PORT, &out) == MCP_PORT_SRC_INVALID);
    assert(out == 0);
    out = 1;
    assert(mcp_port_resolve("1_000", MCP_DEFAULT_PORT, &out) == MCP_PORT_SRC_INVALID);
    assert(out == 0);
}

static void test_fullwidth_digit_rejected(void) {
    /* 全角"２６８００"（UTF-8バイト列）。Pythonのint()は26800として受理してしまう
     * 入力（tests/test_mcp_port_py.py と同じ入力表を共有する。D14）。 */
    uint16_t out = 1;
    assert(mcp_port_resolve("\xef\xbc\x92\xef\xbc\x96\xef\xbc\x98\xef\xbc\x90\xef\xbc\x90",
                            MCP_DEFAULT_PORT, &out) == MCP_PORT_SRC_INVALID);
    assert(out == 0);
}

int main(void) {
    test_env_missing_uses_fallback();
    test_env_empty_uses_fallback();
    test_env_set_overrides();
    test_boundary_min_max();
    test_boundary_just_outside();
    test_zero_is_invalid();
    test_negative_does_not_wrap();
    test_non_ascii_digit_garbage();
    test_huge_input_overflow();
    test_mcp_port_cached();
    test_plus_sign_rejected();
    test_underscore_separator_rejected();
    test_fullwidth_digit_rejected();
    printf("mcp_port_test: OK\n");
    return 0;
}
