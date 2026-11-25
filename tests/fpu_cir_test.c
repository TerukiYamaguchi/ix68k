/*
 * FPU-CIR adapter（src/bus/fpu_cir.c）の単体テスト。
 *
 * CPU バスのフック（x68k_bus_get_hooks()）経由で、実際の MC68881 モデルを相手に
 * CIR の word/long アクセスを行い、応答 primitive の raw word、busy 1回、operand の word
 * ストリーム、save/restore、中断時の部分状態を検査する。期待する raw word は 881UM Table 7-7 の
 * 値を手で置き、production の定数・encoder から取らない。
 */
#include "bus/bus_internal.h"
#include "bus/fpu_cir.h"
#include "bus/x68k_bus.h"
#include "mc68000/cpu.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define BASE         0xE9E000u
#define R_RESPONSE   0x00u
#define R_CONTROL    0x02u
#define R_SAVE       0x04u
#define R_RESTORE    0x06u
#define R_COMMAND    0x0Au
#define R_CONDITION  0x0Eu
#define R_OPERAND    0x10u
#define R_OPERAND_LO 0x12u
#define R_REGSEL     0x14u
#define R_IADDR      0x18u
#define R_IADDR_LO   0x1Au

#define CMD(cls, field, dst, low) \
    ((uint16_t)(((cls) << 13) | ((field) << 10) | ((dst) << 7) | (low)))
#define FMT_L     0
#define FMT_S     1
#define FMT_X     2
#define FMT_W     4
#define FMT_D     5
#define FMT_B     6
#define OPM_FMOVE 0x00u

/* Table 7-7 の primitive 値 */
#define P_BUSY              0x8900u
#define P_IDLE              0x0802u
#define P_COND_FALSE        0x0800u
#define P_COND_TRUE         0x0801u
#define P_EA_IN_B           0x9501u
#define P_EA_IN_W           0x9502u
#define P_EA_IN_L           0x9504u
#define P_EA_OUT_L          0xB104u
#define P_EA_OUT_D          0xB208u
#define P_TAKE_MID_PROTOCOL 0x1D0Du

static x68k_bus_t *s_bus;
static mc68k_bus_t s_h;

static void setup(void) {
    s_bus = x68k_bus_create();
    assert(s_bus);
    assert(x68k_bus_set_fpu_cir(s_bus, 1, X68K_FPU_CIR_PRIMARY));
    s_h = x68k_bus_get_hooks(s_bus);
}

static void teardown(void) {
    x68k_bus_destroy(s_bus);
}

static uint16_t rd(uint32_t off) {
    uint16_t v = 0;
    assert(s_h.read_word(s_h.ctx, BASE + off, &v));
    return v;
}

static void wr(uint32_t off, uint16_t v) {
    assert(s_h.write_word(s_h.ctx, BASE + off, v));
}

static uint32_t rd32(uint32_t off) {
    uint32_t v = 0;
    assert(s_h.read_long(s_h.ctx, BASE + off, &v));
    return v;
}

static void wr32(uint32_t off, uint32_t v) {
    assert(s_h.write_long(s_h.ctx, BASE + off, v));
}

static mc68k_fpu_regs_t regs(void) {
    mc68k_fpu_regs_t r;
    mc68k_get_fpu_regs(s_bus->fpu_cir, &r);
    return r;
}

/* command を書き、busy の後に最初の primitive を読む */
static uint16_t command(uint16_t cmd) {
    wr(R_COMMAND, cmd);
    assert(rd(R_RESPONSE) == P_BUSY);
    return rd(R_RESPONSE);
}

static void load_single(unsigned fpn, uint32_t bits) {
    assert(command(CMD(2, FMT_S, fpn, OPM_FMOVE)) == P_EA_IN_L);
    wr32(R_OPERAND, bits);
    assert(rd(R_RESPONSE) == P_IDLE);
}

/* ---- encoder ---- */

static mc68k_fpu_response_t resp(mc68k_fpu_response_kind_t kind) {
    mc68k_fpu_response_t r;
    memset(&r, 0, sizeof(r));
    r.kind = kind;
    return r;
}

static void test_encoder(void) {
    mc68k_fpu_response_t r = resp(MC68K_FPU_RESPONSE_NULL);
    r.processing = false;
    assert(fpu_cir_encode_response(&r) == P_IDLE);
    r.processing = true;
    assert(fpu_cir_encode_response(&r) == 0x0800u);
    r.condition_true = 1;
    assert(fpu_cir_encode_response(&r) == 0x0801u);
    r.condition_true = 0;
    r.interruptible = true;
    assert(fpu_cir_encode_response(&r) == 0x0900u);
    r.coprocessor_active = true;
    assert(fpu_cir_encode_response(&r) == 0x8900u);
    r.request_pc = true;
    assert(fpu_cir_encode_response(&r) == 0xC900u);
    r.coprocessor_active = false;
    assert(fpu_cir_encode_response(&r) == 0x4900u);

    /* EA transfer: CA/PC/DR・valid ea・length */
    r = resp(MC68K_FPU_RESPONSE_EA_TRANSFER);
    r.coprocessor_active = true;
    r.valid_ea = 5;
    r.length = 4;
    assert(fpu_cir_encode_response(&r) == 0x9504u);
    r.request_pc = true;
    assert(fpu_cir_encode_response(&r) == 0xD504u);
    r.request_pc = false;
    r.valid_ea = 6;
    r.length = 12;
    assert(fpu_cir_encode_response(&r) == 0x960Cu);
    r.transfer_to_host = true;
    r.valid_ea = 2;
    r.length = 8;
    assert(fpu_cir_encode_response(&r) == 0xB208u);
    r.coprocessor_active = false;
    r.request_pc = true;
    r.valid_ea = 1;
    r.length = 4;
    assert(fpu_cir_encode_response(&r) ==
           0x7104u); /* Table 7-7 に無い値。Figure 7-7 から導出（PC=1,DR=1,ea=001,len=4） */

    /* MP register: Dn */
    r = resp(MC68K_FPU_RESPONSE_MP_REGISTER);
    r.coprocessor_active = true;
    r.data_register = 0;
    assert(fpu_cir_encode_response(&r) == 0x8C00u);
    r.data_register = 7;
    assert(fpu_cir_encode_response(&r) == 0x8C07u);
    r.request_pc = true;
    r.data_register = 3;
    assert(fpu_cir_encode_response(&r) == 0xCC03u);

    /* FP registers: DR で方向 */
    r = resp(MC68K_FPU_RESPONSE_FP_REGISTERS);
    r.coprocessor_active = true;
    r.length = 12;
    assert(fpu_cir_encode_response(&r) == 0x810Cu);
    r.transfer_to_host = true;
    assert(fpu_cir_encode_response(&r) == 0xA10Cu);

    /* take exception */
    r = resp(MC68K_FPU_RESPONSE_TAKE_PRE_EXCEPTION);
    r.vector = 0x0B;
    assert(fpu_cir_encode_response(&r) == 0x1C0Bu);
    r.vector = 0x31;
    assert(fpu_cir_encode_response(&r) == 0x1C31u);
    r.request_pc = true;
    r.vector = 0x30;
    assert(fpu_cir_encode_response(&r) == 0x5C30u);
    r = resp(MC68K_FPU_RESPONSE_TAKE_MID_EXCEPTION);
    r.vector = 0x0D;
    assert(fpu_cir_encode_response(&r) == 0x1D0Du);
    r.vector = 0x36;
    assert(fpu_cir_encode_response(&r) == 0x1D36u);
    printf("fpu_cir_test: encoder OK\n");
}

