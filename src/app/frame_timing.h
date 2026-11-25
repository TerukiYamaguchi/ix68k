#pragma once

#include <stdint.h>

/* 1秒当たりunits_per_second個ある単位を、有理数のフレーム周期へ誤差なく配分する。
 * period_numerator / period_denominator は秒単位の周期である。CPUサイクル、音声サンプル、
 * ホストのマイクロ秒deadlineに同じ型を使い、端数をフレーム間で累積する。 */
typedef struct {
    uint64_t units_per_second;
    uint64_t period_numerator;
    uint64_t period_denominator;
    uint64_t remainder;
} frame_cycle_clock_t;

static inline void frame_cycle_clock_set_period(frame_cycle_clock_t *clock,
                                                uint64_t units_per_second,
                                                uint64_t period_numerator,
                                                uint64_t period_denominator) {
    if (clock->units_per_second == units_per_second &&
        clock->period_numerator == period_numerator &&
        clock->period_denominator == period_denominator)
        return;
    /* 端数は旧分母に対する位相なので、新分母へ比例変換して引き継ぐ。
     * ix68kで使うCRTC分母は70MHz未満であり、この積はuint64_tに収まる。 */
    if (clock->units_per_second == units_per_second && clock->period_denominator != 0 &&
        clock->remainder <= UINT64_MAX / period_denominator)
        clock->remainder = clock->remainder * period_denominator / clock->period_denominator;
    else
        clock->remainder = 0;
    clock->units_per_second = units_per_second;
    clock->period_numerator = period_numerator;
    clock->period_denominator = period_denominator;
}

static inline uint32_t frame_cycle_clock_next(frame_cycle_clock_t *clock) {
    uint64_t numerator = clock->units_per_second * clock->period_numerator + clock->remainder;
    uint64_t units = numerator / clock->period_denominator;
    clock->remainder = numerator % clock->period_denominator;
    return (uint32_t)units;
}

/* 描画済みなら描画負荷が原因になり得るためhard overloadではない。
 * 描画を省略した論理フレームでもdeadlineを逃した場合だけ、CPU／周辺／音声
 * 自体が実時間に追いつかない適用限界として扱う。 */
static inline int frame_timing_is_hard_overload(int rendered, uint64_t now_us,
                                                uint64_t deadline_us) {
    return !rendered && now_us > deadline_us;
}
