/*
 * FLOAT3.X（X68000 純正の MC68881 ドライバ）の CIR 命令列 replay テスト。
 *
 * FLOAT3.X の静的逆アセンブル（FLOAT3.S）から、各ルーチンが標準番地 $E9E000 に対して行う
 * バスアクセスの列を手で書き起こし、CPU バスのフック（x68k_bus_get_hooks()）へ流す。
 * 各 step は 1 命令に対応し、ルーチン名（L00xxxx）は FLOAT3.S のラベル。ドライバのバイナリ
 * （arc/ はリポジトリ管理外）には依存しない。期待値（IEEE 754 の bit 列、FPSR の AEXC bit）は
 * ドライバ・production から取らず、手で置く。
 *
 *   a5 = $E9E010 を基準に、response = (-$10,a5) = +$00、command = (-$06,a5) = +$0A、
 *   operand = (a5) = +$10。
 *   cmpi.w #$8900,(-$10,a5) + beq : response を読み、$8900 の間繰り返す（POLL）
 *   tst.w (-$10,a5)               : response を1回読み捨てる（TST）
 *   move.l dN,(a5) / move.l (a5),dN : operand への long 書込み／読出し（WRL/RDL）
 *   move.l (a6),(a5) 等のメモリ operand は d0-d3 に置いた値で代表する
 *     （d0/d1 = (a6)/(4,a6)、d2/d3 = (8,a6)/(12,a6)）。
 *
 * 追加で、FPU 検出の bus error 経路（L00451c）と 2 つの reset
 * 経路（L00451c/L001698）を検査する。
 */
#include "bus/bus_internal.h"
#include "bus/x68k_bus.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BASE         0xE9E000u
#define OFF_RESPONSE 0x00u
#define OFF_RESTORE  0x06u
#define OFF_COMMAND  0x0Au
#define OFF_OPERAND  0x10u
#define BUSY         0x8900u
#define MAX_POLL     64u /* busy が続く上限。無限 busy（応答が進まない変異）を検出する */
#define DREG_COUNT   8u

typedef enum { OP_CMD, OP_POLL, OP_WRL, OP_WRL_IMM, OP_TST, OP_RDL, OP_RESET } op_kind_t;
typedef struct {
    op_kind_t kind;
    uint32_t arg;
} step_t;

static x68k_bus_t *s_bus;
static mc68k_bus_t s_h;
static uint32_t s_d[DREG_COUNT];

static void setup(bool fpu) {
    s_bus = x68k_bus_create();
    assert(s_bus);
    if (fpu)
        assert(x68k_bus_set_fpu_cir(s_bus, 1, X68K_FPU_CIR_PRIMARY));
    s_h = x68k_bus_get_hooks(s_bus);
    memset(s_d, 0, sizeof(s_d));
}

static void teardown(void) {
    x68k_bus_destroy(s_bus);
}

static uint16_t rd16(uint32_t off) {
    uint16_t v = 0;
    assert(s_h.read_word(s_h.ctx, BASE + off, &v));
    return v;
}

static void run(const step_t *steps, size_t count) {
    for (size_t i = 0; i < count; i++) {
        switch (steps[i].kind) {
        case OP_CMD:
            assert(s_h.write_word(s_h.ctx, BASE + OFF_COMMAND, (uint16_t)steps[i].arg));
            break;
        case OP_POLL: {
            unsigned n = 0;
            while (rd16(OFF_RESPONSE) == BUSY)
                assert(++n < MAX_POLL);
            break;
        }
        case OP_WRL:
            assert(s_h.write_long(s_h.ctx, BASE + OFF_OPERAND, s_d[steps[i].arg]));
            break;
        case OP_WRL_IMM:
            assert(s_h.write_long(s_h.ctx, BASE + OFF_OPERAND, steps[i].arg));
            break;
        case OP_TST:
            (void)rd16(OFF_RESPONSE);
            break;
        case OP_RDL:
            assert(s_h.read_long(s_h.ctx, BASE + OFF_OPERAND, &s_d[steps[i].arg]));
            break;
        case OP_RESET:
            assert(s_h.write_word(s_h.ctx, BASE + OFF_RESTORE, 0x0000u));
            (void)rd16(OFF_RESTORE);
            break;
        }
    }
}

