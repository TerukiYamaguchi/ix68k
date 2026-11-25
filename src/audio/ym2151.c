#include "audio/ym2151.h"
#include "audio/ym2151_tables.h"
#include <string.h>

/*
 * ymfm(Copyright (c) 2021, Aaron Giles、BSD 3-Clause License)を、XEiJ
 * xeij/YM2151.java(https://stdkmd.net/xeij/)のJava移植経由でC言語へ移植。
 * 関数コメントの行番号は
 * XEiJ_trace/xeij/YM2151.java上の対応箇所。
 *
 * ライセンス全文は LICENSES/ymfm-BSD-3-Clause.txt を参照。
 *
 * Java実装との主な相違点:
 *   - opdata_cache.waveform(L611)は省略。OPMはWAVEFORMS=1で波形は常に
 *     ym2151_t.waveformの1つだけのため、cache経由の間接参照は不要。
 *   - opdata_cache.eg_shift(L619)は省略。OPMはSSG-EG非搭載でeg_shiftは
 *     常に0（envelope_attenuationのシフトが恒等になる）。
 *   - CSMキーオントリガは未実装。BUSYとIRQ配線はperipheral/opm.cが扱う。
 */

#define ALL_CHANNELS        ((1u << YM2151_CHANNELS) - 1u)
#define WAVEFORM_LENGTH     0x400u /* L228 */
#define LFO_WAVEFORM_LENGTH 256u   /* L393 */
#define PHASE_STEP_DYNAMIC  1u     /* L233 */
#define EG_QUIET            0x380u /* L236 */
#define REG_MODE            0x14u  /* L208 */
#define STATUS_TIMERA       0x01u  /* L209 */
#define STATUS_TIMERB       0x02u  /* L210 */
#define EG_ATTACK           1      /* L216 */
#define EG_DECAY            2
#define EG_SUSTAIN          3
#define EG_RELEASE          4
#define KEYON_NORMAL        0 /* L444 */
#define KEYON_CSM           2

/* ---- 基本ヘルパー(L85-104相当) ---- */

static inline uint32_t bitfield(uint32_t value, int start, int length) {
    return (value >> start) & ((1u << length) - 1u);
}
static inline uint32_t umin32(uint32_t x, uint32_t y) {
    return x < y ? x : y;
}
static inline int32_t clampi(int32_t value, int32_t minval, int32_t maxval) {
    if (value < minval)
        return minval;
    if (value > maxval)
        return maxval;
    return value;
}

/* 32bit値の先頭0ビット数を返す(YM2151.java count_leading_zeros() L113-123)。
 * roundtrip_fp()専用のヘルパーで、value==0のときのみ32を返す特殊な符号付き
 * シフトループを忠実に移植する（__builtin_clzはvalue==0が未定義動作のため
 * 使わない）。 */
static uint8_t count_leading_zeros(uint32_t value) {
    if (value == 0)
        return 32;
    uint8_t count = 0;
    for (; (int32_t)value >= 0; count++)
        value <<= 1;
    return count;
}

/* YM3012 DACのマンティッサ/指数形式を再現し、8チャンネルの総和を
 * -32768〜32767へクリップする(YM2151.java roundtrip_fp() L152-174)。 */
static int32_t roundtrip_fp(int32_t value) {
    if (value < -32768)
        return -32768;
    if (value > 32767)
        return 32767;

    /* 符号ビット以降の連続同値ビット数(leading sign bits)を数える。
     * 負値はビット反転してからcount_leading_zerosを使う。 */
    int32_t scanvalue = value ^ (value >> 31);
    int32_t exponent = 7 - (int32_t)count_leading_zeros((uint32_t)(scanvalue << 17));
    if (exponent < 1)
        exponent = 1;
    exponent -= 1;
    return (int32_t)(int16_t)((value >> exponent) << exponent);
}

/* ---- テーブル参照ヘルパー(L457-580相当) ---- */

static uint32_t abs_sin_attenuation(uint32_t input) {
    if (bitfield(input, 8, 1) != 0)
        input = ~input;
    return s_sin_table[input & 0xffu];
}

static uint32_t attenuation_to_volume(uint32_t input) {
    return (uint32_t)(s_power_table[input & 0xffu] >> (input >> 8));
}