/* ---- 方向・予約位置 ---- */

static void test_open_and_ignored_positions(void) {
    static const uint32_t read_open[] = {R_CONTROL, R_COMMAND, R_CONDITION, R_IADDR, R_IADDR_LO,
                                         0x08u,     0x0Cu,     0x16u,       0x1Cu,   0x1Eu};
    static const uint32_t write_ignored[] = {R_RESPONSE, R_SAVE, 0x08u, 0x0Cu,
                                             0x16u,      0x1Cu,  0x1Eu};
    setup();
    for (unsigned i = 0; i < sizeof(read_open) / sizeof(read_open[0]); i++)
        assert(rd(read_open[i]) == 0xFFFFu);
    for (unsigned i = 0; i < sizeof(write_ignored) / sizeof(write_ignored[0]); i++)
        wr(write_ignored[i], 0x1234u);
    assert(s_bus->fpu_cir_state.violation_count == 0);
    assert(!s_bus->fpu_cir_state.busy_once);
    assert(rd(R_RESPONSE) == P_IDLE); /* 状態不変: 最初の read も busy ではない */
    assert(rd(R_RESTORE) == 0xFFFFu); /* restore 未開始 */
    teardown();
    printf("fpu_cir_test: open/ignored positions OK\n");
}

/* ---- response・busy ---- */

static void test_condition_and_busy(void) {
    setup();
    wr(R_CONDITION, 0x004Fu); /* 下位6bit = $0F（True）。bit6 は無視される */
    assert(rd(R_RESPONSE) == P_BUSY);
    assert(rd(R_RESPONSE) == P_COND_TRUE);
    assert(rd(R_RESPONSE) == P_COND_TRUE); /* primitive は読出しで消費されない */
    wr(R_CONDITION, 0x0000u);              /* False */
    assert(rd(R_RESPONSE) == P_BUSY);
    assert(rd(R_RESPONSE) == P_COND_FALSE);
    /* busy 後に FSAVE/FRESTORE/control が割り込むと $8900 は続かない */
    wr(R_CONDITION, 0x000Fu);
    wr(R_CONTROL, 0x0000u);
    assert(rd(R_RESPONSE) != P_BUSY);
    wr(R_CONDITION, 0x000Fu);
    (void)rd(R_SAVE);
    assert(rd(R_RESPONSE) != P_BUSY);
    assert(s_bus->fpu_cir_state.violation_count == 0);
    teardown();
    printf("fpu_cir_test: condition/busy OK\n");
}

/* ---- operand ストリーム ---- */

static void test_operand_input_widths(void) {
    setup();
    /* L: long write 1回（+$10/+$12 の2 word cycle） */
    assert(command(CMD(2, FMT_L, 2, OPM_FMOVE)) == P_EA_IN_L);
    wr32(R_OPERAND, 5u);
    assert(rd(R_RESPONSE) == P_IDLE);
    assert(regs().fp[2].sign_exp == 0x4001 &&
           regs().fp[2].signif == UINT64_C(0xA000000000000000));
    /* L: 二つの word を +$12/+$10 の任意の組で（同一 CIR） */
    assert(command(CMD(2, FMT_L, 3, OPM_FMOVE)) == P_EA_IN_L);
    wr(R_OPERAND_LO, 0x0000u);
    wr(R_OPERAND, 0x0003u);
    assert(rd(R_RESPONSE) == P_IDLE);
    assert(regs().fp[3].sign_exp == 0x4000 &&
           regs().fp[3].signif == UINT64_C(0xC000000000000000));
    /* W: 1 word（モデルの規約で bit31-16） */
    assert(command(CMD(2, FMT_W, 1, OPM_FMOVE)) == P_EA_IN_W);
    wr(R_OPERAND, 0x0100u);
    assert(s_bus->fpu_cir_state.remaining == 0); /* 1 word 転送で記述子が尽きる */
    assert(rd(R_RESPONSE) == P_IDLE);
    assert(regs().fp[1].sign_exp == 0x3FFF + 8 &&
           regs().fp[1].signif == UINT64_C(0x8000000000000000));
    /* B: 1 word の上位 byte */
    assert(command(CMD(2, FMT_B, 0, OPM_FMOVE)) == P_EA_IN_B);
    wr(R_OPERAND, 0xFD00u);
    assert(s_bus->fpu_cir_state.remaining == 0);
    assert(rd(R_RESPONSE) == P_IDLE);
    assert(regs().fp[0].sign_exp == 0xC000 &&
           regs().fp[0].signif == UINT64_C(0xC000000000000000));
    assert(s_bus->fpu_cir_state.violation_count == 0);
    teardown();
    printf("fpu_cir_test: operand input OK\n");
}

static void test_operand_output(void) {
    setup();
    load_single(0, 0x3FC00000u);                    /* 1.5 */
    assert(command(CMD(3, FMT_L, 0, 0)) == P_BUSY); /* 出力の最初の NULL(CA=1) */
    assert(rd(R_RESPONSE) == P_EA_OUT_L);
    assert(rd32(R_OPERAND) == 0x00000002u); /* 1.5 → 整数 2（丸め） */
    assert(rd(R_RESPONSE) == P_IDLE);
    /* D: 4 word を +$10/+$12 の任意の組で読む（API は2 word ごとに1回） */
    assert(command(CMD(3, FMT_D, 0, 0)) == P_BUSY);
    assert(rd(R_RESPONSE) == P_EA_OUT_D);
    assert(rd(R_OPERAND) == 0x3FF8u);
    assert(rd(R_OPERAND_LO) == 0x0000u);
    assert(rd(R_OPERAND_LO) == 0x0000u);
    assert(rd(R_OPERAND) == 0x0000u);
    assert(rd(R_RESPONSE) == P_IDLE);
    assert(s_bus->fpu_cir_state.violation_count == 0);
    teardown();
    printf("fpu_cir_test: operand output OK\n");
}