#define CMD(x)     {OP_CMD, (x)}
#define POLL       {OP_POLL, 0}
#define WRL(r)     {OP_WRL, (r)}
#define WRL_IMM(v) {OP_WRL_IMM, (v)}
#define TST        {OP_TST, 0}
#define RDL(r)     {OP_RDL, (r)}
#define STEPS(a)   (a), sizeof(a) / sizeof((a)[0])

/* L0016ac: FMOVE.L d0,FP0 / FMOVE.D FP0,d0:d1（long → double） */
static const step_t L0016ac[] = {CMD(0x4000), POLL, WRL(0), TST,
                                 CMD(0x7400), POLL, RDL(0), RDL(1)};

/* L0016da: FMOVE.L #0,FPSR / FMOVE.D d0:d1,FP0 / FINTRZ / FMOVE.L FP0,d0 / FMOVE.L FPSR,d6
 * （double → long。続く epilogue が d6 & $00D0 を CCR へ写す） */
static const step_t L0016da[] = {
    CMD(0x8800), POLL, WRL_IMM(0),  TST,  CMD(0x5400), POLL,        WRL(0), WRL(1), TST,
    CMD(0x0003), POLL, CMD(0x6000), POLL, RDL(0),      CMD(0xA800), POLL,   RDL(6)};

/* L001898: FMOVECR #$32,FP0（1.0）/ FADD.D (a6),FP0 / FMOVE.D FP0,(a6)（x + 1.0） */
static const step_t L001898[] = {CMD(0x5C32), POLL,        CMD(0x5422), POLL,   WRL(0), WRL(1),
                                 TST,         CMD(0x7400), POLL,        RDL(0), RDL(1)};

/* L0018e4: FMOVECR #$32,FP0 / FADD.S (a6),FP0 / FMOVE.S FP0,(a6)（single x + 1.0） */
static const step_t L0018e4[] = {CMD(0x5C32), POLL,        CMD(0x4422), POLL,  WRL(0),
                                 TST,         CMD(0x6400), POLL,        RDL(0)};

/* L001928: FMOVECR #$32,FP0 / FSUB.D (a6),FP0 / FMOVE.D FP0,(a6)（1.0 - x。この後 eori.w
 * #$8000,(a6) で 符号反転して x - 1.0） */
static const step_t L001928[] = {CMD(0x5C32), POLL,        CMD(0x5428), POLL,   WRL(0), WRL(1),
                                 TST,         CMD(0x7400), POLL,        RDL(0), RDL(1)};

/* L001d3e: FMOVE.L #0,FPSR / FMOVE.D (a6),FP0 / FMUL.D (8,a6),FP0 / FMOVE.D FP0,(a6) /
 * FMOVE.L FPSR,d6（epilogue は d6 & $00F0） */
static const step_t L001d3e[] = {
    CMD(0x8800), POLL, WRL_IMM(0),  TST,    CMD(0x5400), POLL,   WRL(0),
    WRL(1),      TST,  CMD(0x5423), POLL,   WRL(2),      WRL(3), TST,
    CMD(0x7400), POLL, RDL(0),      RDL(1), CMD(0xA800), POLL,   RDL(6)};

/* 倍精度の単項関数ルーチン（FTAN/FATAN/FSIN/FCOS/FLOGN/FETOX/FSQRT/FSINH/FCOSH/FTANH/FATANH/
 * FASIN/FACOS/FLOG10/FLOG2）は、FPU を呼ぶ経路が全て同じ形:
 * FOP.D (d0:d1),FP0 を発行 → d0/d1 を operand へ書く → FMOVE.D FP0,d0:d1 で読み戻す。
 * 引数の特殊値（0・Inf・NaN・範囲外）は FPU を呼ばずに host 側で処理し、その経路は ABORT
 * で表す。 */
#define UNARY_STEPS(name, op)                                             \
    static const step_t name[] = {CMD(op),     POLL, WRL(0), WRL(1), TST, \
                                  CMD(0x7400), POLL, RDL(0), RDL(1)}

UNARY_STEPS(L003632, 0x540F); /* FTAN */
UNARY_STEPS(L00367a, 0x540A); /* FATAN */
UNARY_STEPS(L0036be, 0x540E); /* FSIN */
UNARY_STEPS(L003706, 0x541D); /* FCOS */
UNARY_STEPS(L00374c, 0x5414); /* FLOGN */
UNARY_STEPS(L0037da, 0x5410); /* FETOX */
UNARY_STEPS(L00384a, 0x5404); /* FSQRT */
UNARY_STEPS(L00389c, 0x5402); /* FSINH */
UNARY_STEPS(L003938, 0x5419); /* FCOSH */
UNARY_STEPS(L0039d0, 0x5409); /* FTANH */
UNARY_STEPS(L003a00, 0x540D); /* FATANH */
UNARY_STEPS(L003aa0, 0x540C); /* FASIN */
UNARY_STEPS(L003b38, 0x541C); /* FACOS */
UNARY_STEPS(L003bc8, 0x5415); /* FLOG10 */
UNARY_STEPS(L003be8, 0x5416); /* FLOG2 */