static uint32_t attenuation_increment(uint32_t rate, uint32_t index) {
    return bitfield(s_increment_table[rate], 4 * (int)index, 4);
}

static int32_t detune_adjustment(uint32_t detune, uint32_t keycode) {
    int32_t result = (int32_t)s_detune_adjustment[4 * keycode + (detune & 3u)];
    return bitfield(detune, 2, 1) != 0 ? -result : result;
}

/* OPM concatenated block(3bit)+keycode(4bit)+key fraction(6bit)から
 * 0.10位相ステップを求める(L516-580)。 */
static uint32_t opm_key_code_to_phase_step(uint32_t block_freq, int32_t delta) {
    uint32_t block = bitfield(block_freq, 10, 3);
    int32_t adjusted_code =
        (int32_t)bitfield(block_freq, 6, 4) - (int32_t)bitfield(block_freq, 8, 2);
    int32_t eff_freq = (adjusted_code << 6) | (int32_t)bitfield(block_freq, 0, 6);
    eff_freq += delta;

    /* Java Integer.compareUnsigned(eff_freq,768)>=0: eff_freqが負の場合も
     * unsigned比較で真になる(下のif(eff_freq<0)分岐で正しく処理される)。
     */
    if ((uint32_t)eff_freq >= 768u) {
        if (eff_freq < 0) {
            eff_freq += 768;
            if (block-- == 0)
                return s_phase_step[0] >> 7;
        } else {
            eff_freq -= 768;
            if (eff_freq >= 768) {
                block++;
                eff_freq -= 768;
            }
            if (block++ >= 7)
                return s_phase_step[767];
        }
    }

    return s_phase_step[eff_freq] >> (block ^ 7u);
}

/* ---- レジスタ配列アクセス(L1928-2059相当) ---- */

static uint32_t regbyte(const ym2151_t *c, int offset, int start, int count, int extra_offset) {
    return bitfield(c->regdata[offset + extra_offset], start, count);
}
static uint32_t regword(const ym2151_t *c, int offset1, int start1, int count1, int offset2,
                        int start2, int count2, int extra_offset) {
    return (regbyte(c, offset1, start1, count1, extra_offset) << count2) |
           regbyte(c, offset2, start2, count2, extra_offset);
}

static uint32_t reg_lfo_reset(const ym2151_t *c) {
    return regbyte(c, 0x01, 1, 1, 0);
}
static uint32_t reg_noise_frequency(const ym2151_t *c) {
    return regbyte(c, 0x0f, 0, 5, 0) ^ 0x1fu;
}
static uint32_t reg_noise_enable(const ym2151_t *c) {
    return regbyte(c, 0x0f, 7, 1, 0);
}
static uint32_t reg_lfo_rate(const ym2151_t *c) {
    return regbyte(c, 0x18, 0, 8, 0);
}
static uint32_t reg_lfo_am_depth(const ym2151_t *c) {
    return regbyte(c, 0x19, 0, 7, 0);
}
static uint32_t reg_lfo_pm_depth(const ym2151_t *c) {
    return regbyte(c, 0x1a, 0, 7, 0);
}
static uint32_t reg_lfo_waveform(const ym2151_t *c) {
    return regbyte(c, 0x1b, 0, 2, 0);
}

/* タイマA/B関連(0x10-0x14、L1957-1982相当)。CSMはbit7、
 * LOAD/ENABLE/RESETはbit0-5。 */
static uint32_t timer_a_value(const ym2151_t *c) {
    return regword(c, 0x10, 0, 8, 0x11, 0, 2, 0);
}
static uint32_t timer_b_value(const ym2151_t *c) {
    return regbyte(c, 0x12, 0, 8, 0);
}
static uint32_t reg_reset_timer_b(const ym2151_t *c) {
    return regbyte(c, 0x14, 5, 1, 0);
}
static uint32_t reg_reset_timer_a(const ym2151_t *c) {
    return regbyte(c, 0x14, 4, 1, 0);
}
static uint32_t reg_load_timer_b(const ym2151_t *c) {
    return regbyte(c, 0x14, 1, 1, 0);
}
static uint32_t reg_load_timer_a(const ym2151_t *c) {
    return regbyte(c, 0x14, 0, 1, 0);
}

