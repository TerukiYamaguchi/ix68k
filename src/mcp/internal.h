#pragma once

#include <stddef.h>
#include <stdint.h>

#include "bus/x68k_bus.h"

/* 非同期待ちの種類。受信フレームでは返信せず、後続フレームの poll で完了を
 * 判定して応答する操作はこれで表す（待ちの種類ごとに専用のフィールドと
 * 状態機械を増やさないための共通化）。追加するときは commands.c の
 * s_wait_ops に「完了判定」と「応答生成」の2関数を足すだけでよい。 */
typedef enum {
    MCP_WAIT_NONE = 0,
    MCP_WAIT_KEYIN, /* KEY WAIT: キー入力待ちになるまで */
    MCP_WAIT_DBG,   /* DBG WAIT: デバッガが停止するまで */
    MCP_WAIT_KIND_COUNT,
} mcp_wait_kind_t;

typedef struct {
    mcp_wait_kind_t wait_kind; /* MCP_WAIT_NONE = 待ち無し */
    uint64_t wait_deadline;

    /* B_PUTC/B_PRINT の出力キャプチャ。バッファ本体は server.c が所有する。 */
    uint8_t *output;
    int output_len;
    int output_capacity;

    /* PASTE のペーシングキュー。 */
    uint8_t paste_queue[512];
    int paste_len;
    int paste_idx;
    uint64_t paste_next_us;

    /* リクエストの本文バイト列（ヘッダ行の次の '\n' 以降）。MEM WRITE等の
     * 生バイナリ引数に使う。server.c が1コマンドの処理中だけ生存を保証する
     * （非同期待ちに持ち越さないこと）。本文が無いフレームでは NULL/0。 */
    const uint8_t *request_body;
    size_t request_body_len;
} mcp_context_t;

/* server.c が所有する現在のクライアントへ応答を送る。どちらも全バイトを送り切る
 * （送信バッファ長で切れない）。成功したとき非0を返す。
 * mcp_reply: 完結ヘッダ行（"OK\n" / "ERR <CODE> <msg>\n" 等）だけの応答。
 * mcp_reply_bin: 成功（"OK\n"）＋生バイナリ本体の応答（旧base64応答の置き換え）。 */
int mcp_reply(const char *text, size_t len);
int mcp_reply_bin(const uint8_t *data, size_t len);

/*
 * DBG STATUS/STEP/BREAK/WAIT共通のstopinfo応答（"OK <stopinfo>\n"）を
 * infoとreplyの両方の容量内で組み立てる（commands.cのmcp_send_stopinfo()から
 * 切り出し。テストからinfo_sz/reply_szを小さくして切り詰め検出経路を
 * 直接検証できるようにするのが目的）。
 * 戻り値: 成功時はreplyへ書き込んだ長さ（末尾NUL抜き）。infoまたはreplyの
 * どちらかで切り詰めが起きた場合は負値（内容は不定、送信してはならない）。
 */
int mcp_build_stopinfo_reply(char *info, size_t info_sz, char *reply, size_t reply_sz);
