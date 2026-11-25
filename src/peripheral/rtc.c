#include "peripheral/rtc.h"
#include <string.h>

/* BANK0/BANK1共通のレジスタオフセット（addr&0x1F、奇数のみ）。 */
enum {
    RTC_A_SEC1 = 0x01,
    RTC_A_SEC10 = 0x03,
    RTC_A_MIN1 = 0x05,
    RTC_A_MIN10 = 0x07,
    RTC_A_HOUR1 = 0x09,
    RTC_A_HOUR10 = 0x0B,
    RTC_A_WDAY = 0x0D,
    RTC_A_DAY1 = 0x0F,
    RTC_A_DAY10 = 0x11,
    RTC_A_MON1 = 0x13,
    RTC_A_MON10 = 0x15,
    RTC_A_YEAR1 = 0x17,
    RTC_A_YEAR10 = 0x19,
    RTC_A_MODE = 0x1B,
    RTC_A_TEST = 0x1D,
    RTC_A_RESET = 0x1F,
};

/* カウンタレジスタ(0x01-0x19)のニブル幅マスク。index = (off-1)>>1。
 * 『Inside X68000』RTC章・RP5C15データシート準拠。 */
static const uint8_t k_counter_mask[13] = {
    0x0F, /* SEC1 */
    0x07, /* SEC10 (0-5) */
    0x0F, /* MIN1 */
    0x07, /* MIN10 (0-5) */
    0x0F, /* HOUR1 */
    0x03, /* HOUR10 (0-2, 24h) */
    0x07, /* WDAY (0-6) */
    0x0F, /* DAY1 */
    0x03, /* DAY10 (0-3) */
    0x0F, /* MON1 */
    0x01, /* MON10 (0-1) */
    0x0F, /* YEAR1 */
    0x0F, /* YEAR10 */
};

static uint8_t counter_mask(uint32_t off) {
    return k_counter_mask[(off - 1u) >> 1];
}

static uint8_t bank1_mask(uint32_t off) {
    switch (off) {
    case 0x01:
        return 0x07; /* CLKOUT */
    case 0x05:
    case 0x09:
    case 0x0F:
        return 0x0F;
    case 0x07:
        return 0x07;
    case 0x0B:
    case 0x11:
        return 0x03;
    case 0x0D:
        return 0x07;
    case 0x15:
        return 0x01; /* 12/24 */
    case 0x17:
        return 0x03; /* leap */
    default:
        return 0x00;
    }
}

static uint64_t monotonic_us(const rp5c15_t *r) {
    return guest_clock_mono_us(r->clock);
}

static void rtc_sync(rp5c15_t *r);

static unsigned alarm_bit(uint32_t off) {
    return (unsigned)((off - 0x05u) >> 1);
}

bool rtc_alarm_low(rp5c15_t *r) {
    rtc_sync(r);
    bool match = true;
    for (uint32_t off = 0x05; off <= 0x11; off += 2) {
        unsigned bit = alarm_bit(off);
        if ((r->alarm_written_mask & (1u << bit)) && r->bank0[off] != r->bank1[off]) {
            match = false;
            break;
        }
    }
    bool alarm = (r->mode & RTC_MODE_ALARM) && match;
    uint64_t phase = monotonic_us(r);
    bool one_hz = !(r->reset_ctl & 0x08u) && (phase % 1000000u) >= 500000u;
    bool sixteen_hz = !(r->reset_ctl & 0x04u) && (phase % 62500u) >= 31250u;
    return alarm || one_hz || sixteen_hz;
}

void rtc_init(rp5c15_t *r, const guest_clock_t *clock) {
    memset(r, 0, sizeof(*r));
    r->clock = clock;
    r->mode = RTC_MODE_FREE;    /* 既定FREE。IPLROMが起動時に$E8A01Bへ改めて書くため
                                    実害は無いが、起動直後から素直に現在時刻を返す。 */
    r->snap_epoch = (time_t)-1; /* 強制的に初回rtc_sync()で再構築させる */
    r->bank1[0x15] = 1;         /* ix68kはホスト時計を24時間表示で初期化する。 */
    r->reset_ctl = 0x0Cu;       /* 1Hz/16Hzを初期抑止（bit3/2のみ保持）。 */
}