static uint32_t timer_a_period(const ym2151_t *c) {
    return 64u * (1024u - timer_a_value(c));
}
static uint32_t timer_b_period(const ym2151_t *c) {
    return 1024u * (256u - timer_b_value(c));
}

static uint32_t ch_output_any(const ym2151_t *c, int choffs) {
    return regbyte(c, 0x20, 6, 2, choffs);
}
static uint32_t ch_output_0(const ym2151_t *c, int choffs) {
    return regbyte(c, 0x20, 6, 1, choffs);
}
static uint32_t ch_output_1(const ym2151_t *c, int choffs) {
    return regbyte(c, 0x20, 7, 1, choffs);
}
static uint32_t ch_feedback(const ym2151_t *c, int choffs) {
    return regbyte(c, 0x20, 3, 3, choffs);
}
static uint32_t ch_algorithm(const ym2151_t *c, int choffs) {
    return regbyte(c, 0x20, 0, 3, choffs);
}
static uint32_t ch_block_freq(const ym2151_t *c, int choffs) {
    return regword(c, 0x28, 0, 7, 0x30, 2, 6, choffs);
}
static uint32_t ch_lfo_pm_sens(const ym2151_t *c, int choffs) {
    return regbyte(c, 0x38, 4, 3, choffs);
}
static uint32_t ch_lfo_am_sens(const ym2151_t *c, int choffs) {
    return regbyte(c, 0x38, 0, 2, choffs);
}

static uint32_t op_detune(const ym2151_t *c, int opoffs) {
    return regbyte(c, 0x40, 4, 3, opoffs);
}
static uint32_t op_multiple(const ym2151_t *c, int opoffs) {
    return regbyte(c, 0x40, 0, 4, opoffs);
}
static uint32_t op_total_level(const ym2151_t *c, int opoffs) {
    return regbyte(c, 0x60, 0, 7, opoffs);
}
static uint32_t op_ksr(const ym2151_t *c, int opoffs) {
    return regbyte(c, 0x80, 6, 2, opoffs);
}
static uint32_t op_attack_rate(const ym2151_t *c, int opoffs) {
    return regbyte(c, 0x80, 0, 5, opoffs);
}
static uint32_t op_lfo_am_enable(const ym2151_t *c, int opoffs) {
    return regbyte(c, 0xa0, 7, 1, opoffs);
}
static uint32_t op_decay_rate(const ym2151_t *c, int opoffs) {
    return regbyte(c, 0xa0, 0, 5, opoffs);
}
static uint32_t op_detune2(const ym2151_t *c, int opoffs) {
    return regbyte(c, 0xc0, 6, 2, opoffs);
}
static uint32_t op_sustain_rate(const ym2151_t *c, int opoffs) {
    return regbyte(c, 0xc0, 0, 5, opoffs);
}
static uint32_t op_sustain_level(const ym2151_t *c, int opoffs) {
    return regbyte(c, 0xe0, 4, 4, opoffs);
}
static uint32_t op_release_rate(const ym2151_t *c, int opoffs) {
    return regbyte(c, 0xe0, 0, 4, opoffs);
}

/* ---- キャッシュ計算(L1837-1926相当) ---- */

static uint32_t effective_rate(uint32_t rawrate, uint32_t ksr) {
    return (rawrate == 0) ? 0 : umin32(rawrate + ksr, 63u);
}

static uint32_t compute_phase_step(ym2151_t *c, int choffs, int opoffs,
                                   ym2151_opdata_cache_t *cache, int32_t lfo_raw_pm) {
    int32_t delta = s_detune2_delta[op_detune2(c, opoffs)];

    uint32_t pm_sensitivity = ch_lfo_pm_sens(c, choffs);
    if (pm_sensitivity != 0) {
        if (pm_sensitivity < 6)
            delta += lfo_raw_pm >> (6 - pm_sensitivity);
        else
            delta += lfo_raw_pm << (pm_sensitivity - 5);
    }

    uint32_t phase_step = opm_key_code_to_phase_step(cache->block_freq, delta);
    phase_step += (uint32_t)cache->detune;
    return (phase_step * cache->multiple) >> 1;
}