/* L0035cc: FMOVECR #$00,FP0（π）/ FMUL.D d0:d1,FP0 / FMOVE.D FP0,d0:d1 / FMOVE.L FPSR,d6
 * （π * x。epilogue は d6 & $00D0） */
static const step_t L0035cc[] = {CMD(0x5C00), POLL,        CMD(0x5423), POLL,  WRL(0),
                                 WRL(1),      TST,         CMD(0x7400), POLL,  RDL(0),
                                 RDL(1),      CMD(0xA800), POLL,        RDL(6)};

/* FPU を呼ぶ前に host 側で結果を決める経路（L0037aa/L0037ca 等）は、FPU を reset する:
 * move.w #0,(-$000a,a5)（$E9E006 へ null frame の restore）/ tst.w (-$000a,a5) */
static const step_t ABORT_AFTER_OPERAND[] = {{OP_RESET, 0}};

/* epilogue（L00173c 付近）: and.w #mask,d6 → 0 なら CCR=0、それ以外は d6 を 2 回 add.b して
 * carry なら addq、さらに add.b と addq #1 で CCR へ。エラー有無は CCR の C bit で見る */
static uint8_t add_b(uint8_t v, unsigned *carry) {
    *carry = v & 0x80u ? 1u : 0u;
    return (uint8_t)(v + v);
}

#define CCR_MASK 0x1Fu /* X N Z V C */

/* L001746/L0017ec（mask $00D0）の epilogue */
static uint8_t epilogue_ccr_d0(uint32_t fpsr, uint16_t mask) {
    uint16_t d6 = (uint16_t)(fpsr & mask);
    unsigned carry = 0;
    uint8_t b = (uint8_t)d6;
    if (d6 == 0)
        return 0;
    b = add_b(b, &carry);
    b = add_b(b, &carry);
    if (carry)
        b++;
    b = add_b(b, &carry);
    b++;
    return b & CCR_MASK;
}

/* L001dcc（mask $00F0）の epilogue: 3 回目の add.b の結果が負なら addq.b #4（DZ を CCR bit2
 * へ） */
static uint8_t epilogue_ccr_f0(uint32_t fpsr, uint16_t mask) {
    uint16_t d6 = (uint16_t)(fpsr & mask);
    unsigned carry = 0;
    uint8_t b = (uint8_t)d6;
    if (d6 == 0)
        return 0;
    b = add_b(b, &carry);
    b = add_b(b, &carry);
    if (carry)
        b++;
    b = add_b(b, &carry);
    if (b & 0x80u)
        b += 4;
    b++;
    return b & CCR_MASK;
}

#define CCR_C 0x01u
#define CCR_V 0x02u

/* IEEE 754 の bit 列（手で置いた期待値） */
#define D_1_5_HI         0x3FF80000u
#define D_2_5_HI         0x40040000u
#define D_3_5_HI         0x400C0000u
#define D_3_75_HI        0x400E0000u
#define D_5_0_HI         0x40140000u
#define D_7_9_HI         0x401F9999u
#define D_7_9_LO         0x9999999Au
#define D_1E30_HI        0x46293E59u
#define D_1E30_LO        0x39A08CEAu
#define D_1E200_HI       0x6974E718u
#define D_1E200_LO       0xD7D7625Au
#define D_INF_HI         0x7FF00000u
#define S_2_5            0x40200000u
#define S_3_5            0x40600000u
#define SIGN_BIT         0x80000000u
#define INT_MAX_POSITIVE 0x7FFFFFFFu
#define AEXC_IOP         0x80u
#define AEXC_OVFL        0x40u
#define MASK_CONVERT     0x00D0u
#define MASK_MUL         0x00F0u

