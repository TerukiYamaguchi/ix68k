#include "bus/fpu_cir.h"

/* Primitive の共通属性（881UM Figure 7-7） */
#define PRIM_CA            (1u << 15)
#define PRIM_PC            (1u << 14)
#define PRIM_DR            (1u << 13)
/* primitive 種別の固定ビット（Table 7-7 の値から） */
#define PRIM_NULL          0x0800u
#define PRIM_NULL_IA       (1u << 8)
#define PRIM_NULL_PF       (1u << 1)
#define PRIM_NULL_TF       (1u << 0)
#define PRIM_EA_TRANSFER   0x1000u
#define PRIM_EA_SHIFT      8
#define PRIM_MP_REGISTER   0x0C00u
#define PRIM_FP_REGISTERS  0x0100u
#define PRIM_TAKE_PRE      0x1C00u
#define PRIM_TAKE_MID      0x1D00u
#define PRIM_LOW_BYTE      0x00FFu
#define PRIM_DATA_REG_MASK 0x07u
#define PRIM_VALID_EA_MASK 0x07u

#define CONDITION_MASK              0x3Fu /* predicate は下位6bit */
/* 転送中の入れ子 save の拒否は format word の上位 byte が $02（881UM Table 6-6 の $02xx。
 * frame size の下位 byte は未定義で、mc68000 は $0200 を返す）。 */
#define SAVE_FORMAT_VERSION_SHIFT   8
#define SAVE_FORMAT_ILLEGAL_VERSION 0x02u
#define WORD_BYTES                  2u
#define LONG_WORD_COUNT             2u

void fpu_cir_init(fpu_cir_t *cir) {
    *cir = (fpu_cir_t){0};
}

uint16_t fpu_cir_encode_response(const mc68k_fpu_response_t *r) {
    uint16_t word = 0;
    if (r->coprocessor_active)
        word |= PRIM_CA;
    if (r->request_pc)
        word |= PRIM_PC;
    switch (r->kind) {
    case MC68K_FPU_RESPONSE_NULL:
        word |= PRIM_NULL;
        if (r->interruptible)
            word |= PRIM_NULL_IA;
        if (!r->processing)
            word |= PRIM_NULL_PF;
        if (r->condition_true)
            word |= PRIM_NULL_TF;
        break;
    case MC68K_FPU_RESPONSE_EA_TRANSFER:
        word |= PRIM_EA_TRANSFER |
                (uint16_t)((r->valid_ea & PRIM_VALID_EA_MASK) << PRIM_EA_SHIFT) | r->length;
        if (r->transfer_to_host)
            word |= PRIM_DR;
        break;
    case MC68K_FPU_RESPONSE_MP_REGISTER:
        word |= PRIM_MP_REGISTER | (r->data_register & PRIM_DATA_REG_MASK);
        break;
    case MC68K_FPU_RESPONSE_FP_REGISTERS:
        word |= PRIM_FP_REGISTERS | r->length;
        if (r->transfer_to_host)
            word |= PRIM_DR;
        break;
    case MC68K_FPU_RESPONSE_TAKE_PRE_EXCEPTION:
        word |= PRIM_TAKE_PRE | r->vector;
        break;
    case MC68K_FPU_RESPONSE_TAKE_MID_EXCEPTION:
        word |= PRIM_TAKE_MID | r->vector;
        break;
    }
    return word;
}

/* 転送記述子を1 word 分進める（尽きたら 0 で止める）。実際に引いた byte 数を返す */
static uint32_t consume_word(fpu_cir_t *cir) {
    uint32_t consumed = cir->remaining > WORD_BYTES ? WORD_BYTES : cir->remaining;
    cir->remaining -= consumed;
    return consumed;
}

/* 保持した上位 word を捨てる。モデルへは未送信なので、保持時に引いた分を記述子へ戻す
 * （戻さないと書き直した単位が1 word と誤判定され、半値がモデルへ届く） */
static void drop_write_partial(fpu_cir_t *cir) {
    if (cir->wr_partial) {
        cir->remaining += cir->wr_consumed;
        cir->wr_partial = false;
    }
}

