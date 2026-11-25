#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int64_t shift_us;
    bool paused;
    uint64_t paused_at_host_us;
    int64_t wall_lag_sec;
    int64_t frozen_unix_sec;
} guest_clock_t;

void guest_clock_init(guest_clock_t *clock);
uint64_t guest_clock_mono_us(const guest_clock_t *clock);
int64_t guest_clock_unix_sec(const guest_clock_t *clock);
void guest_clock_set_paused(guest_clock_t *clock, bool paused);
void guest_clock_rebase_paused(guest_clock_t *clock, uint64_t target_mono_us);