static void test_partial_discard(void) {
    setup();
    /* 上位 word の後に control（値は何でも ABORT）→ 半値は送られず、FP0 は不変 */
    static const uint16_t control_values[] = {0x0000u, 0x0001u, 0x0002u, 0xFFFFu};
    const mc68k_fpu_regs_t before = regs();
    for (unsigned i = 0; i < sizeof(control_values) / sizeof(control_values[0]); i++) {
        assert(command(CMD(2, FMT_L, 0, OPM_FMOVE)) == P_EA_IN_L);
        wr(R_OPERAND, 0x1234u);
        wr(R_CONTROL, control_values[i]);
        assert(rd(R_RESPONSE) == P_IDLE);
        assert(regs().fp[0].sign_exp == before.fp[0].sign_exp &&
               regs().fp[0].signif == before.fp[0].signif);
        assert(!s_bus->fpu_cir_state.wr_partial);
    }
    /* 後続 dialog は正常に完了する（部分状態が残っていない） */
    load_single(0, 0x3FC00000u);
    assert(regs().fp[0].sign_exp == 0x3FFF);
    /* 別 CIR（condition）への access は部分入力だけを捨てる */
    assert(command(CMD(2, FMT_L, 1, OPM_FMOVE)) == P_EA_IN_L);
    wr(R_OPERAND, 0xFFFFu);
    wr(R_CONDITION, 0x000Fu);
    assert(!s_bus->fpu_cir_state.wr_partial);
    /* dialog 中の condition write はモデルが protocol violation とし、adapter は数える */
    assert(s_bus->fpu_cir_state.violation_count == 1);
    assert(rd(R_RESPONSE) == P_BUSY); /* condition write の busy */
    assert(rd(R_RESPONSE) == P_TAKE_MID_PROTOCOL);
    teardown();
    printf("fpu_cir_test: partial discard OK\n");
}

/* control は転送記述子を破棄する（古い残り byte 数で1 word 単位と誤判定しない） */
static void test_control_clears_descriptor(void) {
    setup();
    assert(command(CMD(2, FMT_W, 0, OPM_FMOVE)) == P_EA_IN_W);
    assert(s_bus->fpu_cir_state.remaining == 2);
    wr(R_CONTROL, 0x0000u);
    assert(s_bus->fpu_cir_state.remaining == 0);
    wr(R_OPERAND, 0x1234u); /* 2 word 単位の上位 word として保持するだけで API は呼ばない */
    assert(s_bus->fpu_cir_state.wr_partial);
    assert(s_bus->fpu_cir_state.violation_count == 0);
    assert(rd(R_RESPONSE) == P_IDLE);
    teardown();
    printf("fpu_cir_test: control clears descriptor OK\n");
}

/* command write は API が false でも busy を立てる（無条件） */
static void test_command_busy_even_on_violation(void) {
    setup();
    assert(command(CMD(2, FMT_L, 0, OPM_FMOVE)) == P_EA_IN_L);
    wr(R_COMMAND, CMD(2, FMT_L, 1, OPM_FMOVE)); /* 転送中の command は不正 */
    assert(s_bus->fpu_cir_state.violation_count == 1);
    assert(rd(R_RESPONSE) == P_BUSY);
    assert(rd(R_RESPONSE) == P_TAKE_MID_PROTOCOL);
    teardown();
    printf("fpu_cir_test: command busy even on violation OK\n");
}

static void test_register_select_violation(void) {
    setup();
    wr(R_REGSEL, 0x0000u);
    assert(s_bus->fpu_cir_state.violation_count == 1);
    assert(rd(R_RESPONSE) == P_TAKE_MID_PROTOCOL);
    teardown();
    printf("fpu_cir_test: register select violation OK\n");
}

static void test_instruction_address(void) {
    setup();
    wr32(R_IADDR, 0x00123456u);
    assert(regs().fpiar == 0x00123456u);
    wr(R_IADDR_LO, 0x00AAu); /* +$1A も同じ CIR: 上位→下位の順 */
    wr(R_IADDR, 0xBBCCu);
    assert(regs().fpiar == 0x00AABBCCu);
    /* 上位 word の後に別 CIR へ access すると半値は送らない */
    wr(R_IADDR, 0x1111u);
    wr(R_CONDITION, 0x000Fu);
    wr(R_IADDR, 0x2222u);
    assert(regs().fpiar == 0x00AABBCCu);
    assert(s_bus->fpu_cir_state.violation_count == 0);
    teardown();
    printf("fpu_cir_test: instruction address OK\n");
}

/* ---- save / restore ---- */

/* FSAVE: format word と body（operand CIR の word 列）を読む。返り値は body の word 数 */
static unsigned fsave(uint16_t *format, uint16_t *body, unsigned max_words) {
    *format = rd(R_SAVE);
    unsigned words = 0;
    uint32_t bytes = s_bus->fpu_cir_state.remaining;
    while (bytes > 0 && words < max_words) {
        body[words++] = rd(R_OPERAND);
        bytes = bytes > 2u ? bytes - 2u : 0;
    }
    return words;
}

static void test_restore_null_and_readback(void) {
    setup();
    load_single(0, 0x3FC00000u);
    assert(regs().fp[0].sign_exp == 0x3FFF);
    wr(R_RESTORE, 0x0000u); /* null frame: reset */
    assert(rd(R_RESTORE) == 0x0000u);
    assert(regs().fp[0].sign_exp != 0x3FFF); /* reset 後の FP0 は 1.5 でない */
    teardown();
    printf("fpu_cir_test: null restore OK\n");
}

static void test_rejected_save_and_response_keep_position(void) {
    uint16_t format_a, format_b;
    uint16_t body_a[64], body_b[64];
    setup();
    load_single(0, 0x3FC00000u);
    unsigned n = fsave(&format_a, body_a, 64);
    assert(n == 12 && format_a == 0x1F18u); /* 881 idle: 24 byte */
    /* restore body の最初の word の後に拒否される save と response read を挿入する */
    wr(R_RESTORE, format_a);
    wr(R_OPERAND, body_a[0]);
    assert(rd(R_SAVE) == 0x0200u); /* 入れ子 save は illegal format */
    (void)rd(R_RESPONSE);
    for (unsigned i = 1; i < n; i++)
        wr(R_OPERAND, body_a[i]);
    assert(s_bus->fpu_cir_state.violation_count == 0);
    assert(rd(R_RESPONSE) == P_IDLE);
    unsigned m = fsave(&format_b, body_b, 64);
    assert(m == n && format_b == format_a && memcmp(body_a, body_b, n * sizeof(uint16_t)) == 0);
    teardown();
    printf("fpu_cir_test: rejected save/response keep position OK\n");
}