static void test_conversions(void) {
    setup(true);
    /* L → D */
    s_d[0] = 5;
    run(STEPS(L0016ac));
    assert(s_d[0] == D_5_0_HI && s_d[1] == 0);

    /* D → L（FINTRZ）: 範囲内は CCR=0 */
    s_d[0] = D_7_9_HI;
    s_d[1] = D_7_9_LO;
    run(STEPS(L0016da));
    assert(s_d[0] == 7u);
    assert(epilogue_ccr_d0(s_d[6], MASK_CONVERT) == 0);
    /* 負数 */
    s_d[0] = D_7_9_HI | SIGN_BIT;
    s_d[1] = D_7_9_LO;
    run(STEPS(L0016da));
    assert(s_d[0] == (uint32_t)-7);
    assert(epilogue_ccr_d0(s_d[6], MASK_CONVERT) == 0);
    /* 範囲外: FPSR の AEXC.IOP が立ち、$00D0 マスクで CCR の C bit が立つ */
    s_d[0] = D_1E30_HI;
    s_d[1] = D_1E30_LO;
    run(STEPS(L0016da));
    assert(s_d[0] == INT_MAX_POSITIVE); /* OPERR かつトラップ無効: 最大正整数（881UM §6.1.3） */
    assert((s_d[6] & MASK_CONVERT) == AEXC_IOP);
    assert(epilogue_ccr_d0(s_d[6], MASK_CONVERT) & CCR_C);
    /* 直後の正常な変換で CCR が戻る（FMOVE.L #0,FPSR で FPSR をクリアしている） */
    s_d[0] = D_7_9_HI;
    s_d[1] = D_7_9_LO;
    run(STEPS(L0016da));
    assert(s_d[0] == 7u && epilogue_ccr_d0(s_d[6], MASK_CONVERT) == 0);
    assert(s_bus->fpu_cir_state.violation_count == 0);
    teardown();
    printf("fpu_cir_float3_test: conversions OK\n");
}

static void test_arithmetic(void) {
    setup(true);
    /* x + 1.0（double）: 2.5 → 3.5 */
    s_d[0] = D_2_5_HI;
    s_d[1] = 0;
    run(STEPS(L001898));
    assert(s_d[0] == D_3_5_HI && s_d[1] == 0);
    /* single x + 1.0: 2.5 → 3.5 */
    s_d[0] = S_2_5;
    run(STEPS(L0018e4));
    assert(s_d[0] == S_3_5);
    /* 1.0 - x（double）: 3.5 → -2.5。FLOAT3 はこの後 bit31 を反転して x - 1.0 = 2.5 にする */
    s_d[0] = D_3_5_HI;
    s_d[1] = 0;
    run(STEPS(L001928));
    assert(s_d[0] == (D_2_5_HI | SIGN_BIT) && s_d[1] == 0);
    /* x * y（double）: 1.5 * 2.5 = 3.75、FPSR の $00F0 は 0 */
    s_d[0] = D_1_5_HI;
    s_d[1] = 0;
    s_d[2] = D_2_5_HI;
    s_d[3] = 0;
    run(STEPS(L001d3e));
    assert(s_d[0] == D_3_75_HI && s_d[1] == 0);
    assert(epilogue_ccr_f0(s_d[6], MASK_MUL) == 0);
    /* オーバフロー: 1e200 * 1e200 = +Inf、AEXC.OVFL が立つ */
    s_d[0] = D_1E200_HI;
    s_d[1] = D_1E200_LO;
    s_d[2] = D_1E200_HI;
    s_d[3] = D_1E200_LO;
    run(STEPS(L001d3e));
    assert(s_d[0] == D_INF_HI && s_d[1] == 0);
    assert((s_d[6] & MASK_MUL) & AEXC_OVFL);
    assert(epilogue_ccr_f0(s_d[6], MASK_MUL) == (CCR_V | CCR_C)); /* OVFL は V と C */
    assert(s_bus->fpu_cir_state.violation_count == 0);
    teardown();
    printf("fpu_cir_float3_test: arithmetic OK\n");
}

/* L00451c: FPU 有無の判定。bus error vector を差し替えた上で $E9E006 へ $0000 を書き、tst.w
 * で読む。 ボードなしでは書込みが CPU bus error
 * となり、ドライバは「数値演算プロセッサがありません」へ進む */
static void test_detection_without_board(void) {
    setup(false);
    assert(!s_h.write_word(s_h.ctx, BASE + OFF_RESTORE, 0x0000u));
    uint16_t v = 0;
    assert(!s_h.read_word(s_h.ctx, BASE + OFF_RESTORE, &v));
    teardown();
    printf("fpu_cir_float3_test: detection without board OK\n");
}