static double test_rate(uint8_t test) {
    static const int64_t unit_sec[4] = {1, 60, 86400, 31557600};
    int64_t sum = 0;
    int64_t scale = 0;
    for (unsigned bit = 0; bit < 4; bit++) {
        if (test & (1u << bit)) {
            sum += unit_sec[bit];
            if (!scale)
                scale = unit_sec[bit];
        }
    }
    /* XEiJ RTC_TEST_FREQ/SCALE と同じ、16384Hz入力当たりの更新間隔/単位。 */
    return test ? (double)scale * 16384.0 / (double)(sum + 1) : 1.0;
}

int64_t rtc_now(const rp5c15_t *r) {
    if (r->test) {
        uint64_t elapsed_us = monotonic_us(r) - r->test_anchor_us;
        return r->test_anchor_unix +
               (int64_t)((double)elapsed_us * test_rate(r->test) / 1000000.0);
    }
    return guest_clock_unix_sec(r->clock) + r->offset_sec;
}

void rtc_set(rp5c15_t *r, int64_t target_unix) {
    if (r->test) {
        r->test_anchor_unix = target_unix;
        r->test_anchor_us = monotonic_us(r);
        r->snap_epoch = (time_t)-1;
        return;
    }
    r->offset_sec = target_unix - guest_clock_unix_sec(r->clock);
}

void rtc_snapshot_rebase(rp5c15_t *r, int64_t saved_now) {
    if (!r->test)
        r->offset_sec = saved_now - guest_clock_unix_sec(r->clock);
}

/* bank0を time_t t 時点の値で再構築する。曜日は localtime() のtm_wdayに
 * weekday_offsetを足したもの（WDAY直接書き込みで生じたずれを保つ）。 */
static void snap_from_time(rp5c15_t *r, time_t t) {
    struct tm tmv = *localtime(&t);
    r->bank0[RTC_A_SEC1] = (uint8_t)(tmv.tm_sec % 10);
    r->bank0[RTC_A_SEC10] = (uint8_t)(tmv.tm_sec / 10);
    r->bank0[RTC_A_MIN1] = (uint8_t)(tmv.tm_min % 10);
    r->bank0[RTC_A_MIN10] = (uint8_t)(tmv.tm_min / 10);
    int display_hour = tmv.tm_hour;
    if (!(r->bank1[0x15] & 1u)) {
        int pm = display_hour >= 12;
        display_hour %= 12;
        r->bank0[RTC_A_HOUR1] = (uint8_t)(display_hour % 10);
        r->bank0[RTC_A_HOUR10] = (uint8_t)((display_hour / 10) | (pm ? 2 : 0));
    } else {
        r->bank0[RTC_A_HOUR1] = (uint8_t)(display_hour % 10);
        r->bank0[RTC_A_HOUR10] = (uint8_t)(display_hour / 10);
    }
    int wday = (tmv.tm_wday + r->weekday_offset) % 7;
    if (wday < 0)
        wday += 7;
    r->bank0[RTC_A_WDAY] = (uint8_t)wday;
    r->bank0[RTC_A_DAY1] = (uint8_t)(tmv.tm_mday % 10);
    r->bank0[RTC_A_DAY10] = (uint8_t)(tmv.tm_mday / 10);
    int mon = tmv.tm_mon + 1;
    r->bank0[RTC_A_MON1] = (uint8_t)(mon % 10);
    r->bank0[RTC_A_MON10] = (uint8_t)(mon / 10);
    /* 年は1980起点2桁（Human68k _DATESET/_DATEGET/_DATEBINと同じ規約、iocs.c参照）。 */
    int y2 = (tmv.tm_year + 1900) - 1980;
    if (y2 < 0)
        y2 = 0;
    y2 %= 100;
    r->bank0[RTC_A_YEAR1] = (uint8_t)(y2 % 10);
    r->bank0[RTC_A_YEAR10] = (uint8_t)(y2 / 10);
    r->snap_epoch = t;
}