static void test_valid_save_switches_stream(void) {
    setup();
    mc68k_fpu_t *ref = mc68k_fpu_create(MC68K_FPU_68881);
    assert(ref);
    /* operand の上位 word だけ送った状態（部分入力）で有効な save を読む */
    assert(command(CMD(2, FMT_L, 0, OPM_FMOVE)) == P_EA_IN_L);
    assert(mc68k_fpu_write_command_word(ref, CMD(2, FMT_L, 0, OPM_FMOVE)));
    assert(mc68k_fpu_read_response(ref).kind == MC68K_FPU_RESPONSE_EA_TRANSFER);
    wr(R_OPERAND, 0x1234u);
    uint16_t fmt = 0, ref_fmt = 0;
    size_t ref_body = 0;
    fmt = rd(R_SAVE);
    mc68k_fpu_begin_save(ref, &ref_fmt, &ref_body);
    assert(ref_body > 0 && fmt == ref_fmt); /* busy frame */
    assert(s_bus->fpu_cir_state.remaining == ref_body);
    assert(!s_bus->fpu_cir_state.wr_partial);
    for (unsigned i = 0; i < 4; i++) { /* busy body の先頭 4 longword */
        uint32_t v = 0;
        assert(mc68k_fpu_read_operand(ref, &v));
        assert(rd(R_OPERAND) == (uint16_t)(v >> 16));
        assert(rd(R_OPERAND) == (uint16_t)v);
    }
    mc68k_fpu_destroy(ref);
    teardown();
    printf("fpu_cir_test: valid save switches stream OK\n");
}

/* ---- FMOVEM（register select・MP_REGISTER・FP_REGISTERS） ---- */

#define MOVEM_IN(mode, low)  ((uint16_t)((6 << 13) | ((mode) << 11) | (low)))
#define MOVEM_OUT(mode, low) ((uint16_t)((7 << 13) | ((mode) << 11) | (low)))
#define MODE_POST_STATIC     2
#define MODE_POST_DYNAMIC    3
#define P_FP_REGS_IN         0x810Cu
#define P_FP_REGS_OUT        0xA10Cu
#define P_MP_REG_D3          0x8C03u
#define REGISTER_LONGS       3u /* 拡張精度 1 レジスタ = 12 byte = 3 longword */

static void test_movem(void) {
    static const uint32_t in[6] = {0x3FFF0000u, 0x80000000u, 0x00010002u,
                                   0x40000000u, 0x80000000u, 0x12345678u};
    setup();
    /* 静的 list（FP0, FP1）の入力。register select は mask を上位 byte で返す */
    assert(command(MOVEM_IN(MODE_POST_STATIC, 0xC0)) == P_FP_REGS_IN);
    assert(rd(R_REGSEL) == 0xC000u);
    for (unsigned i = 0; i < 6; i++)
        wr32(R_OPERAND, in[i]);
    assert(rd(R_RESPONSE) == P_IDLE);
    assert(regs().fp[0].sign_exp == 0x3FFF && regs().fp[1].sign_exp == 0x4000);
    /* 同じ list の出力 */
    assert(command(MOVEM_OUT(MODE_POST_STATIC, 0xC0)) == P_FP_REGS_OUT);
    assert(rd(R_REGSEL) == 0xC000u);
    for (unsigned i = 0; i < 2 * REGISTER_LONGS; i++)
        assert(rd32(R_OPERAND) == in[i]);
    assert(rd(R_RESPONSE) == P_IDLE);
    /* 動的 list（D3）: MP_REGISTER で Dn の値を operand CIR へ書き、FP_REGISTERS が続く */
    assert(command(MOVEM_OUT(MODE_POST_DYNAMIC, 3 << 4)) == P_MP_REG_D3);
    wr32(R_OPERAND, 0x40u); /* FP1 だけ */
    assert(rd(R_RESPONSE) == P_FP_REGS_OUT);
    assert(rd(R_REGSEL) == 0x4000u);
    for (unsigned i = 0; i < REGISTER_LONGS; i++)
        assert(rd32(R_OPERAND) == in[REGISTER_LONGS + i]);
    assert(rd(R_RESPONSE) == P_IDLE);
    /* list 0: 転送なし。register select の読出しで完了する */
    assert(command(MOVEM_OUT(MODE_POST_STATIC, 0x00)) == P_FP_REGS_OUT);
    assert(rd(R_REGSEL) == 0x0000u);
    assert(rd(R_RESPONSE) == P_IDLE);
    assert(s_bus->fpu_cir_state.violation_count == 0);
    teardown();
    printf("fpu_cir_test: fmovem OK\n");
}

/* ---- W/B の出力、API false の全1、方向切替、+$12 起点の long ---- */

static void test_small_output_widths(void) {
    setup();
    load_single(0, 0x41700000u); /* 15.0 */
    assert(command(CMD(3, FMT_B, 0, 0)) == P_BUSY);
    assert(rd(R_RESPONSE) == 0xB101u);
    assert(rd(R_OPERAND) == 0x0F00u); /* B は word の上位 byte */
    assert(rd(R_RESPONSE) == P_IDLE);
    assert(command(CMD(3, FMT_W, 0, 0)) == P_BUSY);
    assert(rd(R_RESPONSE) == 0xB102u);
    assert(rd(R_OPERAND_LO) == 0x000Fu); /* +$12 でも同じ CIR */
    assert(rd(R_RESPONSE) == P_IDLE);
    assert(s_bus->fpu_cir_state.violation_count == 0);
    teardown();
    printf("fpu_cir_test: B/W output OK\n");
}

static void test_api_false_reads_all_ones(void) {
    setup();
    assert(rd(R_OPERAND) == 0xFFFFu); /* 転送 phase でない operand read */
    assert(s_bus->fpu_cir_state.violation_count == 1);
    assert(rd32(R_OPERAND) == 0xFFFFFFFFu);
    assert(s_bus->fpu_cir_state.violation_count == 3);
    assert(rd(R_REGSEL) == 0xFFFFu); /* register select も phase 外 */
    assert(s_bus->fpu_cir_state.violation_count == 4);
    assert(rd(R_RESPONSE) == P_TAKE_MID_PROTOCOL); /* bus error でなく response で通知 */
    teardown();
    printf("fpu_cir_test: API false reads all ones OK\n");
}

