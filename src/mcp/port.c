#include "mcp/port.h"

#include <errno.h>
#include <stdlib.h>

/* プラットフォーム非依存に書く（Winsockもwindows.hもincludeしない）。Windows専用
 * 機能のロジックだが、macOS/Linuxのビルドでもここをテストできるのが
 * の核心（sock_path.c の拡張ではなく新規ファイルに切り出した理由）。 */

mcp_port_src_t mcp_port_resolve(const char *env, uint16_t fallback, uint16_t *out) {
    if (env == NULL || env[0] == '\0') {
        *out = fallback;
        return MCP_PORT_SRC_DEFAULT;
    }
    /* 全文字がASCII数字であることを先に検査する。isdigit()（<ctype.h>）は
     * ロケール依存かつcharをunsigned charへ昇格させないと未定義動作になるため、
     * 範囲比較で書く。この一括検査で、先頭の空白・符号(+/-)・アンダースコア・
     * 末尾のごみ・全角数字がすべて同じ1つの規則で落ちる
     * （strtoul単体の寛容さに頼らない。Pythonのparse_port()と同一の受理規則）。 */
    for (const char *p = env; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            *out = 0;
            return MCP_PORT_SRC_INVALID;
        }
    }
    errno = 0;
    char *end = NULL;
    unsigned long v = strtoul(env, &end, 10);
    if (*end != '\0' || errno == ERANGE || v < MCP_PORT_MIN || v > MCP_PORT_MAX) {
        *out = 0;
        return MCP_PORT_SRC_INVALID;
    }
    *out = (uint16_t)v;
    return MCP_PORT_SRC_ENV;
}

static uint16_t s_mcp_port;
static int s_mcp_port_ready;
static mcp_port_src_t s_mcp_port_source;

uint16_t mcp_port(void) {
    if (!s_mcp_port_ready) {
        s_mcp_port_ready = 1;
        s_mcp_port_source =
            mcp_port_resolve(getenv("IX68K_MCP_PORT"), MCP_DEFAULT_PORT, &s_mcp_port);
    }
    return s_mcp_port;
}

mcp_port_src_t mcp_port_source(void) {
    mcp_port();
    return s_mcp_port_source;
}
