#pragma once

#include <stddef.h>

/* MCPソケットパスの解決結果の由来。呼び出し側（server.c）が診断ログを
 * 出し分けるために使う。この層は pal_log に依存させない（テストから素で
 * 呼べるようにするため）。 */
typedef enum {
    MCP_SOCK_SRC_DEFAULT = 0, /* 環境変数が無い（または空） → 既定パスを採用 */
    MCP_SOCK_SRC_ENV,         /* 環境変数 IX68K_MCP_SOCK の値を採用 */
    MCP_SOCK_SRC_TOO_LONG,    /* sun_path に収まらない → out は空文字列 */
} mcp_sock_src_t;

/* env（getenv()の戻り値。NULL可）と fallback から使用するソケットパスを決める。
 * out_size には sockaddr_un::sun_path のサイズ（終端NUL込み）を渡すこと。
 * 採用したい文字列が out_size - 1 を超える場合は MCP_SOCK_SRC_TOO_LONG を返し、
 * out には空文字列を入れる（切り詰めた文字列は返さない: 意図と違うパスへ
 * bind してしまうサイレント事故を防ぐため）。 */
mcp_sock_src_t mcp_sock_path_resolve(const char *env, const char *fallback, char *out,
                                     size_t out_size);

/* 実際に使用するソケットパス。初回呼び出しで解決してキャッシュし、以後は
 * 同じ内容を返す（mcp_init と mcp_cleanup で必ず同じパスを見るため）。
 * 解決に失敗した場合（パスが長すぎる）は NULL。 */
const char *mcp_sock_path(void);

/* 直近の mcp_sock_path() の解決結果の由来（ログ用）。 */
mcp_sock_src_t mcp_sock_path_source(void);
