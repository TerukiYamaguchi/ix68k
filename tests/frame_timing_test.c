#include "app/frame_timing.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void test_exact_one_second(uint32_t cpu_hz) {
    frame_cycle_clock_t clock;
    memset(&clock, 0, sizeof(clock));
    frame_cycle_clock_set_period(&clock, cpu_hz, 1, 60);

    uint64_t total = 0;
    uint32_t base = cpu_hz / 60;
    for (int i = 0; i < 60; i++) {
        uint32_t cycles = frame_cycle_clock_next(&clock);
        assert(cycles == base || cycles == base + 1);
        total += cycles;
    }
    assert(total == cpu_hz);
    assert(clock.remainder == 0);
}

static void test_long_run(uint32_t cpu_hz) {
    enum { SECONDS = 3600 };
    frame_cycle_clock_t clock;
    memset(&clock, 0, sizeof(clock));
    frame_cycle_clock_set_period(&clock, cpu_hz, 1, 60);

    uint64_t total = 0;
    for (int i = 0; i < 60 * SECONDS; i++)
        total += frame_cycle_clock_next(&clock);
    assert(total == (uint64_t)cpu_hz * SECONDS);
}

static void test_crtc_rate(uint32_t units_per_second, uint64_t period_numerator,
                           uint64_t period_denominator, uint64_t frames) {
    frame_cycle_clock_t clock = {0};
    frame_cycle_clock_set_period(&clock, units_per_second, period_numerator, period_denominator);
    uint64_t total = 0;
    for (uint64_t i = 0; i < frames; i++)
        total += frame_cycle_clock_next(&clock);
    assert(total == units_per_second * period_numerator * frames / period_denominator);
}

static void test_period_change_preserves_phase(void) {
    frame_cycle_clock_t clock = {0};
    frame_cycle_clock_set_period(&clock, 10, 1, 3);
    assert(frame_cycle_clock_next(&clock) == 3);
    assert(clock.remainder == 1);
    frame_cycle_clock_set_period(&clock, 10, 1, 7);
    assert(clock.remainder == 2); /* 旧位相1/3を新分母7へ再正規化 */
    assert(frame_cycle_clock_next(&clock) == 1);
}

int main(void) {
    const uint32_t clocks[] = {10000000, 16000000, 25000000, 33000000, 50000000};
    for (unsigned i = 0; i < sizeof(clocks) / sizeof(clocks[0]); i++) {
        test_exact_one_second(clocks[i]);
        test_long_run(clocks[i]);
    }
    /* CRTMOD17相当: 16 * 176 * 465 / 69,551,990 秒。 */
    test_crtc_rate(10000000u, 16u * 176u * 465u, 69551990u, 100000u);
    test_period_change_preserves_phase();
    assert(frame_timing_is_hard_overload(1, 101, 100) == 0);
    assert(frame_timing_is_hard_overload(0, 100, 100) == 0);
    assert(frame_timing_is_hard_overload(0, 101, 100) == 1);
    printf("frame_timing_test: OK\n");
    return 0;
}