/* bank0の秒-年フィールドから time_t を組み立てる（曜日は無視。mktime()自体が
 * 入力tm_wdayを使わず再計算するため、weekday_offsetとは独立に扱ってよい）。 */
static time_t snap_to_time(const rp5c15_t *r) {
    time_t now = (time_t)guest_clock_unix_sec(r->clock);
    struct tm t = *localtime(&now); /* tm_isdst等の地域情報を継承するための種 */
    t.tm_sec = r->bank0[RTC_A_SEC10] * 10 + r->bank0[RTC_A_SEC1];
    t.tm_min = r->bank0[RTC_A_MIN10] * 10 + r->bank0[RTC_A_MIN1];
    if (r->bank1[0x15] & 1u) {
        t.tm_hour = r->bank0[RTC_A_HOUR10] * 10 + r->bank0[RTC_A_HOUR1];
    } else {
        int hour = (r->bank0[RTC_A_HOUR10] & 1u) * 10 + r->bank0[RTC_A_HOUR1];
        t.tm_hour = hour + ((r->bank0[RTC_A_HOUR10] & 2u) ? 12 : 0);
    }
    t.tm_mday = r->bank0[RTC_A_DAY10] * 10 + r->bank0[RTC_A_DAY1];
    t.tm_mon = (r->bank0[RTC_A_MON10] * 10 + r->bank0[RTC_A_MON1]) - 1;
    t.tm_year = (r->bank0[RTC_A_YEAR10] * 10 + r->bank0[RTC_A_YEAR1]) + 1980 - 1900;
    t.tm_isdst = -1;
    return mktime(&t);
}

/* FREE中の読み出し前に呼ぶ。秒が変わっていなければ再構築を省く。HOLD中は
 * 何もしない（凍結中のbank0をそのまま読ませる）。 */
static void rtc_sync(rp5c15_t *r) {
    if (!(r->mode & RTC_MODE_FREE))
        return;
    time_t t = (time_t)rtc_now(r);
    if (t != r->snap_epoch)
        snap_from_time(r, t);
}

/* bank0の現在値をoffset_secへ書き戻す（HOLD中の編集をFREE復帰時に反映、
 * またはFREE中の即時書き込み）。 */
static void rtc_commit(rp5c15_t *r) {
    time_t target = snap_to_time(r);
    rtc_set(r, (int64_t)target);
    r->snap_epoch = (time_t)rtc_now(r);
}

uint8_t rtc_reg_read8(rp5c15_t *r, uint32_t addr) {
    uint32_t off = addr & 0x1Fu;
    if (off >= RTC_A_SEC1 && off <= RTC_A_YEAR10) {
        if (r->mode & RTC_MODE_BANK1) {
            if (off == 0x03 || off == 0x13 || off == 0x19)
                return 0x00u; /* ADJUST / reserved */
            return r->bank1[off] & bank1_mask(off);
        }
        rtc_sync(r);
        return r->bank0[off];
    }
    if (off == RTC_A_MODE)
        return r->mode;
    if (off == RTC_A_TEST || off == RTC_A_RESET)
        return 0x00u; /* write-only */
    return 0x00u;     /* 到達しない想定（呼び出し側が奇数addrのみ渡す） */
}