/* ラッチした下位 word を捨てる。モデルは longword 全体を渡し済みなので、読み飛ばした
 * 下位 word の分だけ記述子を進めてモデルの転送位置と揃える */
static void drop_read_latch(fpu_cir_t *cir) {
    if (cir->rd_latch) {
        consume_word(cir);
        cir->rd_latch = false;
    }
}

static void discard_operand_partials(fpu_cir_t *cir) {
    drop_write_partial(cir);
    drop_read_latch(cir);
}

static void discard_partials(fpu_cir_t *cir) {
    discard_operand_partials(cir);
    cir->iaddr_partial = false;
}

/* API の false はモデル側が protocol violation として保持し、次の response read で通知される。
 * adapter は cycle を完了させ、診断用に回数だけ数える。 */
static bool track_violation(fpu_cir_t *cir, bool ok) {
    if (!ok)
        cir->violation_count++;
    return ok;
}

/* 単位の word 数: 残りが 1〜2 byte なら1 word、それ以外（無い/尽きた含む）は2 word */
static uint32_t unit_words(const fpu_cir_t *cir) {
    return (cir->remaining > 0 && cir->remaining <= WORD_BYTES) ? 1u : LONG_WORD_COUNT;
}

static uint16_t read_response(fpu_cir_t *cir, mc68k_fpu_t *fpu) {
    if (cir->busy_once) {
        cir->busy_once = false;
        return FPU_CIR_BUSY_PRIMITIVE;
    }
    mc68k_fpu_response_t r = mc68k_fpu_read_response(fpu);
    switch (r.kind) {
    case MC68K_FPU_RESPONSE_EA_TRANSFER:
    case MC68K_FPU_RESPONSE_FP_REGISTERS:
        discard_operand_partials(cir); /* 新しい転送へ旧転送の半端な入力を持ち込まない */
        cir->remaining = r.length;
        break;
    case MC68K_FPU_RESPONSE_MP_REGISTER:
        discard_operand_partials(cir);
        cir->remaining = 0; /* length を持たない: 4 byte 単位 */
        break;
    default:
        break; /* NULL/TAKE は記述子を更新しない */
    }
    return fpu_cir_encode_response(&r);
}

static uint16_t read_save(fpu_cir_t *cir, mc68k_fpu_t *fpu) {
    uint16_t format = 0;
    size_t body_bytes = 0;
    cir->busy_once = false;
    mc68k_fpu_begin_save(fpu, &format, &body_bytes);
    if ((format >> SAVE_FORMAT_VERSION_SHIFT) == SAVE_FORMAT_ILLEGAL_VERSION && body_bytes == 0)
        return format; /* 拒否された入れ子 save: モデルも adapter も転送状態を変えない */
    discard_partials(cir);
    cir->remaining = (uint32_t)body_bytes;
    return format;
}

static uint16_t read_operand(fpu_cir_t *cir, mc68k_fpu_t *fpu) {
    drop_write_partial(cir);
    cir->iaddr_partial = false;
    if (cir->rd_latch) {
        cir->rd_latch = false;
        consume_word(cir);
        return cir->rd_lo;
    }
    uint32_t value = 0;
    if (!track_violation(cir, mc68k_fpu_read_operand(fpu, &value)))
        return FPU_CIR_OPEN_WORD;
    if (unit_words(cir) == LONG_WORD_COUNT) {
        cir->rd_latch = true;
        cir->rd_lo = (uint16_t)value;
    }
    consume_word(cir);
    return (uint16_t)(value >> FPU_CIR_WORD_BITS);
}

