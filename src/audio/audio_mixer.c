#include "audio/audio_mixer.h"

/* phase は Q16.16 固定小数点（入力サンプル単位）。振幅側(int32)は最大でも
 * OPM生成値(14bit相当)程度のため、frac(0..65535)を掛けても int64_t で
 * 十分収まる。 */
#define FRAC_BITS 16
#define FRAC_ONE  (1u << FRAC_BITS)
#define FRAC_MASK (FRAC_ONE - 1u)

static int32_t interp_linear(int32_t p0, int32_t p1, uint32_t frac) {
    return p0 + (int32_t)(((int64_t)(p1 - p0) * (int64_t)frac) >> FRAC_BITS);
}

/* Catmull-Rom 3次補間。p_1,p0,p1,p2 の4点からp0-p1間をfrac(Q16.16)で補間する。
 * 固定小数点で係数を手で導出すると誤りやすいため、標準的な浮動小数点の式を
 * そのまま使う（1出力サンプルあたり数FLOPで、リアルタイム制約上は無視できる
 * コスト）。 */
static int32_t interp_cubic(int32_t pm1, int32_t p0, int32_t p1, int32_t p2, uint32_t frac) {
    double t = (double)frac / (double)FRAC_ONE;
    double a0 = -0.5 * pm1 + 1.5 * p0 - 1.5 * p1 + 0.5 * p2;
    double a1 = (double)pm1 - 2.5 * p0 + 2.0 * p1 - 0.5 * p2;
    double a2 = -0.5 * pm1 + 0.5 * p1;
    double a3 = p0;
    double result = ((a0 * t + a1) * t + a2) * t + a3;
    return (int32_t)(result + (result >= 0.0 ? 0.5 : -0.5));
}

/* (idx, frac, mode) から1チャンネル分の補間済みサンプルを求める（ステレオ
 * interleaved用、stride=2）。prev: CUBICがidx==0で参照するp(-1)
 * （前コール最後のサンプル）。
 * in_valid_count: in[]バッファ中、実際に読み出して安全な要素数（呼び出し側の
 * バッファ管理の都合上、位相の停止境界(in_count)より後ろにも先読み用の
 * 有効データが続いていることがある。無ければin_countと同じ値を渡す）。 */
static int32_t resample_sample(const int32_t *prev, const int32_t *in, int in_valid_count,
                               int stride, int idx, uint32_t frac, audio_resample_mode_t mode) {
    int32_t p0 = in[idx * stride];
    if (mode == AUDIO_RESAMPLE_NEAREST)
        return p0;
    int32_t p1 =
        (idx + 1 < in_valid_count) ? in[(idx + 1) * stride] : in[(in_valid_count - 1) * stride];
    if (mode == AUDIO_RESAMPLE_LINEAR)
        return interp_linear(p0, p1, frac);
    int32_t pm1 = (idx > 0) ? in[(idx - 1) * stride] : *prev;
    int32_t p2 =
        (idx + 2 < in_valid_count) ? in[(idx + 2) * stride] : in[(in_valid_count - 1) * stride];
    return interp_cubic(pm1, p0, p1, p2, frac);
}

int audio_mix_resample_pan(const int16_t *in, int in_count, uint32_t native_rate_hz,
                           uint32_t out_rate_hz, int left_on, int right_on, int16_t *out,
                           int out_capacity_frames, audio_resample_mode_t mode,
                           audio_resample_state_t *state) {
    if (in_count <= 0 || native_rate_hz == 0u || out_rate_hz == 0u)
        return 0;

    int16_t l_gain = left_on ? (int16_t)1 : (int16_t)0;
    int16_t r_gain = right_on ? (int16_t)1 : (int16_t)0;

    uint64_t step = ((uint64_t)native_rate_hz << FRAC_BITS) / out_rate_hz;
    uint64_t phase = state->phase;
    int32_t prev = state->have_prev ? state->prev[0] : (int32_t)in[0];

    int frames = 0;
    while (frames < out_capacity_frames) {
        int idx = (int)(phase >> FRAC_BITS);
        if (idx >= in_count)
            break;
        uint32_t frac = (uint32_t)(phase & FRAC_MASK);

        int32_t p0 = in[idx];
        int32_t sample;
        if (mode == AUDIO_RESAMPLE_NEAREST) {
            sample = p0;
        } else {
            int32_t p1 = (idx + 1 < in_count) ? in[idx + 1] : in[in_count - 1];
            if (mode == AUDIO_RESAMPLE_LINEAR) {
                sample = interp_linear(p0, p1, frac);
            } else {
                int32_t pm1 = (idx > 0) ? in[idx - 1] : prev;
                int32_t p2 = (idx + 2 < in_count) ? in[idx + 2] : in[in_count - 1];
                sample = interp_cubic(pm1, p0, p1, p2, frac);
            }
        }
        out[frames * 2 + 0] = (int16_t)(sample * l_gain);
        out[frames * 2 + 1] = (int16_t)(sample * r_gain);
        frames++;
        phase += step;
    }

    /* 消費し切った(idx>=in_count に達した)ときだけ、次コールの先頭からの
     * 相対位置へ位相を巻き戻す。out_capacity_framesで頭打ちして未消費の
     * 入力を残したまま抜けた場合は巻き戻さない（次コールは新しいバッファに
     * なるため、その状態を引きずっても意味がない。実際の呼び出し元は
     * capacityに十分な余裕を持たせているため通常発生しない）。 */
    if ((phase >> FRAC_BITS) >= (uint64_t)in_count)
        phase -= (uint64_t)in_count << FRAC_BITS;
    state->phase = phase;
    state->prev[0] = in[in_count - 1];
    state->have_prev = 1;
    return frames;
}