static void cache_operator_data(ym2151_t *c, int choffs, int opoffs,
                                ym2151_opdata_cache_t *cache) {
    uint32_t block_freq = cache->block_freq = ch_block_freq(c, choffs);
    uint32_t keycode = bitfield(block_freq, 8, 5);

    cache->detune = detune_adjustment(op_detune(c, opoffs), keycode);

    cache->multiple = op_multiple(c, opoffs) * 2;
    if (cache->multiple == 0)
        cache->multiple = 1;

    if (reg_lfo_pm_depth(c) == 0 || ch_lfo_pm_sens(c, choffs) == 0)
        cache->phase_step = compute_phase_step(c, choffs, opoffs, cache, 0);
    else
        cache->phase_step = PHASE_STEP_DYNAMIC;

    cache->total_level = op_total_level(c, opoffs) << 3;

    cache->eg_sustain = op_sustain_level(c, opoffs);
    cache->eg_sustain |= (cache->eg_sustain + 1) & 0x10u;
    cache->eg_sustain <<= 5;

    uint32_t ksrval = keycode >> (op_ksr(c, opoffs) ^ 3u);
    cache->eg_rate[EG_ATTACK] = (uint8_t)effective_rate(op_attack_rate(c, opoffs) * 2, ksrval);
    cache->eg_rate[EG_DECAY] = (uint8_t)effective_rate(op_decay_rate(c, opoffs) * 2, ksrval);
    cache->eg_rate[EG_SUSTAIN] =
        (uint8_t)effective_rate(op_sustain_rate(c, opoffs) * 2, ksrval);
    cache->eg_rate[EG_RELEASE] =
        (uint8_t)effective_rate(op_release_rate(c, opoffs) * 4 + 2, ksrval);
}

/* ---- オペレータ(fm_operator, L640-970相当) ---- */

static uint32_t operator_phase(const ym2151_operator_t *op) {
    return op->phase >> 10;
}

static void operator_keyonoff(ym2151_operator_t *op, uint32_t on, int type) {
    op->keyon_live = (uint8_t)((op->keyon_live & ~(1u << type)) | (bitfield(on, 0, 1) << type));
}

static void operator_start_attack(ym2151_operator_t *op, int is_restart) {
    if (op->env_state == EG_ATTACK)
        return;
    op->env_state = EG_ATTACK;
    if (!is_restart)
        op->phase = 0;
    if (op->cache.eg_rate[EG_ATTACK] >= 62)
        op->env_attenuation = 0;
}

static void operator_start_release(ym2151_operator_t *op) {
    if (op->env_state >= EG_RELEASE)
        return;
    op->env_state = EG_RELEASE;
}

static void operator_clock_keystate(ym2151_operator_t *op, uint32_t keystate) {
    if ((keystate ^ op->key_state) != 0) {
        op->key_state = (uint8_t)keystate;
        if (keystate != 0)
            operator_start_attack(op, 0);
        else
            operator_start_release(op);
    }
}

static void operator_clock_envelope(ym2151_operator_t *op, uint32_t env_counter) {
    if (op->env_state == EG_ATTACK && op->env_attenuation == 0)
        op->env_state = EG_DECAY;
    if (op->env_state == EG_DECAY && op->env_attenuation >= op->cache.eg_sustain)
        op->env_state = EG_SUSTAIN;

    uint32_t rate = op->cache.eg_rate[op->env_state];
    uint32_t rate_shift = rate >> 2;
    env_counter <<= rate_shift;

    if (bitfield(env_counter, 0, 11) != 0)
        return;

    uint32_t relevant_bits =
        bitfield(env_counter, (rate_shift <= 11u) ? 11 : (int)rate_shift, 3);
    uint32_t increment = attenuation_increment(rate, relevant_bits);

    if (op->env_state == EG_ATTACK) {
        if (rate < 62u) {
            /* Java の (~m_env_attenuation * increment) >>> 4 を32bit演算のまま
             * 忠実に再現(符号付きintのビットパターンをunsigned乗算・論理右
             * シフトで再現する)。 */
            uint32_t inv = ~(uint32_t)op->env_attenuation;
            op->env_attenuation = (uint16_t)(op->env_attenuation + ((inv * increment) >> 4));
        }
    } else {
        op->env_attenuation = (uint16_t)(op->env_attenuation + increment);
        if (op->env_attenuation >= 0x400u)
            op->env_attenuation = 0x3ffu;
    }
}