static void test_direction_switch(void) {
    setup();
    const mc68k_fpu_regs_t before = regs();
    /* 書込みの上位 word の後に read: 部分入力を捨て、半値は送られない */
    assert(command(CMD(2, FMT_L, 0, OPM_FMOVE)) == P_EA_IN_L);
    wr(R_OPERAND, 0x1234u);
    assert(rd(R_OPERAND) == 0xFFFFu);
    assert(!s_bus->fpu_cir_state.wr_partial);
    assert(s_bus->fpu_cir_state.violation_count == 1);
    assert(regs().fp[0].signif == before.fp[0].signif);
    teardown();

    /* 読出しの上位 word の後に write: ラッチした下位 word を捨てる */
    setup();
    load_single(0, 0x3FC00000u);
    assert(command(CMD(3, FMT_D, 0, 0)) == P_BUSY);
    assert(rd(R_RESPONSE) == P_EA_OUT_D);
    assert(rd(R_OPERAND) == 0x3FF8u);
    assert(s_bus->fpu_cir_state.rd_latch);
    wr(R_OPERAND, 0x5555u);
    assert(!s_bus->fpu_cir_state.rd_latch);
    /* 2 word 単位の書込みの上位 word は保持されるだけで、API はまだ呼ばれない */
    assert(s_bus->fpu_cir_state.wr_partial && s_bus->fpu_cir_state.violation_count == 0);
    teardown();
    printf("fpu_cir_test: direction switch OK\n");
}

static void test_long_from_lo_offset(void) {
    setup();
    const mc68k_fpu_regs_t before = regs();
    /* +$12 の long は 2 cycle 目が +$14（register select）。bus error にならず、
     * 1 cycle 目の operand 上位 word は 2 cycle 目の別 CIR access で捨てられる */
    assert(command(CMD(2, FMT_L, 0, OPM_FMOVE)) == P_EA_IN_L);
    wr32(R_OPERAND_LO, 0x00000005u);
    assert(!s_bus->fpu_cir_state.wr_partial);
    assert(s_bus->fpu_cir_state.violation_count == 1); /* register select write */
    assert(regs().fp[0].signif == before.fp[0].signif);
    teardown();

    setup();
    load_single(0, 0x3FC00000u);
    assert(command(CMD(3, FMT_D, 0, 0)) == P_BUSY);
    assert(rd(R_RESPONSE) == P_EA_OUT_D);
    assert(rd32(R_OPERAND_LO) ==
           0x3FF8FFFFu); /* operand の上位 word と、phase 外の register select */
    assert(!s_bus->fpu_cir_state.rd_latch);
    assert(s_bus->fpu_cir_state.violation_count == 1);
    teardown();
    printf("fpu_cir_test: long from +$12 OK\n");
}

/* ---- 部分入力の破棄と転送記述子（Opus B1/B2） ---- */

#define EXP_65541  0x400Fu
#define SIG_65541  UINT64_C(0x8002800000000000)
#define P_EA_OUT_X 0xB20Cu

static void check_fp0_is_65541(void) {
    assert(regs().fp[0].sign_exp == EXP_65541 && regs().fp[0].signif == SIG_65541);
}

/* 上位 word を捨てた後に long を書き直すと、全値がモデルへ届く（半値・ずれた記述子なし） */
static void test_discard_restores_descriptor(void) {
    static const struct {
        uint32_t off;
        uint16_t value;
    } discards[] = {{0x08u, 0x0000u}, {R_IADDR, 0x1111u}};
    for (unsigned i = 0; i < sizeof(discards) / sizeof(discards[0]); i++) {
        setup();
        assert(command(CMD(2, FMT_L, 0, OPM_FMOVE)) == P_EA_IN_L);
        wr(R_OPERAND, 0x0001u);
        wr(discards[i].off, discards[i].value);
        wr32(R_OPERAND, 0x00010005u);
        assert(rd(R_RESPONSE) == P_IDLE);
        check_fp0_is_65541();
        assert(s_bus->fpu_cir_state.violation_count == 0);
        teardown();
    }
    /* 読出し側: ラッチした下位 word を捨てた後は次の longword から読む */
    setup();
    assert(command(CMD(2, FMT_D, 0, OPM_FMOVE)) == 0x9608u);
    wr32(R_OPERAND, 0x3FF80000u);
    wr32(R_OPERAND, 0x12345678u);
    assert(rd(R_RESPONSE) == P_IDLE);
    assert(command(CMD(3, FMT_D, 0, 0)) == P_BUSY);
    assert(rd(R_RESPONSE) == P_EA_OUT_D);
    assert(rd(R_OPERAND) == 0x3FF8u);
    wr(0x08u, 0x0000u);               /* 予約位置への write でラッチを捨てる */
    assert(rd(R_OPERAND) == 0x1234u); /* モデルの 2 longword 目 */
    assert(rd(R_OPERAND) == 0x5678u);
    assert(s_bus->fpu_cir_state.remaining == 0); /* 読み飛ばした下位 word の分も進んでいる */
    assert(rd(R_RESPONSE) == P_IDLE);
    assert(s_bus->fpu_cir_state.violation_count == 0);
    teardown();
    printf("fpu_cir_test: discard restores descriptor OK\n");
}

/* 記述子が尽きている（MP_REGISTER は length を持たない）状態で上位 word を捨てても、
 * 書き直しは2 word 単位のまま（戻す byte 数は実際に引いた分だけ） */
static void test_discard_with_empty_descriptor(void) {
    static const uint32_t fp1_words[3] = {0x40000000u, 0x80000000u, 0x00000000u}; /* 2.0 */
    setup();
    load_single(1, 0x40000000u);
    assert(command(MOVEM_OUT(MODE_POST_DYNAMIC, 3 << 4)) == P_MP_REG_D3);
    assert(s_bus->fpu_cir_state.remaining == 0);
    wr(R_OPERAND, 0x0000u); /* Dn の上位 word */
    assert(s_bus->fpu_cir_state.wr_partial);
    wr(0x08u, 0x0000u); /* 予約位置 write で破棄 */
    assert(s_bus->fpu_cir_state.remaining == 0);
    wr32(R_OPERAND, 0x40u); /* 書き直し: FP1 だけ */
    assert(rd(R_RESPONSE) == P_FP_REGS_OUT);
    assert(rd(R_REGSEL) == 0x4000u);
    for (unsigned i = 0; i < REGISTER_LONGS; i++)
        assert(rd32(R_OPERAND) == fp1_words[i]);
    assert(rd(R_RESPONSE) == P_IDLE);
    assert(s_bus->fpu_cir_state.violation_count == 0);
    teardown();
    printf("fpu_cir_test: discard with empty descriptor OK\n");
}

