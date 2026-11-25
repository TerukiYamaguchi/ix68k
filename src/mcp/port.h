#pragma once

#include <stdint.h>

/* MCP TCPポート番号の解決結果の由来。呼び出し側（server.c / main.c）が診断ログと
 * ウィンドウタイトルを出し分けるために使う。この層は pal_log に依存させない
 * （テストから素で呼べるようにするため、sock_path.h と同じ方針）。 */
typedef enum {
    MCP_PORT_SRC_DEFAULT = 0, /* 環境変数が無い（または空） → 既定ポートを採用 */
    MCP_PORT_SRC_ENV,         /* 環境変数 IX68K_MCP_PORT の値を採用 */
    MCP_PORT_SRC_INVALID,     /* 構文エラーまたは範囲外 → *out は 0 */
} mcp_port_src_t;

/* 既定ポート。IANA well-known(0-1023)と、Windows/Linuxが動的に払い出す
 * ephemeralレンジの外側から選んでいる。
 * tools-host/mcp/ix68k_mcp_server.py と tools-host/ix68k-hostfile/ix68k-hostfile にも同じ値がある。
 * 変えるときは3箇所同時に直すこと。 */
#define MCP_DEFAULT_PORT 26800

/* 受け付けるポート番号の範囲。 */
#define MCP_PORT_MIN 1024
#define MCP_PORT_MAX 65535

/* env（getenv()の戻り値。NULL可）と fallback から使用するポート番号を決める。
 * 受理規則: 空でない文字列であり、ASCII数字 '0'-'9' だけから成り、その10進値が
 * MCP_PORT_MIN 以上 MCP_PORT_MAX 以下であること。符号（+/-）・前後の空白・
 * アンダースコア区切り・全角数字はいずれも不正（tools-host/mcp/ix68k_mcp_server.py と
 * tools-host/ix68k-hostfile/ix68k-hostfile の parse_port() と同一の受理規則にすること）。
 * 不正な値のときは MCP_PORT_SRC_INVALID を返し *out に 0 を入れる（既定へ
 * フォールバックしない: 別インスタンスへ気付かず接続する事故を防ぐため）。 */
mcp_port_src_t mcp_port_resolve(const char *env, uint16_t fallback, uint16_t *out);

/* 実際に使用するポート番号。初回呼び出しで解決してキャッシュする。
 * 解決に失敗した場合は 0（呼び出し側はこれを「MCPを起動しない」と解釈する）。 */
uint16_t mcp_port(void);

/* 直近の mcp_port() の解決結果の由来（ログ／ウィンドウタイトル用）。 */
mcp_port_src_t mcp_port_source(void);