/* eg_shift省略の理由はファイル冒頭コメント参照(常に0でOPMには無関係)。
 */
static uint32_t operator_envelope_attenuation(const ym2151_t *c, const ym2151_operator_t *op,
                                              uint32_t am_offset) {
    uint32_t result = op->env_attenuation;
    if (op_lfo_am_enable(c, op->opoffs) != 0)
        result += am_offset;
    result += op->cache.total_level;
    return umin32(result, 0x3ffu);
}

/* cache.waveform省略の理由はファイル冒頭コメント参照(OPMは波形1種のみ)。
 */
static int32_t operator_compute_volume(ym2151_t *c, ym2151_operator_t *op, uint32_t phase,
                                       uint32_t am_offset) {
    if (op->env_attenuation > EG_QUIET)
        return 0;

    uint32_t sin_attenuation = c->waveform[phase & (WAVEFORM_LENGTH - 1)];
    uint32_t env_attenuation = operator_envelope_attenuation(c, op, am_offset) << 2;
    int32_t result =
        (int32_t)attenuation_to_volume((sin_attenuation & 0x7fffu) + env_attenuation);
    return bitfield(sin_attenuation, 15, 1) != 0 ? -result : result;
}

static int32_t operator_compute_noise_volume(ym2151_t *c, ym2151_operator_t *op,
                                             uint32_t am_offset) {
    int32_t result = (int32_t)((operator_envelope_attenuation(c, op, am_offset) ^ 0x3ffu) << 1);
    return bitfield(c->noise_state, 0, 1) != 0 ? -result : result;
}

static int operator_prepare(ym2151_t *c, ym2151_operator_t *op) {
    cache_operator_data(c, op->choffs, op->opoffs, &op->cache);
    operator_clock_keystate(op, op->keyon_live != 0 ? 1u : 0u);
    op->keyon_live = (uint8_t)(op->keyon_live & ~(1u << KEYON_CSM));
    return (op->env_state != EG_RELEASE || op->env_attenuation < EG_QUIET);
}

static void operator_clock(ym2151_t *c, ym2151_operator_t *op, uint32_t env_counter,
                           int32_t lfo_raw_pm) {
    if (bitfield(env_counter, 0, 2) == 0)
        operator_clock_envelope(op, env_counter >> 2);

    uint32_t phase_step = op->cache.phase_step;
    if (phase_step == PHASE_STEP_DYNAMIC)
        phase_step = compute_phase_step(c, op->choffs, op->opoffs, &op->cache, lfo_raw_pm);

    op->phase += phase_step;
}

/* ---- チャンネル(fm_channel, L976-1169相当) ---- */

static void channel_keyonoff(ym2151_channel_t *ch, uint32_t states, int type) {
    for (int opnum = 0; opnum < 4; opnum++)
        operator_keyonoff(ch->op[opnum], bitfield(states, opnum, 1), type);
}

static int channel_prepare(ym2151_t *c, ym2151_channel_t *ch) {
    uint32_t active_mask = 0;
    for (int opnum = 0; opnum < 4; opnum++) {
        if (operator_prepare(c, ch->op[opnum]))
            active_mask |= (1u << opnum);
    }
    return active_mask != 0;
}

static void channel_clock(ym2151_t *c, ym2151_channel_t *ch, uint32_t env_counter,
                          int32_t lfo_raw_pm) {
    ch->feedback_0 = ch->feedback_1;
    ch->feedback_1 = ch->feedback_in;
    for (int opnum = 0; opnum < 4; opnum++)
        operator_clock(c, ch->op[opnum], env_counter, lfo_raw_pm);
}

static uint32_t lfo_am_offset(ym2151_t *c, int choffs) {
    uint32_t am_sensitivity = ch_lfo_am_sens(c, choffs);
    if (am_sensitivity == 0)
        return 0;
    return (uint32_t)c->lfo_am << (am_sensitivity - 1);
}