void rtc_reg_write8(rp5c15_t *r, uint32_t addr, uint8_t val) {
    uint32_t off = addr & 0x1Fu;
    if (off >= RTC_A_SEC1 && off <= RTC_A_YEAR10) {
        if (r->mode & RTC_MODE_BANK1) {
            if (off == 0x03) { /* ADJUST: 00-29sは同じ分、30-59sは次分へ丸める。 */
                if (val & 1u) {
                    rtc_sync(r);
                    time_t t = snap_to_time(r);
                    struct tm tmv = *localtime(&t);
                    if (tmv.tm_sec >= 30)
                        tmv.tm_min++;
                    tmv.tm_sec = 0;
                    tmv.tm_isdst = -1;
                    t = mktime(&tmv);
                    rtc_set(r, (int64_t)t);
                    snap_from_time(r, t);
                    if (!(r->mode & RTC_MODE_FREE))
                        r->snap_dirty = 1;
                }
                return;
            }
            uint8_t mask = bank1_mask(off);
            if (mask && !(off >= 0x05 && off <= 0x11 &&
                          monotonic_us(r) < r->alarm_write_inhibit_until_us)) {
                r->bank1[off] = (uint8_t)(val & mask);
                if (off >= 0x05 && off <= 0x11)
                    r->alarm_written_mask |= (uint8_t)(1u << alarm_bit(off));
            }
            return;
        }
        uint8_t masked = (uint8_t)(val & counter_mask(off));
        if (off == RTC_A_WDAY) {
            /* WDAYは日付から独立した7進カウンタ（実チップ・XEiJ rtcWeekGap準拠）。
             * 既存bank0[WDAY]とのずれをweekday_offsetへ
             * 積み、以後の自然な日送りでも書き込んだ値からの相対位置が保たれる。 */
            int old_wday = r->bank0[RTC_A_WDAY];
            r->weekday_offset = (r->weekday_offset + (int)masked - old_wday + 7) % 7;
        }
        r->bank0[off] = masked;
        if (r->mode & RTC_MODE_FREE)
            rtc_commit(r);
        else
            r->snap_dirty = 1;
        return;
    }
    if (off == RTC_A_MODE) {
        uint8_t new_mode = (uint8_t)(val & RTC_MODE_MASK);
        int was_free = (r->mode & RTC_MODE_FREE) != 0;
        int becomes_free = (new_mode & RTC_MODE_FREE) != 0;
        if (was_free && !becomes_free) {
            /* FREE→HOLD: 自走していた現在値を取り込んで凍結する。 */
            snap_from_time(r, (time_t)rtc_now(r));
            r->snap_dirty = 0;
        } else if (!was_free && becomes_free) {
            /* HOLD→FREE: HOLD中の編集があれば反映してから自走を再開する。 */
            if (r->snap_dirty)
                rtc_commit(r);
            r->snap_dirty = 0;
        }
        r->mode = new_mode;
        return;
    }
    if (off == RTC_A_TEST) {
        uint8_t next = (uint8_t)(val & 0x0Fu);
        if (next != r->test) {
            int64_t now = rtc_now(r);
            r->test = next;
            r->test_anchor_unix = now;
            r->test_anchor_us = monotonic_us(r);
            if (!next)
                r->offset_sec = now - guest_clock_unix_sec(r->clock);
            r->snap_epoch = (time_t)-1;
        }
        return;
    }
    if (off == RTC_A_RESET) {
        r->reset_ctl = (uint8_t)(val & 0x0Cu); /* bit3/2は保持される。 */
        if (val & 0x01u) {
            for (uint32_t a = 0x05; a <= 0x11; a += 2)
                r->bank1[a] = 0;
            r->alarm_written_mask = 0;
            r->alarm_write_inhibit_until_us = monotonic_us(r) + 100u;
        }
        return;
    }
}

uint8_t rtc_reg_peek8(const rp5c15_t *r, uint32_t addr) {
    uint32_t off = addr & 0x1Fu;
    if (off >= RTC_A_SEC1 && off <= RTC_A_YEAR10) {
        if (!(r->mode & RTC_MODE_BANK1))
            return r->bank0[off];
        if (off == 0x03 || off == 0x13 || off == 0x19)
            return 0;
        return r->bank1[off] & bank1_mask(off);
    }
    if (off == RTC_A_MODE)
        return r->mode;
    if (off == RTC_A_TEST || off == RTC_A_RESET)
        return 0;
    return 0x00u;
}
