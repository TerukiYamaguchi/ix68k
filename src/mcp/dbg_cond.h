#pragma once
/*
 * dbg_cond.h — 条件付きブレークポイントの条件式パーサ・評価器。
 *
 * 文法:
 *   cond ::= <term> ["&&" <term>]*        AND のみ（OR は実用上ほぼ不要）
 *   term ::= <reg> <op> <hex>             D0-D7, A0-A7, PC, SR, USP, SSP
 *          | "[" <hex> "]" ("." ("b"|"w"|"l"))? <op> <hex>   メモリ値
 *          | "#" <n>                      パスカウント: 最初のn回(10進)は無視
 *   op   ::= "==" | "!=" | "<" | ">" | "<=" | ">="
 *
 * ソケットコード（mcp/server.c）に依存させず単体テスト可能にする。
 */
#include <stddef.h>
#include <stdint.h>
#include "mc68000/cpu_types.h"

#define DBG_COND_MAX_TERMS 4

typedef enum {
    DBG_COND_TERM_REG,
    DBG_COND_TERM_MEM,
    DBG_COND_TERM_PASSCOUNT
} dbg_cond_term_kind_t;
typedef enum {
    DBG_COND_EQ,
    DBG_COND_NE,
    DBG_COND_LT,
    DBG_COND_GT,
    DBG_COND_LE,
    DBG_COND_GE
} dbg_cond_op_t;

typedef struct {
    dbg_cond_term_kind_t kind;
    char reg_name[4];   /* kind==REG: "D0".."D7"/"A0".."A7"/"PC"/"SR"/"USP"/"SSP" */
    uint32_t mem_addr;  /* kind==MEM */
    int mem_size;       /* kind==MEM: 1/2/4 */
    dbg_cond_op_t op;   /* kind==REG/MEM */
    uint32_t rhs;       /* kind==REG/MEM: 比較値（符号なし比較） */
    int pass_n;         /* kind==PASSCOUNT: 無視する回数の設定値 */
    int pass_remaining; /* kind==PASSCOUNT: 実行時にデクリメントされる残数 */
} dbg_cond_term_t;

typedef struct {
    int term_count;
    dbg_cond_term_t terms[DBG_COND_MAX_TERMS];
} dbg_cond_t;

/*
 * textをパースしてoutへ書き込む。成功時0、構文エラー時-1（outは未変更）。
 * text が NULL または空文字列なら無条件（term_count=0、常にヒット扱い）として
 * out を初期化し0を返す。
 */
int dbg_cond_parse(const char *text, dbg_cond_t *out);

/*
 * 非侵襲評価。mc68k_get_regs()（読み取り専用）とmc68k_debug_read_bytes()
 * （非侵襲）だけを使い、debug.hの「condはCPU状態を変更してはならず、BP APIを
 * 再入呼びしてもいけない」契約を満たす。PASSCOUNT項のpass_remainingだけは
 * 呼び出しごとに更新する（ホスト側レコードの内部状態でありCPU状態ではない）。
 * 全項が真（AND）なら非0を返す。
 */
int dbg_cond_eval(dbg_cond_t *cond, mc68k_state_t *cpu);