static void write_operand(fpu_cir_t *cir, mc68k_fpu_t *fpu, uint16_t word) {
    drop_read_latch(cir);
    cir->iaddr_partial = false;
    if (cir->wr_partial) {
        cir->wr_partial = false;
        if (track_violation(cir, mc68k_fpu_write_operand(
                                     fpu, ((uint32_t)cir->wr_hi << FPU_CIR_WORD_BITS) | word)))
            consume_word(cir);
        return;
    }
    if (unit_words(cir) == 1u) {
        if (track_violation(cir,
                            mc68k_fpu_write_operand(fpu, (uint32_t)word << FPU_CIR_WORD_BITS)))
            consume_word(cir);
        return;
    }
    cir->wr_partial = true;
    cir->wr_hi = word;
    cir->wr_consumed = consume_word(cir);
}

static void write_instruction_address(fpu_cir_t *cir, mc68k_fpu_t *fpu, uint16_t word) {
    discard_operand_partials(cir);
    if (cir->iaddr_partial) {
        cir->iaddr_partial = false;
        track_violation(cir, mc68k_fpu_write_instruction_address(
                                 fpu, ((uint32_t)cir->iaddr_hi << FPU_CIR_WORD_BITS) | word));
        return;
    }
    cir->iaddr_partial = true;
    cir->iaddr_hi = word;
}

uint16_t fpu_cir_read16(fpu_cir_t *cir, mc68k_fpu_t *fpu, uint32_t offset) {
    switch (offset) {
    case FPU_CIR_OFF_RESPONSE:
        return read_response(cir, fpu);
    case FPU_CIR_OFF_SAVE:
        return read_save(cir, fpu);
    case FPU_CIR_OFF_OPERAND_HI:
    case FPU_CIR_OFF_OPERAND_LO:
        return read_operand(cir, fpu);
    case FPU_CIR_OFF_RESTORE:
        discard_partials(cir);
        return cir->restore_started ? cir->restore_readback : FPU_CIR_OPEN_WORD;
    case FPU_CIR_OFF_REGSEL: {
        uint16_t mask = 0;
        discard_partials(cir);
        return track_violation(cir, mc68k_fpu_read_register_select(fpu, &mask))
                   ? mask
                   : FPU_CIR_OPEN_WORD;
    }
    default:
        discard_partials(cir); /* 書込み専用・予約位置 */
        return FPU_CIR_OPEN_WORD;
    }
}

void fpu_cir_write16(fpu_cir_t *cir, mc68k_fpu_t *fpu, uint32_t offset, uint16_t value) {
    switch (offset) {
    case FPU_CIR_OFF_CONTROL:
        /* 値を問わず ABORT。部分状態・記述子・busy を先に破棄する */
        discard_partials(cir);
        cir->remaining = 0;
        cir->busy_once = false;
        mc68k_fpu_control(fpu, MC68K_FPU_CONTROL_ABORT);
        break;
    case FPU_CIR_OFF_RESTORE: {
        uint16_t readback = 0;
        size_t body_bytes = 0;
        discard_partials(cir);
        cir->busy_once = false;
        mc68k_fpu_begin_restore(fpu, value, &readback, &body_bytes);
        cir->restore_started = true;
        cir->restore_readback = readback;
        cir->remaining = (uint32_t)body_bytes;
        break;
    }
    case FPU_CIR_OFF_COMMAND:
        discard_partials(cir);
        track_violation(cir, mc68k_fpu_write_command_word(fpu, value));
        cir->busy_once = true;
        break;
    case FPU_CIR_OFF_CONDITION:
        discard_partials(cir);
        track_violation(cir, mc68k_fpu_write_condition(fpu, (uint8_t)(value & CONDITION_MASK)));
        cir->busy_once = true;
        break;
    case FPU_CIR_OFF_OPERAND_HI:
    case FPU_CIR_OFF_OPERAND_LO:
        write_operand(cir, fpu, value);
        break;
    case FPU_CIR_OFF_REGSEL:
        discard_partials(cir);
        track_violation(cir, mc68k_fpu_write_register_select(fpu)); /* 常に false */
        break;
    case FPU_CIR_OFF_IADDR_HI:
    case FPU_CIR_OFF_IADDR_LO:
        write_instruction_address(cir, fpu, value);
        break;
    default:
        discard_partials(cir); /* 読出し専用・予約位置への書込みは無視 */
        break;
    }
}