int audio_resample_mono(const int16_t *in, int in_count, uint32_t in_rate_hz,
                        uint32_t out_rate_hz, int16_t *out, int out_capacity,
                        audio_resample_mode_t mode, audio_resample_state_t *state) {
    if (in_count <= 0 || in_rate_hz == 0u || out_rate_hz == 0u)
        return 0;

    uint64_t step = ((uint64_t)in_rate_hz << FRAC_BITS) / out_rate_hz;
    uint64_t phase = state->phase;
    int32_t prev = state->have_prev ? state->prev[0] : (int32_t)in[0];

    int n = 0;
    while (n < out_capacity) {
        int idx = (int)(phase >> FRAC_BITS);
        if (idx >= in_count)
            break;
        uint32_t frac = (uint32_t)(phase & FRAC_MASK);

        int32_t p0 = in[idx];
        int32_t sample;
        if (mode == AUDIO_RESAMPLE_NEAREST) {
            sample = p0;
        } else {
            int32_t p1 = (idx + 1 < in_count) ? in[idx + 1] : in[in_count - 1];
            if (mode == AUDIO_RESAMPLE_LINEAR) {
                sample = interp_linear(p0, p1, frac);
            } else {
                int32_t pm1 = (idx > 0) ? in[idx - 1] : prev;
                int32_t p2 = (idx + 2 < in_count) ? in[idx + 2] : in[in_count - 1];
                sample = interp_cubic(pm1, p0, p1, p2, frac);
            }
        }
        out[n++] = (int16_t)sample;
        phase += step;
    }

    if ((phase >> FRAC_BITS) >= (uint64_t)in_count)
        phase -= (uint64_t)in_count << FRAC_BITS;
    state->phase = phase;
    state->prev[0] = in[in_count - 1];
    state->have_prev = 1;
    return n;
}

int audio_resample_stereo_i32(const int32_t *in, int in_frames, int in_valid_frames,
                              uint32_t in_rate_hz, uint32_t out_rate_hz, int32_t *out,
                              int out_capacity_frames, audio_resample_mode_t mode,
                              audio_resample_state_t *state) {
    if (in_frames <= 0 || in_rate_hz == 0u || out_rate_hz == 0u)
        return 0;
    if (in_valid_frames < in_frames)
        in_valid_frames = in_frames;

    uint64_t step = ((uint64_t)in_rate_hz << FRAC_BITS) / out_rate_hz;
    uint64_t phase = state->phase;
    int32_t prev_l = state->have_prev ? state->prev[0] : in[0];
    int32_t prev_r = state->have_prev ? state->prev[1] : in[1];

    int frames = 0;
    while (frames < out_capacity_frames) {
        int idx = (int)(phase >> FRAC_BITS);
        if (idx >= in_frames)
            break;
        uint32_t frac = (uint32_t)(phase & FRAC_MASK);

        out[frames * 2 + 0] =
            resample_sample(&prev_l, in + 0, in_valid_frames, 2, idx, frac, mode);
        out[frames * 2 + 1] =
            resample_sample(&prev_r, in + 1, in_valid_frames, 2, idx, frac, mode);
        frames++;
        phase += step;
    }

    if ((phase >> FRAC_BITS) >= (uint64_t)in_frames)
        phase -= (uint64_t)in_frames << FRAC_BITS;
    state->phase = phase;
    state->prev[0] = in[(in_frames - 1) * 2 + 0];
    state->prev[1] = in[(in_frames - 1) * 2 + 1];
    state->have_prev = 1;
    return frames;
}