/* 8アルゴリズム(CON=0-7)をs_algorithm_ops経由で分岐せず統一処理する
 * (L1074-1167相当)。outL/outRへ加算合成(呼び出し元がフレーム先頭で0
 * クリアする)。L/Rパンはch_output_0/1のハードスイッチ(クロスフェード
 * 無し)。 */
static void channel_output_4op(ym2151_t *c, ym2151_channel_t *ch, int rshift, int32_t clipmax,
                               int32_t *outL, int32_t *outR) {
    uint32_t am_offset = lfo_am_offset(c, ch->choffs);

    int32_t opmod = 0;
    uint32_t feedback = ch_feedback(c, ch->choffs);
    if (feedback != 0)
        opmod = (int32_t)(ch->feedback_0 + ch->feedback_1) >> (10 - feedback);

    int32_t op1value = operator_compute_volume(
        c, ch->op[0], operator_phase(ch->op[0]) + (uint32_t)opmod, am_offset);
    ch->feedback_in = (int16_t)op1value;

    if (ch_output_any(c, ch->choffs) == 0)
        return;

    uint32_t algorithm_ops = s_algorithm_ops[ch_algorithm(c, ch->choffs)];

    int16_t opout[8];
    opout[0] = 0;
    opout[1] = (int16_t)op1value;

    opmod = opout[bitfield(algorithm_ops, 0, 1)] >> 1;
    opout[2] = (int16_t)operator_compute_volume(
        c, ch->op[1], operator_phase(ch->op[1]) + (uint32_t)opmod, am_offset);
    opout[5] = (int16_t)(opout[1] + opout[2]);

    opmod = opout[bitfield(algorithm_ops, 1, 3)] >> 1;
    opout[3] = (int16_t)operator_compute_volume(
        c, ch->op[2], operator_phase(ch->op[2]) + (uint32_t)opmod, am_offset);
    opout[6] = (int16_t)(opout[1] + opout[3]);
    opout[7] = (int16_t)(opout[2] + opout[3]);

    int32_t result;
    if (reg_noise_enable(c) != 0 && ch->choffs == 7) {
        result = operator_compute_noise_volume(c, ch->op[3], am_offset);
    } else {
        opmod = opout[bitfield(algorithm_ops, 4, 3)] >> 1;
        result = operator_compute_volume(
            c, ch->op[3], operator_phase(ch->op[3]) + (uint32_t)opmod, am_offset);
    }
    result >>= rshift;

    int32_t clipmin = -clipmax - 1;
    if (bitfield(algorithm_ops, 7, 1) != 0)
        result = clampi(result + (opout[1] >> rshift), clipmin, clipmax);
    if (bitfield(algorithm_ops, 8, 1) != 0)
        result = clampi(result + (opout[2] >> rshift), clipmin, clipmax);
    if (bitfield(algorithm_ops, 9, 1) != 0)
        result = clampi(result + (opout[3] >> rshift), clipmin, clipmax);

    if (ch_output_0(c, ch->choffs) != 0)
        *outL += result;
    if (ch_output_1(c, ch->choffs) != 0)
        *outR += result;
}

/* ---- ノイズ/LFO(L1749-1835相当) ---- */

static int32_t clock_noise_and_lfo(ym2151_t *c) {
    uint32_t freq = reg_noise_frequency(c);
    for (int rep = 0; rep < 2; rep++) {
        c->noise_lfsr <<= 1;
        c->noise_lfsr |= bitfield(c->noise_lfsr, 17, 1) ^ bitfield(c->noise_lfsr, 14, 1) ^ 1u;
        c->noise_counter = (uint8_t)(c->noise_counter + 1);
        if (c->noise_counter >= freq) {
            c->noise_counter = 0;
            c->noise_state = (uint8_t)bitfield(c->noise_lfsr, 17, 1);
        }
    }

    uint32_t rate = reg_lfo_rate(c);
    c->lfo_counter += (0x10u | bitfield(rate, 0, 4)) << bitfield(rate, 4, 4);
    if (reg_lfo_reset(c) != 0)
        c->lfo_counter = 0;

    uint32_t lfo = bitfield(c->lfo_counter, 22, 8);
    uint32_t lfo_noise = bitfield(c->noise_lfsr, 17, 8);
    c->lfo_waveform[LFO_WAVEFORM_LENGTH * 3 + ((lfo + 1) & 0xffu)] =
        (int16_t)(lfo_noise | (lfo_noise << 8));

    int32_t ampm = c->lfo_waveform[LFO_WAVEFORM_LENGTH * reg_lfo_waveform(c) + lfo];
    c->lfo_am = (uint8_t)(((ampm & 0xff) * (int32_t)reg_lfo_am_depth(c)) >> 7);
    return ((ampm >> 8) * (int32_t)reg_lfo_pm_depth(c)) >> 7;
}