/* 転送 response が新しい記述子を設定するとき、保持中の operand の部分入力は捨てる */
static void test_new_descriptor_drops_partials(void) {
    setup();
    /* 旧転送の途中状態（上位 word 保持・記述子に2 byte 残り）を作る */
    s_bus->fpu_cir_state.wr_partial = true;
    s_bus->fpu_cir_state.wr_hi = 0x1111u;
    s_bus->fpu_cir_state.wr_consumed = 2;
    s_bus->fpu_cir_state.remaining = 2;
    assert(mc68k_fpu_write_command_word(s_bus->fpu_cir, CMD(2, FMT_D, 0, OPM_FMOVE)));
    assert(rd(R_RESPONSE) == 0x9608u); /* adapter の busy_once は立っていない */
    assert(!s_bus->fpu_cir_state.wr_partial);
    assert(s_bus->fpu_cir_state.remaining == 8); /* 旧転送の戻し分が加算されない */
    teardown();

    /* MP_REGISTER response でも同じ */
    setup();
    s_bus->fpu_cir_state.wr_partial = true;
    s_bus->fpu_cir_state.wr_hi = 0x1111u;
    s_bus->fpu_cir_state.wr_consumed = 2;
    s_bus->fpu_cir_state.remaining = 2;
    assert(mc68k_fpu_write_command_word(s_bus->fpu_cir, MOVEM_OUT(MODE_POST_DYNAMIC, 3 << 4)));
    assert(rd(R_RESPONSE) == P_MP_REG_D3);
    assert(!s_bus->fpu_cir_state.wr_partial);
    assert(s_bus->fpu_cir_state.remaining == 0);
    teardown();
    printf("fpu_cir_test: new descriptor drops partials OK\n");
}

/* 実ホスト経路（hook 経由の CIR 列）: 転送 response が来る前に書かれた operand 上位 word は、
 * 新しい転送の記述子設定で捨てられ、その後の転送は正しく完了する */
static void test_new_descriptor_drops_partials_host_path(void) {
    /* command → operand 上位 word → busy → EA response（D） */
    setup();
    wr(R_COMMAND, CMD(2, FMT_D, 0, OPM_FMOVE));
    wr(R_OPERAND, 0x1111u);
    assert(s_bus->fpu_cir_state.wr_partial);
    assert(rd(R_RESPONSE) == P_BUSY);
    assert(s_bus->fpu_cir_state.wr_partial); /* busy は部分入力を保持する */
    assert(rd(R_RESPONSE) == 0x9608u);
    assert(!s_bus->fpu_cir_state.wr_partial);
    assert(s_bus->fpu_cir_state.remaining == 8);
    wr32(R_OPERAND, 0x3FF80000u);
    wr32(R_OPERAND, 0x00000000u);
    assert(rd(R_RESPONSE) == P_IDLE);
    assert(regs().fp[0].sign_exp == 0x3FFF &&
           regs().fp[0].signif == UINT64_C(0xC000000000000000));
    assert(s_bus->fpu_cir_state.violation_count == 0);
    teardown();

    /* FMOVEM 動的 list: Dn の後に余分な上位 word → FP_REGISTERS response */
    setup();
    load_single(1, 0x40000000u);
    assert(command(MOVEM_OUT(MODE_POST_DYNAMIC, 3 << 4)) == P_MP_REG_D3);
    wr32(R_OPERAND, 0x40u);
    wr(R_OPERAND, 0x2222u);
    assert(s_bus->fpu_cir_state.wr_partial);
    assert(rd(R_RESPONSE) == P_FP_REGS_OUT);
    assert(!s_bus->fpu_cir_state.wr_partial);
    assert(s_bus->fpu_cir_state.remaining == 12);
    assert(rd(R_REGSEL) == 0x4000u);
    assert(rd32(R_OPERAND) == 0x40000000u);
    assert(rd32(R_OPERAND) == 0x80000000u);
    assert(rd32(R_OPERAND) == 0x00000000u);
    assert(rd(R_RESPONSE) == P_IDLE);
    assert(s_bus->fpu_cir_state.violation_count == 0);
    teardown();
    printf("fpu_cir_test: new descriptor drops partials (host path) OK\n");
}

/* busy の response read は部分入力を保持する（busy は adapter だけの表示） */
static void test_busy_keeps_partial(void) {
    setup();
    wr(R_COMMAND, CMD(2, FMT_L, 0, OPM_FMOVE));
    wr(R_OPERAND, 0x1234u);
    assert(rd(R_RESPONSE) == P_BUSY);
    assert(s_bus->fpu_cir_state.wr_partial);
    /* 保持した上位 word と組み立てて API へ（response 前なので violation） */
    wr(R_OPERAND_LO, 0x5678u);
    assert(!s_bus->fpu_cir_state.wr_partial);
    assert(s_bus->fpu_cir_state.violation_count == 1);
    assert(rd(R_RESPONSE) == P_TAKE_MID_PROTOCOL);
    teardown();

    /* instruction-address の上位 word も busy で捨てない */
    setup();
    wr(R_COMMAND, CMD(2, FMT_L, 0, OPM_FMOVE));
    wr(R_IADDR, 0x0001u);
    assert(rd(R_RESPONSE) == P_BUSY);
    assert(s_bus->fpu_cir_state.iaddr_partial);
    wr(R_IADDR_LO, 0x2345u);
    assert(regs().fpiar == 0x00012345u);
    teardown();
    printf("fpu_cir_test: busy keeps partial OK\n");
}

/* 転送 response は instruction-address の上位 word を捨てない（捨てるのは operand
 * の部分入力だけ） */
static void test_transfer_response_keeps_iaddr_partial(void) {
    setup();
    wr(R_COMMAND, CMD(2, FMT_L, 0, OPM_FMOVE));
    assert(rd(R_RESPONSE) == P_BUSY);
    wr(R_IADDR, 0x0001u);
    assert(rd(R_RESPONSE) == P_EA_IN_L);
    assert(s_bus->fpu_cir_state.iaddr_partial);
    wr(R_IADDR_LO, 0x2345u);
    assert(regs().fpiar == 0x00012345u);
    teardown();

    /* MP_REGISTER response でも同じ */
    setup();
    wr(R_COMMAND, MOVEM_OUT(MODE_POST_DYNAMIC, 3 << 4));
    assert(rd(R_RESPONSE) == P_BUSY);
    wr(R_IADDR, 0x0003u);
    assert(rd(R_RESPONSE) == P_MP_REG_D3);
    assert(s_bus->fpu_cir_state.iaddr_partial);
    wr(R_IADDR_LO, 0x4567u);
    assert(regs().fpiar == 0x00034567u);
    teardown();
    printf("fpu_cir_test: transfer response keeps iaddr partial OK\n");
}

