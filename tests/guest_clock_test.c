#include "bus/bus_internal.h"
#include <assert.h>
#include <stdio.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define PAUSE_WAIT_MS 200
#define RESUME_WAIT_MS 20
#define RESUME_TOLERANCE_US 20000u
#define REBASE_TARGET_US 123456789u

static void sleep_ms(unsigned ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts = {.tv_sec = ms / 1000u, .tv_nsec = (long)(ms % 1000u) * 1000000L};
    nanosleep(&ts, NULL);
#endif
}

static void test_clock_freeze_and_resume(void) {
    guest_clock_t clock;
    guest_clock_init(&clock);
    assert(guest_clock_unix_sec(&clock) == (int64_t)time(NULL));

    guest_clock_set_paused(&clock, true);
    uint64_t frozen_mono = guest_clock_mono_us(&clock);
    int64_t frozen_unix = guest_clock_unix_sec(&clock);
    guest_clock_set_paused(&clock, true);
    sleep_ms(PAUSE_WAIT_MS);
    assert(guest_clock_mono_us(&clock) == frozen_mono);
    assert(guest_clock_unix_sec(&clock) == frozen_unix);

    guest_clock_set_paused(&clock, false);
    uint64_t resumed_mono = guest_clock_mono_us(&clock);
    assert(resumed_mono >= frozen_mono);
    assert(resumed_mono - frozen_mono < RESUME_TOLERANCE_US);
    sleep_ms(RESUME_WAIT_MS);
    assert(guest_clock_mono_us(&clock) > resumed_mono);
}

static void test_clock_rebase(void) {
    guest_clock_t clock;
    guest_clock_init(&clock);
    guest_clock_set_paused(&clock, true);
    guest_clock_rebase_paused(&clock, REBASE_TARGET_US);
    sleep_ms(PAUSE_WAIT_MS);
    assert(guest_clock_mono_us(&clock) == REBASE_TARGET_US);
    guest_clock_set_paused(&clock, false);
    assert(guest_clock_mono_us(&clock) >= REBASE_TARGET_US);
}

static void test_rtc_freeze_and_rebase(void) {
    x68k_bus_t *bus = x68k_bus_create();
    assert(bus != NULL);

    int64_t saved_now = rtc_now(&bus->rtc);
    bool saved_phase = rtc_alarm_low(&bus->rtc);
    x68k_bus_guest_clock_set_paused(bus, 1);
    sleep_ms(PAUSE_WAIT_MS);
    assert(rtc_now(&bus->rtc) == saved_now);
    assert(rtc_alarm_low(&bus->rtc) == saved_phase);

    rtc_set(&bus->rtc, 1700000000);
    saved_now = rtc_now(&bus->rtc);
    sleep_ms(PAUSE_WAIT_MS);
    rtc_snapshot_rebase(&bus->rtc, saved_now);
    assert(rtc_now(&bus->rtc) == saved_now);

    bus->rtc.test = 1;
    int64_t saved_offset = bus->rtc.offset_sec;
    rtc_snapshot_rebase(&bus->rtc, saved_now);
    assert(bus->rtc.offset_sec == saved_offset);
    x68k_bus_destroy(bus);
}

int main(void) {
    test_clock_freeze_and_resume();
    test_clock_rebase();
    test_rtc_freeze_and_rebase();
    puts("guest_clock_test: OK");
    return 0;
}