/* ---- 公開API ---- */

void ym2151_reset(ym2151_t *c) {
    memset(c, 0, sizeof(*c));

    c->active_channels = ALL_CHANNELS;
    c->modified_channels = ALL_CHANNELS;

    /* チャンネル/オペレータの割り当て(コンストラクタ相当、L1289-1310)。
     * OPMはchannel_offset/operator_offsetとも恒等写像。 */
    for (int chnum = 0; chnum < YM2151_CHANNELS; chnum++) {
        c->channel[chnum].choffs = chnum;
        for (int index = 0; index < 4; index++) {
            uint32_t opnum = bitfield(s_fixed_map[chnum], 8 * index, 8);
            c->channel[chnum].op[index] = &c->operators[opnum];
            c->operators[opnum].choffs = chnum;
            c->operators[opnum].opoffs = (int)opnum;
        }
    }

    /* オペレータ初期状態(fm_operatorコンストラクタ、L656-665相当) */
    for (int opnum = 0; opnum < YM2151_OPERATORS; opnum++) {
        c->operators[opnum].env_attenuation = 0x3ffu;
        c->operators[opnum].env_state = EG_RELEASE;
    }

    c->noise_lfsr = 1;

    /* 正弦波テーブル(m_waveform、L1326-1329相当)。OPMは波形1種のみ。 */
    for (uint32_t index = 0; index < WAVEFORM_LENGTH; index++)
        c->waveform[index] =
            (uint16_t)(abs_sin_attenuation(index) | (bitfield(index, 9, 1) << 15));

    /* LFO波形テーブル(m_lfo_waveform、L1333-1351相当)。0=鋸波,1=矩形波,
     * 2=三角波,3=ノイズ(動的に埋まる、ここでは0初期化のみ)。ビット
     * パターンの合成のみが目的のため、符号付き左シフトのUBを避けて
     * 全てunsigned演算で行い、最後にint16_tへ再解釈する。 */
    for (uint32_t index = 0; index < LFO_WAVEFORM_LENGTH; index++) {
        uint32_t am, pm;

        am = index ^ 0xffu;
        pm = index & 0xffu;
        c->lfo_waveform[LFO_WAVEFORM_LENGTH * 0 + index] = (int16_t)(am | (pm << 8));

        am = (bitfield(index, 7, 1) != 0) ? 0u : 0xffu;
        pm = am ^ 0x80u;
        c->lfo_waveform[LFO_WAVEFORM_LENGTH * 1 + index] = (int16_t)(am | (pm << 8));

        am = (bitfield(index, 7, 1) != 0) ? ((index << 1) & 0xffu)
                                          : (((index ^ 0xffu) << 1) & 0xffu);
        pm = (bitfield(index, 6, 1) != 0) ? am : (am ^ 0xffu);
        c->lfo_waveform[LFO_WAVEFORM_LENGTH * 2 + index] = (int16_t)(am | (pm << 8));

        c->lfo_waveform[LFO_WAVEFORM_LENGTH * 3 + index] = 0;
    }

    /* 0x20-0x27=0xc0でL/R出力を有効にする。REG_MODEはmemsetの0でよい。 */
    c->regdata[0x20] = c->regdata[0x21] = c->regdata[0x22] = c->regdata[0x23] = 0xc0u;
    c->regdata[0x24] = c->regdata[0x25] = c->regdata[0x26] = c->regdata[0x27] = 0xc0u;
}

/* $14(REG_MODE)書き込み(engine_mode_write, L1648-1676相当)。FRESET(bit4/5)で
 * ステータスフラグをクリアし、LOAD(bit0/1)のエッジトリガでタイマの
 * start/stopを行う。running中に再度LOADを立てても(既に動作中のため)
 * リロードされない(update_timer, L1690-1719)。 */