int audio_mix_resample_pan_i32(const int16_t *in, int in_count, int in_valid_count,
                               uint32_t native_rate_hz, uint32_t out_rate_hz, int left_on,
                               int right_on, int32_t *out, int out_capacity_frames,
                               audio_resample_mode_t mode, audio_resample_state_t *state) {
    if (in_count <= 0 || native_rate_hz == 0u || out_rate_hz == 0u)
        return 0;
    if (in_valid_count < in_count)
        in_valid_count = in_count;

    int32_t l_gain = left_on ? 1 : 0;
    int32_t r_gain = right_on ? 1 : 0;

    uint64_t step = ((uint64_t)native_rate_hz << FRAC_BITS) / out_rate_hz;
    uint64_t phase = state->phase;
    int32_t prev = state->have_prev ? state->prev[0] : (int32_t)in[0];

    int frames = 0;
    while (frames < out_capacity_frames) {
        int idx = (int)(phase >> FRAC_BITS);
        if (idx >= in_count)
            break;
        uint32_t frac = (uint32_t)(phase & FRAC_MASK);

        int32_t p0 = in[idx];
        int32_t sample;
        if (mode == AUDIO_RESAMPLE_NEAREST) {
            sample = p0;
        } else {
            int32_t p1 = (idx + 1 < in_valid_count) ? in[idx + 1] : in[in_valid_count - 1];
            if (mode == AUDIO_RESAMPLE_LINEAR) {
                sample = interp_linear(p0, p1, frac);
            } else {
                int32_t pm1 = (idx > 0) ? in[idx - 1] : prev;
                int32_t p2 = (idx + 2 < in_valid_count) ? in[idx + 2] : in[in_valid_count - 1];
                sample = interp_cubic(pm1, p0, p1, p2, frac);
            }
        }
        out[frames * 2 + 0] = sample * l_gain;
        out[frames * 2 + 1] = sample * r_gain;
        frames++;
        phase += step;
    }

    if ((phase >> FRAC_BITS) >= (uint64_t)in_count)
        phase -= (uint64_t)in_count << FRAC_BITS;
    state->phase = phase;
    state->prev[0] = in[in_count - 1];
    state->have_prev = 1;
    return frames;
}

static inline int16_t clip16(int32_t v) {
    if (v < -32768)
        return -32768;
    if (v > 32767)
        return 32767;
    return (int16_t)v;
}

/* マスターボリュームスケール(Q12固定小数点)。XEiJ SoundSource.javaの既定
 * sndTargetScale(SND_VOLUME_DEFAULT時、2^(12-1)=2048=0.5倍)に合わせた
 * ヘッドルーム。OPM(最大±32767)とADPCM(最大±32767)が同時にフルスケール
 * でも 0.5 倍後は ±32767 に収まり、不要なハードクリップを抑えられる。 */
#define MASTER_SCALE_Q12   2048
#define MASTER_SCALE_SHIFT 12

int audio_mix_sum_clip_stereo(const int32_t *src_a, const int32_t *src_b, int frames,
                              int16_t *out) {
    for (int i = 0; i < frames; i++) {
        int32_t sum_l = src_a[i * 2 + 0] + src_b[i * 2 + 0];
        int32_t sum_r = src_a[i * 2 + 1] + src_b[i * 2 + 1];
        out[i * 2 + 0] = clip16((sum_l * MASTER_SCALE_Q12) >> MASTER_SCALE_SHIFT);
        out[i * 2 + 1] = clip16((sum_r * MASTER_SCALE_Q12) >> MASTER_SCALE_SHIFT);
    }
    return frames;
}

/* R=0.999 の Q16 固定小数点表現。fc=(1-R)*fs/(2π)≈7Hz @44100Hz。 */
#define DC_BLOCK_R_Q16 65470

void audio_dc_block_stereo(int32_t *buf, int frames, audio_dc_block_state_t *state) {
    for (int i = 0; i < frames; i++) {
        for (int ch = 0; ch < 2; ch++) {
            int32_t x = buf[i * 2 + ch];
            int64_t diff_q16 = (int64_t)(x - state->x1[ch]) << 16;
            int64_t feedback_q16 = (DC_BLOCK_R_Q16 * state->y1_q16[ch]) >> 16;
            int64_t y_q16 = diff_q16 + feedback_q16;
            buf[i * 2 + ch] = (int32_t)(y_q16 >> 16);
            state->x1[ch] = x;
            state->y1_q16[ch] = y_q16;
        }
    }
}
