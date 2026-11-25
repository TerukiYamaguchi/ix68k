#include "bus/guest_clock.h"
#include <assert.h>
#include <time.h>
#ifdef _WIN32
#include "platform/win_compat.h"
#endif

static uint64_t host_mono_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

void guest_clock_init(guest_clock_t *clock) {
    clock->shift_us = 0;
    clock->paused = false;
    clock->paused_at_host_us = 0;
    clock->wall_lag_sec = 0;
    clock->frozen_unix_sec = 0;
}

uint64_t guest_clock_mono_us(const guest_clock_t *clock) {
    uint64_t host_us = clock->paused ? clock->paused_at_host_us : host_mono_us();
    return (uint64_t)((int64_t)host_us + clock->shift_us);
}

int64_t guest_clock_unix_sec(const guest_clock_t *clock) {
    if (clock->paused)
        return clock->frozen_unix_sec;
    return (int64_t)time(NULL) - clock->wall_lag_sec;
}

void guest_clock_set_paused(guest_clock_t *clock, bool paused) {
    if (clock->paused == paused)
        return;
    if (paused) {
        clock->paused_at_host_us = host_mono_us();
        clock->frozen_unix_sec = (int64_t)time(NULL) - clock->wall_lag_sec;
        clock->paused = true;
        return;
    }
    clock->shift_us -= (int64_t)(host_mono_us() - clock->paused_at_host_us);
    clock->wall_lag_sec = (int64_t)time(NULL) - clock->frozen_unix_sec;
    clock->paused = false;
}

void guest_clock_rebase_paused(guest_clock_t *clock, uint64_t target_mono_us) {
    assert(clock->paused);
    clock->paused_at_host_us = host_mono_us();
    clock->shift_us = (int64_t)target_mono_us - (int64_t)clock->paused_at_host_us;
}