static void engine_mode_write(ym2151_t *c, uint8_t data) {
    c->regdata[REG_MODE] = data;

    uint8_t reset_mask = 0;
    if (reg_reset_timer_b(c) != 0)
        reset_mask |= STATUS_TIMERB;
    if (reg_reset_timer_a(c) != 0)
        reset_mask |= STATUS_TIMERA;
    c->status = (uint8_t)(c->status & (uint8_t)~reset_mask);

    if (reg_load_timer_a(c) != 0) {
        if (!c->timer_a_running) {
            c->timer_a_counter = timer_a_period(c);
            c->timer_a_running = 1;
        }
    } else {
        c->timer_a_running = 0;
    }
    if (reg_load_timer_b(c) != 0) {
        if (!c->timer_b_running) {
            c->timer_b_counter = timer_b_period(c);
            c->timer_b_running = 1;
        }
    } else {
        c->timer_b_running = 0;
    }
}

void ym2151_write(ym2151_t *c, uint8_t addr, uint8_t data) {
    c->address = addr;

    if (addr == REG_MODE) {
        engine_mode_write(c, data);
        return;
    }

    c->modified_channels = ALL_CHANNELS;

    /* regs_write相当(L1739-1747): 0x19書き込みはbit7でPM深度用の隣接
     * レジスタ0x1aへリダイレクトされる。0x1aへの直接書き込みは無視。
     */
    if (addr == 0x19)
        c->regdata[addr + bitfield(data, 7, 1)] = data;
    else if (addr != 0x1a)
        c->regdata[addr] = data;

    /* KEYON(0x08)書き込みの即時処理(L1446-1454) */
    if (addr == 0x08) {
        uint32_t keyon_channel = bitfield(data, 0, 3);
        uint32_t keyon_opmask = bitfield(data, 3, 4);
        if (keyon_channel < YM2151_CHANNELS)
            channel_keyonoff(&c->channel[keyon_channel], keyon_opmask, KEYON_NORMAL);
    }

    /* CT1/CT2はadpcm.cが同じ書き込み値を独立に追跡する。BUSY状態は
     * peripheral/opm.cが管理するため、このコアでは処理しない。 */
}

void ym2151_generate(ym2151_t *c, int32_t *buf, int frames) {
    for (int i = 0; i < frames; i++) {
        c->total_clocks = (uint8_t)(c->total_clocks + 1);

        if (c->modified_channels != 0 || c->prepare_count++ >= 4096u) {
            c->active_channels = 0;
            for (int chnum = 0; chnum < YM2151_CHANNELS; chnum++) {
                if (channel_prepare(c, &c->channel[chnum]))
                    c->active_channels |= (1u << chnum);
            }
            c->modified_channels = 0;
            c->prepare_count = 0;
        }

        /* EG_CLOCK_DIVIDER=3固定(OPM)。Java版のEG_CLOCK_DIVIDER==1分岐
         * (常に不成立)はOPM専用移植のため省略。 */
        c->env_counter++;
        if (bitfield(c->env_counter, 0, 2) == 3u)
            c->env_counter += 1u; /* 4 - EG_CLOCK_DIVIDER(=3) */

        int32_t lfo_raw_pm = clock_noise_and_lfo(c);

        for (int chnum = 0; chnum < YM2151_CHANNELS; chnum++)
            channel_clock(c, &c->channel[chnum], c->env_counter, lfo_raw_pm);

        int32_t out_l = 0, out_r = 0;
        uint32_t chanmask = ALL_CHANNELS & c->active_channels;
        for (int chnum = 0; chnum < YM2151_CHANNELS; chnum++) {
            if (bitfield(chanmask, chnum, 1) != 0)
                channel_output_4op(c, &c->channel[chnum], 0, 32767, &out_l, &out_r);
        }

        buf[i * 2 + 0] = roundtrip_fp(out_l);
        buf[i * 2 + 1] = roundtrip_fp(out_r);
    }
}

uint8_t ym2151_status(const ym2151_t *c) {
    return (uint8_t)(c->status & (STATUS_TIMERA | STATUS_TIMERB));
}
