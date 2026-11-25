#include "mcp/sock_path.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_env_missing_uses_fallback(void) {
    char out[32];
    mcp_sock_src_t src = mcp_sock_path_resolve(NULL, "/tmp/x68k_mcp.sock", out, sizeof(out));
    assert(src == MCP_SOCK_SRC_DEFAULT);
    assert(strcmp(out, "/tmp/x68k_mcp.sock") == 0);
}

static void test_env_empty_uses_fallback(void) {
    char out[32];
    mcp_sock_src_t src = mcp_sock_path_resolve("", "/tmp/x68k_mcp.sock", out, sizeof(out));
    assert(src == MCP_SOCK_SRC_DEFAULT);
    assert(strcmp(out, "/tmp/x68k_mcp.sock") == 0);
}

static void test_env_set_overrides(void) {
    char out[32];
    mcp_sock_src_t src =
        mcp_sock_path_resolve("/tmp/x68k_mcp_a.sock", "/tmp/x68k_mcp.sock", out, sizeof(out));
    assert(src == MCP_SOCK_SRC_ENV);
    assert(strcmp(out, "/tmp/x68k_mcp_a.sock") == 0);
}

static void test_env_exact_fit(void) {
    char out[8];
    /* out_size=8 → 収まる最大は7文字+NUL。 */
    mcp_sock_src_t src = mcp_sock_path_resolve("1234567", "fallback", out, sizeof(out));
    assert(src == MCP_SOCK_SRC_ENV);
    assert(strcmp(out, "1234567") == 0);
}

static void test_env_one_byte_over(void) {
    char out[8];
    mcp_sock_src_t src = mcp_sock_path_resolve("12345678", "fallback", out, sizeof(out));
    assert(src == MCP_SOCK_SRC_TOO_LONG);
    assert(out[0] == '\0'); /* 切り詰めた文字列を返さない（D6の核心）。 */
}

static void test_fallback_too_long(void) {
    char out[8];
    mcp_sock_src_t src = mcp_sock_path_resolve(NULL, "123456789", out, sizeof(out));
    assert(src == MCP_SOCK_SRC_TOO_LONG);
    assert(out[0] == '\0');
}

static void test_out_size_extreme(void) {
    char out[1];
    mcp_sock_src_t src = mcp_sock_path_resolve(NULL, "a", out, sizeof(out));
    assert(src == MCP_SOCK_SRC_TOO_LONG);
    assert(out[0] == '\0');
}

static void test_mcp_sock_path_cached(void) {
    const char *p1 = mcp_sock_path();
    const char *p2 = mcp_sock_path();
    assert(p1 != NULL);
    assert(p1 == p2); /* 同一ポインタ＝mcp_init/mcp_cleanupが同じパスを見る */
}

int main(void) {
    test_env_missing_uses_fallback();
    test_env_empty_uses_fallback();
    test_env_set_overrides();
    test_env_exact_fit();
    test_env_one_byte_over();
    test_fallback_too_long();
    test_out_size_extreme();
    test_mcp_sock_path_cached();
    printf("mcp_sock_path_test: OK\n");
    return 0;
}
