#include "mcp/sock_path.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <afunix.h>
#include <windows.h>
/* <stdio.h> が無いと _snprintf() が暗黙宣言となり、legacy_stdio_definitions.lib への
 * /DEFAULTLIB 埋め込みが起きず単体リンク時に未解決シンボルになる（x68000本体は他の
 * TU経由でたまたま解決していた）。 */
#include <stdio.h>
#else
#include <sys/un.h>
#endif

/* sockaddr_un::sun_path の実サイズを実環境から取る（プラットフォームで
 * 異なるためハードコードしない。macOS=104、Linux/Windows afunixは108）。 */
#define MCP_SOCK_PATH_MAX (sizeof(((struct sockaddr_un *)0)->sun_path))

mcp_sock_src_t mcp_sock_path_resolve(const char *env, const char *fallback, char *out,
                                     size_t out_size) {
    int use_env = env && env[0] != '\0';
    const char *chosen = use_env ? env : fallback;
    size_t len = strlen(chosen);
    if (out_size == 0 || len >= out_size) {
        if (out_size > 0)
            out[0] = '\0';
        return MCP_SOCK_SRC_TOO_LONG;
    }
    memcpy(out, chosen, len + 1);
    return use_env ? MCP_SOCK_SRC_ENV : MCP_SOCK_SRC_DEFAULT;
}

#ifdef _WIN32
/* 旧 server.c の mcp_sock_path() から移設。%TEMP%\ix68k_mcp.sock を組み立てる。 */
static const char *mcp_sock_default_path(void) {
    static char path[MAX_PATH];
    if (path[0] == '\0') {
        char tmp[MAX_PATH];
        DWORD n = GetTempPathA((DWORD)sizeof(tmp), tmp);
        if (n == 0 || n >= sizeof(tmp))
            strcpy(tmp, ".\\");
        _snprintf(path, sizeof(path) - 1, "%six68k_mcp.sock", tmp);
        path[sizeof(path) - 1] = '\0';
    }
    return path;
}
#else
static const char *mcp_sock_default_path(void) {
    return "/tmp/ix68k_mcp.sock";
}
#endif

static char s_mcp_sock_path[MCP_SOCK_PATH_MAX];
static int s_mcp_sock_path_ready;
static mcp_sock_src_t s_mcp_sock_path_source;

const char *mcp_sock_path(void) {
    if (!s_mcp_sock_path_ready) {
        s_mcp_sock_path_ready = 1;
        s_mcp_sock_path_source =
            mcp_sock_path_resolve(getenv("IX68K_MCP_SOCK"), mcp_sock_default_path(),
                                  s_mcp_sock_path, sizeof(s_mcp_sock_path));
    }
    return s_mcp_sock_path_source == MCP_SOCK_SRC_TOO_LONG ? NULL : s_mcp_sock_path;
}

mcp_sock_src_t mcp_sock_path_source(void) {
    mcp_sock_path();
    return s_mcp_sock_path_source;
}