static void test_extended_operand(void) {
    setup();
    assert(command(CMD(2, FMT_X, 5, OPM_FMOVE)) == 0x960Cu);
    wr32(R_OPERAND, 0xC0000000u);
    wr32(R_OPERAND, 0xA0000000u);
    wr32(R_OPERAND, 0x0000AAAAu);
    assert(rd(R_RESPONSE) == P_IDLE);
    assert(regs().fp[5].sign_exp == 0xC000 &&
           regs().fp[5].signif == UINT64_C(0xA00000000000AAAA));
    assert(command(CMD(3, FMT_X, 5, 0)) == P_BUSY);
    assert(rd(R_RESPONSE) == P_EA_OUT_X);
    assert(rd32(R_OPERAND) == 0xC0000000u);
    assert(rd32(R_OPERAND) == 0xA0000000u);
    assert(rd32(R_OPERAND) == 0x0000AAAAu); /* 仮数の下位 longword */
    assert(rd(R_RESPONSE) == P_IDLE);
    assert(s_bus->fpu_cir_state.violation_count == 0);
    teardown();
    printf("fpu_cir_test: extended operand OK\n");
}

/* 別 CIR への access は、operand の部分入力・ラッチ・IA の上位 word をすべて捨てる */
static void arm_write_partial(void) {
    assert(command(CMD(2, FMT_L, 0, OPM_FMOVE)) == P_EA_IN_L);
    wr(R_OPERAND, 0x1234u);
    assert(s_bus->fpu_cir_state.wr_partial);
}

static void arm_read_latch(void) {
    assert(command(CMD(2, FMT_D, 0, OPM_FMOVE)) == 0x9608u);
    wr32(R_OPERAND, 0x3FF80000u);
    wr32(R_OPERAND, 0x12345678u);
    assert(rd(R_RESPONSE) == P_IDLE);
    assert(command(CMD(3, FMT_D, 0, 0)) == P_BUSY);
    assert(rd(R_RESPONSE) == P_EA_OUT_D);
    (void)rd(R_OPERAND);
    assert(s_bus->fpu_cir_state.rd_latch);
}

static void arm_iaddr_partial(void) {
    wr(R_IADDR, 0x1111u);
    assert(s_bus->fpu_cir_state.iaddr_partial);
}

static void test_other_cir_discards_partials(void) {
    static const struct {
        bool is_write;
        uint32_t off;
    } actions[] = {{true, R_COMMAND},  {true, R_CONDITION}, {false, R_RESTORE},
                   {false, 0x08u},     {true, 0x0Cu},       {false, R_REGSEL},
                   {false, R_COMMAND}, {true, R_RESPONSE}};
    static void (*const arms[])(void) = {arm_write_partial, arm_read_latch, arm_iaddr_partial};
    for (unsigned a = 0; a < sizeof(actions) / sizeof(actions[0]); a++) {
        for (unsigned m = 0; m < sizeof(arms) / sizeof(arms[0]); m++) {
            setup();
            arms[m]();
            if (actions[a].is_write)
                wr(actions[a].off,
                   actions[a].off == R_COMMAND ? CMD(2, FMT_L, 0, OPM_FMOVE) : 0x000Fu);
            else
                (void)rd(actions[a].off);
            assert(!s_bus->fpu_cir_state.wr_partial);
            assert(!s_bus->fpu_cir_state.rd_latch);
            assert(!s_bus->fpu_cir_state.iaddr_partial);
            teardown();
        }
    }
    printf("fpu_cir_test: other CIR discards partials OK\n");
}

/* operand と instruction address は互いの上位 word を捨てる */
static void test_operand_and_iaddr_discard_each_other(void) {
    setup();
    wr(R_IADDR, 0x1111u);
    (void)rd(R_OPERAND); /* phase 外の read（violation）でも IA の上位 word は捨てる */
    wr(R_IADDR, 0x2222u);
    assert(regs().fpiar == 0);
    teardown();

    setup();
    assert(command(CMD(2, FMT_L, 0, OPM_FMOVE)) == P_EA_IN_L);
    wr(R_IADDR, 0x1111u);
    wr(R_OPERAND, 0x0001u); /* IA の上位 word を捨て、operand の上位 word を保持 */
    wr(R_IADDR, 0x2222u);   /* operand の上位 word を捨て、IA の上位 word を保持 */
    assert(regs().fpiar == 0);
    wr32(R_OPERAND, 0x00010005u); /* operand 側は新規の long として全値が届く */
    assert(rd(R_RESPONSE) == P_IDLE);
    check_fp0_is_65541();
    teardown();
    printf("fpu_cir_test: operand/iaddr discard each other OK\n");
}

/* ---- restore / control / NULL response の副作用 ---- */

static void test_restore_side_effects(void) {
    setup();
    wr(R_CONDITION, 0x000Fu);
    wr(R_RESTORE, 0x0000u);
    assert(rd(R_RESPONSE) != P_BUSY); /* FRESTORE が割り込むと $8900 は続かない */
    teardown();

    setup();
    assert(command(CMD(2, FMT_L, 0, OPM_FMOVE)) == P_EA_IN_L);
    wr(R_OPERAND, 0x0001u);
    wr(R_RESTORE, 0x0000u); /* null restore: 部分入力を捨てる */
    assert(!s_bus->fpu_cir_state.wr_partial);
    assert(command(CMD(2, FMT_L, 0, OPM_FMOVE)) == P_EA_IN_L);
    wr32(R_OPERAND, 0x00000005u);
    assert(rd(R_RESPONSE) == P_IDLE);
    assert(regs().fp[0].sign_exp == 0x4001 &&
           regs().fp[0].signif == UINT64_C(0xA000000000000000));
    assert(s_bus->fpu_cir_state.violation_count == 0);
    teardown();

    /* readback は書込み値でなく begin_restore() の戻り（不正 format は $02xx） */
    setup();
    wr(R_RESTORE, 0x1234u);
    assert(rd(R_RESTORE) == 0x0234u);
    teardown();
    printf("fpu_cir_test: restore side effects OK\n");
}

