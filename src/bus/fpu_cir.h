#pragma once
/*
 * FPU-CIR（CZ-6BP1/CZ-6BP1A, MC68881）の CIR adapter。
 *
 * 16-bit の CIR access を mc68000 の公開 FPU API（mc68000/fpu.h）へ変換するだけで、
 * FPU の状態機械は持たない。保持するのは busy 表示、operand の word ストリーム位置、
 * 転送記述子（残り byte 数）のみ。モデルが返した response は保持しない。
 */
#include "mc68000/fpu.h"
#include <stdbool.h>
#include <stdint.h>

/* window 内 offset（881UM Table 7-2 / アウトサイド X68000 図4） */
#define FPU_CIR_OFF_RESPONSE   0x00u
#define FPU_CIR_OFF_CONTROL    0x02u
#define FPU_CIR_OFF_SAVE       0x04u
#define FPU_CIR_OFF_RESTORE    0x06u
#define FPU_CIR_OFF_COMMAND    0x0Au
#define FPU_CIR_OFF_CONDITION  0x0Eu
#define FPU_CIR_OFF_OPERAND_HI 0x10u
#define FPU_CIR_OFF_OPERAND_LO 0x12u
#define FPU_CIR_OFF_REGSEL     0x14u
#define FPU_CIR_OFF_IADDR_HI   0x18u
#define FPU_CIR_OFF_IADDR_LO   0x1Au

/* 未実装/逆方向/拒否された read のデータ */
#define FPU_CIR_OPEN_WORD      0xFFFFu
/* CIR は16-bit ポート。long は上位 word・下位 word の二つの cycle */
#define FPU_CIR_WORD_BITS      16
/* 最初の response read が返す busy（CA=1, IA=1 の NULL primitive） */
#define FPU_CIR_BUSY_PRIMITIVE 0x8900u

typedef struct {
    bool busy_once; /* 次の response read が $8900 を返す（command/condition write で立つ） */
    uint32_t remaining; /* 転送記述子: operand の残り byte 数（0 は「無い/尽きた」） */
    bool wr_partial;    /* operand write の上位 word を保持中 */
    uint16_t wr_hi;
    uint32_t wr_consumed; /* 上位 word の保持時に記述子から引いた byte 数（破棄時に戻す） */
    bool rd_latch;        /* operand read の下位 word を保持中 */
    uint16_t rd_lo;
    bool iaddr_partial; /* instruction address の上位 word を保持中 */
    uint16_t iaddr_hi;
    bool restore_started;      /* restore CIR を一度でも書いた */
    uint16_t restore_readback; /* 直近の begin_restore() の readback */
    uint32_t violation_count;  /* FPU API が false を返した回数（診断・テスト用） */
} fpu_cir_t;

void fpu_cir_init(fpu_cir_t *cir);

/* response を 881UM Table 7-7 の16-bit primitive へ符号化する（純関数） */
uint16_t fpu_cir_encode_response(const mc68k_fpu_response_t *response);

/* offset は window 先頭からの偶数 offset（0..$1E）。FPU API の失敗(protocol violation)は
 * cycle を完了させ、read は全1を返す。 */
uint16_t fpu_cir_read16(fpu_cir_t *cir, mc68k_fpu_t *fpu, uint32_t offset);
void fpu_cir_write16(fpu_cir_t *cir, mc68k_fpu_t *fpu, uint32_t offset, uint16_t value);