typedef struct {
    const char *name;
    const step_t *steps;
    size_t count;
    uint32_t in_hi, in_lo;
    uint32_t out_hi, out_lo;
} unary_case_t;

#define UNARY_CASE(name, ih, il, oh, ol) \
    {#name, name, sizeof(name) / sizeof((name)[0]), ih, il, oh, ol}

#define D_0       0x00000000u
#define D_1_0_HI  0x3FF00000u
#define D_2_0_HI  0x40000000u
#define D_3_0_HI  0x40080000u
#define D_4_0_HI  0x40100000u
#define D_8_0_HI  0x40200000u
#define D_100_HI  0x40590000u
#define D_2_25_HI 0x40020000u
#define D_PI_HI   0x400921FBu
#define D_PI_LO   0x54442D18u
#define D_PI_2_HI 0x3FF921FBu
#define D_PI_4_HI 0x3FE921FBu
#define D_E_HI    0x4005BF0Au
#define D_E_LO    0x8B145769u
#define D_LN2_HI  0x3FE62E42u
#define D_LN2_LO  0xFEFA39EFu

/* 手で置いた期待値。入力は全て exact に表せる値で、結果が exact か、IEEE 754 倍精度の定数。 */
static void test_unary_functions(void) {
    static const unary_case_t cases[] = {
        UNARY_CASE(L003632, D_0, D_0, D_0, D_0),                /* tan 0 = 0 */
        UNARY_CASE(L00367a, D_0, D_0, D_0, D_0),                /* atan 0 = 0 */
        UNARY_CASE(L00367a, D_1_0_HI, D_0, D_PI_4_HI, D_PI_LO), /* atan 1 = π/4 */
        UNARY_CASE(L0036be, D_0, D_0, D_0, D_0),                /* sin 0 = 0 */
        UNARY_CASE(L003706, D_0, D_0, D_1_0_HI, D_0),           /* cos 0 = 1 */
        UNARY_CASE(L00374c, D_1_0_HI, D_0, D_0, D_0),           /* ln 1 = 0 */
        UNARY_CASE(L00374c, D_2_0_HI, D_0, D_LN2_HI, D_LN2_LO), /* ln 2 */
        UNARY_CASE(L0037da, D_0, D_0, D_1_0_HI, D_0),           /* e^0 = 1 */
        UNARY_CASE(L0037da, D_1_0_HI, D_0, D_E_HI, D_E_LO),     /* e^1 = e */
        UNARY_CASE(L00384a, D_4_0_HI, D_0, D_2_0_HI, D_0),      /* sqrt 4 = 2 */
        UNARY_CASE(L00384a, D_2_25_HI, D_0, 0x3FF80000u, D_0),  /* sqrt 2.25 = 1.5 */
        UNARY_CASE(L00389c, D_0, D_0, D_0, D_0),                /* sinh 0 = 0 */
        UNARY_CASE(L003938, D_0, D_0, D_1_0_HI, D_0),           /* cosh 0 = 1 */
        UNARY_CASE(L0039d0, D_0, D_0, D_0, D_0),                /* tanh 0 = 0 */
        UNARY_CASE(L003a00, D_0, D_0, D_0, D_0),                /* atanh 0 = 0 */
        UNARY_CASE(L003aa0, D_0, D_0, D_0, D_0),                /* asin 0 = 0 */
        UNARY_CASE(L003b38, D_0, D_0, D_PI_2_HI, D_PI_LO),      /* acos 0 = π/2 */
        UNARY_CASE(L003bc8, D_100_HI, D_0, D_2_0_HI, D_0),      /* log10 100 = 2 */
        UNARY_CASE(L003be8, D_8_0_HI, D_0, D_3_0_HI, D_0),      /* log2 8 = 3 */
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        setup(true);
        s_d[0] = cases[i].in_hi;
        s_d[1] = cases[i].in_lo;
        run(cases[i].steps, cases[i].count);
        if (s_d[0] != cases[i].out_hi || s_d[1] != cases[i].out_lo)
            fprintf(stderr, "%s(%08X:%08X) = %08X:%08X, expected %08X:%08X\n", cases[i].name,
                    cases[i].in_hi, cases[i].in_lo, s_d[0], s_d[1], cases[i].out_hi,
                    cases[i].out_lo);
        assert(s_d[0] == cases[i].out_hi && s_d[1] == cases[i].out_lo);
        assert(s_bus->fpu_cir_state.violation_count == 0);
        teardown();
    }
    printf("fpu_cir_float3_test: unary functions OK\n");
}

/* L0035cc: FMOVECR #$00（π）× x */
static void test_pi_multiply(void) {
    setup(true);
    s_d[0] = D_1_0_HI;
    s_d[1] = 0;
    run(STEPS(L0035cc));
    assert(s_d[0] == D_PI_HI && s_d[1] == D_PI_LO);
    assert(epilogue_ccr_d0(s_d[6], MASK_CONVERT) == 0);
    s_d[0] = D_2_0_HI;
    s_d[1] = 0;
    run(STEPS(L0035cc));
    assert(s_d[0] == 0x401921FBu && s_d[1] == D_PI_LO); /* 2π */
    assert(epilogue_ccr_d0(s_d[6], MASK_CONVERT) == 0);
    assert(s_bus->fpu_cir_state.violation_count == 0);
    teardown();
    printf("fpu_cir_float3_test: pi multiply OK\n");
}

/* FLOGN(0) など、operand を送った後に host 側が結果を決める経路（L003792/L0037aa/L0037ca）:
 * result を読まずに null frame の restore で FPU を reset する。続く通常の dialog が動く */
static void test_unary_abort_path(void) {
    setup(true);
    static const step_t head[] = {CMD(0x5414), POLL, WRL(0), WRL(1),
                                  TST}; /* ln 0 の operand まで */
    s_d[0] = 0;
    s_d[1] = 0;
    run(STEPS(head));
    run(STEPS(ABORT_AFTER_OPERAND));
    mc68k_fpu_regs_t after;
    mc68k_get_fpu_regs(s_bus->fpu_cir, &after);
    assert(after.fp[0].sign_exp == 0x7FFF); /* reset 後は非数 */
    s_d[0] = D_4_0_HI;
    s_d[1] = 0;
    run(STEPS(L00384a)); /* reset 後も FSQRT が正しく動く */
    assert(s_d[0] == D_2_0_HI && s_d[1] == 0);
    assert(s_bus->fpu_cir_state.violation_count == 0);
    teardown();
    printf("fpu_cir_float3_test: unary abort path OK\n");
}

/* 2 つの reset 経路: L00451c の move.w #0,($E9E006)+tst.w と、_B_INTVCS d1=$1FF で登録される
 * L001698 の move.w #0,($E9E006)+move.w ($E9E006),d0。どちらも null frame restore で、FPn を
 * reset する */
static void reset_sequence_detect(void) { /* L00451c */
    assert(s_h.write_word(s_h.ctx, BASE + OFF_RESTORE, 0x0000u));
    (void)rd16(OFF_RESTORE);
}

static void reset_sequence_handler(void) { /* L001698 */
    assert(s_h.write_word(s_h.ctx, BASE + OFF_RESTORE, 0x0000u));
    s_d[0] = rd16(OFF_RESTORE);
}

static void test_reset_paths(void) {
    static void (*const resets[])(void) = {reset_sequence_detect, reset_sequence_handler};
    for (unsigned i = 0; i < sizeof(resets) / sizeof(resets[0]); i++) {
        setup(true);
        s_d[0] = 5;
        run(STEPS(L0016ac)); /* FP0 = 5.0 */
        mc68k_fpu_regs_t before;
        mc68k_get_fpu_regs(s_bus->fpu_cir, &before);
        assert(before.fp[0].sign_exp == 0x4001);
        resets[i]();
        mc68k_fpu_regs_t after;
        mc68k_get_fpu_regs(s_bus->fpu_cir, &after);
        assert(after.fp[0].sign_exp == 0x7FFF); /* reset 後は非数 */
        if (i == 1)
            assert(s_d[0] == 0x0000u); /* null frame の readback */
        /* reset 後も通常の dialog が動く */
        s_d[0] = 5;
        run(STEPS(L0016ac));
        assert(s_d[0] == D_5_0_HI && s_d[1] == 0);
        assert(s_bus->fpu_cir_state.violation_count == 0);
        teardown();
    }
    printf("fpu_cir_float3_test: reset paths OK\n");
}

int main(void) {
    test_conversions();
    test_arithmetic();
    test_unary_functions();
    test_pi_multiply();
    test_unary_abort_path();
    test_detection_without_board();
    test_reset_paths();
    printf("fpu_cir_float3_test: ALL OK\n");
    return 0;
}