static void test_control_keeps_registers(void) {
    static const uint16_t values[] = {0x0000u, 0x0001u, 0x0002u, 0x0003u, 0xFFFFu};
    setup();
    load_single(0, 0x3FC00000u);
    wr32(R_IADDR, 0x00123456u);
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        wr(R_CONTROL, values[i]);
        assert(regs().fp[0].sign_exp == 0x3FFF); /* hardware reset ではない */
        assert(regs().fpiar == 0x00123456u);
    }
    teardown();
    printf("fpu_cir_test: control keeps registers OK\n");
}

static void test_null_response_keeps_descriptor(void) {
    setup();
    assert(command(CMD(2, FMT_W, 1, OPM_FMOVE)) == P_EA_IN_W);
    (void)rd(R_RESPONSE); /* 転送中の response read は記述子を更新しない */
    wr(R_OPERAND, 0x0100u);
    assert(rd(R_RESPONSE) == P_IDLE);
    assert(regs().fp[1].sign_exp == 0x3FFF + 8);
    teardown();
    printf("fpu_cir_test: response keeps descriptor OK\n");
}

/* ---- byte / 奇数アクセスは API を呼ばず状態を変えない ---- */

static void test_byte_and_odd_access_leave_state(void) {
    setup();
    uint16_t w = 0;
    uint32_t l = 0;
    uint8_t b = 0;
    arm_write_partial(); /* byte・奇数アクセスは部分入力にも触れない */
    s_bus->fpu_cir_state.busy_once = true;
    assert(s_h.read_byte(s_h.ctx, BASE + R_RESPONSE, &b) && b == 0xFFu);
    assert(s_h.read_byte(s_h.ctx, BASE + R_RESPONSE + 1u, &b) && b == 0xFFu);
    assert(s_h.read_word(s_h.ctx, BASE + R_RESPONSE + 1u, &w) && w == 0xFFFFu);
    assert(s_h.read_long(s_h.ctx, BASE + R_RESPONSE + 1u, &l) && l == 0xFFFFFFFFu);
    assert(s_h.write_byte(s_h.ctx, BASE + R_COMMAND, 0x12u));
    assert(s_h.write_word(s_h.ctx, BASE + R_COMMAND + 1u, CMD(2, FMT_L, 0, OPM_FMOVE)));
    assert(s_h.write_long(s_h.ctx, BASE + R_OPERAND + 1u, 0x11112222u));
    assert(s_bus->fpu_cir_state.busy_once);
    assert(s_bus->fpu_cir_state.wr_partial);
    assert(s_bus->fpu_cir_state.violation_count == 0);
    /* backdoor も同様 */
    assert(x68k_bus_read8(s_bus, BASE + R_RESPONSE) == 0xFFu);
    x68k_bus_write8(s_bus, BASE + R_COMMAND, 0x12u);
    assert(s_bus->fpu_cir_state.busy_once);
    teardown();
    printf("fpu_cir_test: byte/odd access leaves state OK\n");
}

/* ---- ユーザモード保護: バスエラー cycle ではデバイスを呼ばない（Opus N1） ---- */

static void test_user_mode_does_not_touch_device(void) {
    setup();
    mc68k_bus_t h = s_h;
    uint32_t pc = 0x1000u;
    x68k_bus_write8(s_bus, 0x000000u, 0x00u);
    x68k_bus_write8(s_bus, 0x000001u, 0x00u);
    x68k_bus_write8(s_bus, 0x000002u, 0x20u); /* SSP=$2000 */
    x68k_bus_write8(s_bus, 0x000003u, 0x00u);
    x68k_bus_write8(s_bus, 0x000004u, 0x00u);
    x68k_bus_write8(s_bus, 0x000005u, 0x00u);
    x68k_bus_write8(s_bus, 0x000006u, (uint8_t)(pc >> 8));
    x68k_bus_write8(s_bus, 0x000007u, (uint8_t)pc);
    x68k_bus_write8(s_bus, pc, 0x46u); /* MOVE #$0000,SR（ユーザモードへ） */
    x68k_bus_write8(s_bus, pc + 1u, 0xFCu);
    x68k_bus_write8(s_bus, pc + 2u, 0x00u);
    x68k_bus_write8(s_bus, pc + 3u, 0x00u);
    mc68k_state_t *cpu = mc68k_create(&h);
    assert(cpu);
    x68k_bus_set_cpu(s_bus, cpu);
    mc68k_reset(cpu);
    mc68k_execute(cpu, 1);
    assert(!mc68k_is_supervisor(cpu));
    x68k_bus_set_super_protect(s_bus, 1);

    s_bus->fpu_cir_state.busy_once = true;
    uint16_t w = 0;
    assert(!h.read_word(h.ctx, BASE + R_RESPONSE, &w) && w == 0xFFFFu);
    assert(s_bus->fpu_cir_state.busy_once); /* 消費されていない */
    assert(!h.write_word(h.ctx, BASE + R_COMMAND, CMD(2, FMT_L, 0, OPM_FMOVE)));
    uint32_t l = 0;
    assert(!h.read_long(h.ctx, BASE + R_OPERAND, &l));
    assert(!h.write_long(h.ctx, BASE + R_IADDR, 0x1u));
    assert(s_bus->fpu_cir_state.busy_once && s_bus->fpu_cir_state.violation_count == 0);
    assert(regs().fpiar == 0);
    mc68k_destroy(cpu);
    teardown();
    printf("fpu_cir_test: user mode no device access OK\n");
}

int main(void) {
    test_encoder();
    test_open_and_ignored_positions();
    test_condition_and_busy();
    test_operand_input_widths();
    test_operand_output();
    test_partial_discard();
    test_control_clears_descriptor();
    test_command_busy_even_on_violation();
    test_register_select_violation();
    test_instruction_address();
    test_restore_null_and_readback();
    test_rejected_save_and_response_keep_position();
    test_valid_save_switches_stream();
    test_movem();
    test_small_output_widths();
    test_api_false_reads_all_ones();
    test_direction_switch();
    test_long_from_lo_offset();
    test_discard_restores_descriptor();
    test_discard_with_empty_descriptor();
    test_new_descriptor_drops_partials();
    test_new_descriptor_drops_partials_host_path();
    test_busy_keeps_partial();
    test_transfer_response_keeps_iaddr_partial();
    test_extended_operand();
    test_other_cir_discards_partials();
    test_operand_and_iaddr_discard_each_other();
    test_restore_side_effects();
    test_control_keeps_registers();
    test_null_response_keeps_descriptor();
    test_byte_and_odd_access_leave_state();
    test_user_mode_does_not_touch_device();
    printf("fpu_cir_test: ALL OK\n");
    return 0;
}
