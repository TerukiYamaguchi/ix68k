/*
 * RTC (RP5C15, $E8A001-$E8A01F) の単体テスト。
 * XEiJ MemoryMappedDevice.MMD_RTC / RP5C15.java 準拠の偶数バスエラー・
 * 奇数バイトレジスタ・HOLD/FREE状態機械・weekday_offset・IOCSとの時計共有を検証する。
 */
#include "bus/bus_internal.h"
#include <assert.h>
#include <stdio.h>
#include <time.h>

#define RTC_MODE_ADDR  0xE8A01Bu
#define RTC_TEST_ADDR  0xE8A01Du
#define RTC_RESET_ADDR 0xE8A01Fu

/* addr(奇数、$E8A001-$E8A019)を1バイト読む薄いラッパー。 */
static uint8_t rd(mc68k_bus_t *h, uint32_t addr) {
    uint8_t v = 0xFFu;
    bool ok = h->read_byte(h->ctx, addr, &v);
    assert(ok);
    return v;
}

static void wr(mc68k_bus_t *h, uint32_t addr, uint8_t val) {
    bool ok = h->write_byte(h->ctx, addr, val);
    assert(ok);
}

/* BANK0カウンタ13レジスタをまとめて読み、10進値へ組み立てる。 */
typedef struct {
    int sec, min, hour, day, mon, year2, wday;
} bcd_time_t;

static bcd_time_t read_counters(mc68k_bus_t *h) {
    bcd_time_t t;
    t.sec = rd(h, 0xE8A003u) * 10 + rd(h, 0xE8A001u);
    t.min = rd(h, 0xE8A007u) * 10 + rd(h, 0xE8A005u);
    t.hour = rd(h, 0xE8A00Bu) * 10 + rd(h, 0xE8A009u);
    t.wday = rd(h, 0xE8A00Du);
    t.day = rd(h, 0xE8A011u) * 10 + rd(h, 0xE8A00Fu);
    t.mon = rd(h, 0xE8A015u) * 10 + rd(h, 0xE8A013u);
    t.year2 = rd(h, 0xE8A019u) * 10 + rd(h, 0xE8A017u);
    return t;
}

/* localtime(t) から期待値を独立に組み立てる（rtc.c実装とは別経路で検算する）。 */
static bcd_time_t expected_from_time(time_t t) {
    struct tm tmv = *localtime(&t);
    bcd_time_t e;
    e.sec = tmv.tm_sec;
    e.min = tmv.tm_min;
    e.hour = tmv.tm_hour;
    e.day = tmv.tm_mday;
    e.mon = tmv.tm_mon + 1;
    e.year2 = ((tmv.tm_year + 1900) - 1980) % 100;
    e.wday = tmv.tm_wday;
    return e;
}

static void assert_bcd_eq(bcd_time_t a, bcd_time_t b) {
    assert(a.sec == b.sec);
    assert(a.min == b.min);
    assert(a.hour == b.hour);
    assert(a.day == b.day);
    assert(a.mon == b.mon);
    assert(a.year2 == b.year2);
    assert(a.wday == b.wday);
}

static time_t make_time(int year, int mon1, int day, int hour, int min, int sec) {
    struct tm t;
    time_t now = time(NULL);
    t = *localtime(&now); /* tm_isdst等の地域情報を継承する種 */
    t.tm_year = year - 1900;
    t.tm_mon = mon1 - 1;
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_min = min;
    t.tm_sec = sec;
    t.tm_isdst = -1;
    return mktime(&t);
}

int main(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    mc68k_bus_t hooks = x68k_bus_get_hooks(b);

    /* 1. デコード分離: crtc_regs汎用配列を直接汚しても、RTC奇数アドレスの
     *    読み出しには影響しない（bus_read_crtc8がRTC範囲を先取りするため）。 */
    b->crtc_regs[0xA001u] = 0xAAu; /* $E8A001 - CRTC_BASE */
    uint8_t v = rd(&hooks, 0xE8A001u);
    assert(v != 0xAAu && v <= 9);

    /* 2. 偶数アドレスのバイトアクセスはバスエラー（XEiJ MMD_RTC準拠）。
     *    奇数アドレスは成功する。 */
    uint8_t dummy;
    assert(!hooks.read_byte(hooks.ctx, 0xE8A000u, &dummy));
    assert(!hooks.write_byte(hooks.ctx, 0xE8A002u, 0x00u));
    assert(hooks.read_byte(hooks.ctx, 0xE8A001u, &dummy));
    assert(!hooks.read_byte(hooks.ctx, 0xE8B000u, &dummy));
    assert(hooks.read_byte(hooks.ctx, 0xE8B001u, &dummy));

    /* 3. BANK1個別仕様: ADJUST/予約は読出0、CLKOUT/アラーム/12-24/leapは
     *    RP5C15の有効ビットだけを保持する。 */
    wr(&hooks, RTC_MODE_ADDR, 0x09u); /* FREE|BANK1 */
    static const uint32_t bank_addrs[] = {0xE8A001u, 0xE8A005u, 0xE8A007u, 0xE8A009u,
                                          0xE8A00Bu, 0xE8A00Du, 0xE8A00Fu, 0xE8A011u,
                                          0xE8A015u, 0xE8A017u};
    static const uint8_t expect_mask[] = {0x07, 0x0F, 0x07, 0x0F, 0x03,
                                          0x07, 0x0F, 0x03, 0x01, 0x03};
    for (size_t i = 0; i < sizeof(bank_addrs) / sizeof(bank_addrs[0]); i++) {
        wr(&hooks, bank_addrs[i], 0xFFu);
        assert(rd(&hooks, bank_addrs[i]) == expect_mask[i]);
    }
    wr(&hooks, 0xE8A003u, 0xFFu);
    assert(rd(&hooks, 0xE8A003u) == 0);
    assert(rd(&hooks, 0xE8A013u) == 0 && rd(&hooks, 0xE8A019u) == 0);
    wr(&hooks, RTC_MODE_ADDR, 0x08u); /* FREE|BANK0 に戻す（BANK0側は無傷のはず） */

    /* 4. BCD正当性 + HOLD一貫性: 既知時刻をx68k_bus_rtc_set()で設定し、HOLDで
     *    フリーズしたスナップショットが13レジスタで一致し、HOLD中はホスト側の
     *    経過時間（offset_secの人為的な変化）を無視し続けること。 */
    time_t target = make_time(2026, 9, 10, 23, 59, 58); /* 木曜 */
    x68k_bus_rtc_set(b, (int64_t)target);
    wr(&hooks, RTC_MODE_ADDR, 0x00u); /* FREE(自走)→HOLD: 現在値を凍結 */
    bcd_time_t got = read_counters(&hooks);
    assert_bcd_eq(got, expected_from_time(target));
    b->rtc.offset_sec += 2; /* HOLD中に「2秒経過」させても凍結値は変わらないはず */
    bcd_time_t got2 = read_counters(&hooks);
    assert_bcd_eq(got, got2);

    /* 5. FREE復帰後の進行: HOLD解除で自走を再開し、日跨ぎ(23:59:58+2s→翌日00:00:00)
     *    を正しく反映すること。 */
    wr(&hooks, RTC_MODE_ADDR, 0x08u); /* HOLD→FREE */
    bcd_time_t got3 = read_counters(&hooks);
    assert_bcd_eq(got3, expected_from_time(target + 2));

    /* 6. 時刻設定（rtsettod相当）: HOLD中に12レジスタへ2000-02-29(閏年)を書き込み、
     *    FREE復帰でMMIO読み出しとx68k_bus_rtc_now()の両方が一致すること。 */
    wr(&hooks, RTC_MODE_ADDR, 0x00u); /* HOLD */
    wr(&hooks, 0xE8A001u, 6);
    wr(&hooks, 0xE8A003u, 5); /* sec=56 */
    wr(&hooks, 0xE8A005u, 4);
    wr(&hooks, 0xE8A007u, 3); /* min=34 */
    wr(&hooks, 0xE8A009u, 2);
    wr(&hooks, 0xE8A00Bu, 1); /* hour=12 */
    wr(&hooks, 0xE8A00Fu, 9);
    wr(&hooks, 0xE8A011u, 2); /* day=29 */
    wr(&hooks, 0xE8A013u, 2);
    wr(&hooks, 0xE8A015u, 0); /* mon=02 */
    wr(&hooks, 0xE8A017u, 0);
    wr(&hooks, 0xE8A019u, 2); /* year2=20 (2000) */
    wr(&hooks, RTC_MODE_ADDR, 0x08u); /* HOLD→FREE: commit */
    time_t expect_2000 = make_time(2000, 2, 29, 12, 34, 56);
    int64_t now_after = x68k_bus_rtc_now(b);
    assert(now_after == (int64_t)expect_2000);
    wr(&hooks, RTC_MODE_ADDR, 0x00u); /* 再度HOLDして読み戻しを検証 */
    assert_bcd_eq(read_counters(&hooks), expected_from_time(expect_2000));

    /* 7. 年末境界: 2026-12-31 23:59:59 → +1s で 2027-01-01、year2は47。 */
    wr(&hooks, RTC_MODE_ADDR, 0x08u); /* 直前のHOLDから一旦FREEへ（rtc_set直後のHOLD書き込みで
                                          確実にFREE→HOLD遷移させ、新しい値を取り込むため） */
    time_t ynd = make_time(2026, 12, 31, 23, 59, 59);
    x68k_bus_rtc_set(b, (int64_t)ynd + 1);
    wr(&hooks, RTC_MODE_ADDR, 0x00u); /* FREE→HOLDで取り込む */
    bcd_time_t yexp = expected_from_time(ynd + 1);
    assert(yexp.year2 == 47 && yexp.mon == 1 && yexp.day == 1);
    assert_bcd_eq(read_counters(&hooks), yexp);

    /* 8. 曜日の独立性: WDAYへ日付と無関係な値(0-6)を明示的に書き込み、読み戻しで
     *    保持されること（localtime()由来の値へ勝手に戻らない）。 */
    uint8_t cur_wday = rd(&hooks, 0xE8A00Du);
    uint8_t new_wday = (uint8_t)((cur_wday + 3) % 7);
    wr(&hooks, 0xE8A00Du, new_wday);
    assert(rd(&hooks, 0xE8A00Du) == new_wday);
    wr(&hooks, RTC_MODE_ADDR, 0x08u); /* HOLD→FREE(commit) */
    wr(&hooks, RTC_MODE_ADDR, 0x00u); /* FREE→HOLD(再取り込み) */
    assert(rd(&hooks, 0xE8A00Du) == new_wday); /* weekday_offsetにより保持される */
    wr(&hooks, RTC_MODE_ADDR, 0x08u);          /* FREEへ戻す */

    /* 9. MODE/TEST/RESET: MODEだけが読出し可能。TEST/RESETは書込専用で、
     *    RESET bit0はalarmレジスタを0へ戻す。 */
    wr(&hooks, RTC_MODE_ADDR, 0xFFu);
    assert(rd(&hooks, RTC_MODE_ADDR) == 0x0Du);
    wr(&hooks, RTC_MODE_ADDR, 0x08u); /* FREEへ戻す（以降のテストのため） */
    wr(&hooks, RTC_TEST_ADDR, 0xFFu);
    assert(rd(&hooks, RTC_TEST_ADDR) == 0);
    wr(&hooks, RTC_TEST_ADDR, 0x00u);
    wr(&hooks, RTC_MODE_ADDR, 0x09u);
    wr(&hooks, 0xE8A005u, 9);
    wr(&hooks, RTC_MODE_ADDR, 0x08u);
    wr(&hooks, RTC_RESET_ADDR, 0xFFu);
    assert(rd(&hooks, RTC_RESET_ADDR) == 0);
    wr(&hooks, RTC_MODE_ADDR, 0x09u);
    assert(rd(&hooks, 0xE8A005u) == 0);
    wr(&hooks, RTC_MODE_ADDR, 0x08u);

    /* 10. alarm比較器: RESET後は全桁don't-care、alarm各桁を書いた分だけ
     *     比較し、MODE alarmが有効なときだけLOWを出す。パルスは両方OFFにする。 */
    time_t alarm_time = make_time(2026, 3, 4, 5, 6, 0);
    x68k_bus_rtc_set(b, (int64_t)alarm_time);
    wr(&hooks, RTC_MODE_ADDR, 0x00u); /* HOLDしてbank0を固定 */
    uint8_t alarm_wday = rd(&hooks, 0xE8A00Du);
    wr(&hooks, RTC_RESET_ADDR, 0x0Du); /* 1/16Hz OFF + alarm reset */
    b->rtc.alarm_write_inhibit_until_us = 0; /* テスト時計ではなくMMIO比較だけを検査する。 */
    wr(&hooks, RTC_MODE_ADDR, 0x05u); /* HOLD|ALARM|BANK1 */
    wr(&hooks, 0xE8A005u, 6);
    wr(&hooks, 0xE8A007u, 0);
    wr(&hooks, 0xE8A009u, 5);
    wr(&hooks, 0xE8A00Bu, 0);
    wr(&hooks, 0xE8A00Du, alarm_wday);
    wr(&hooks, 0xE8A00Fu, 4);
    wr(&hooks, 0xE8A011u, 0);
    wr(&hooks, RTC_MODE_ADDR, 0x04u); /* HOLD|ALARM|BANK0 */
    assert(rtc_alarm_low(&b->rtc));
    wr(&hooks, RTC_MODE_ADDR, 0x05u);
    wr(&hooks, 0xE8A005u, 7);
    wr(&hooks, RTC_MODE_ADDR, 0x04u);
    assert(!rtc_alarm_low(&b->rtc));
    wr(&hooks, RTC_MODE_ADDR, 0x08u);

    /* 11. GPIP0: RTC LOWをbit0=0へ反映し、AER=fallingでIERB/IMRBが
     *     揃う場合にIPRB/ch0をラッチする。 */
    mfp_reg_write8(b, 0xE88003u, 0x00u);
    mfp_reg_write8(b, 0xE88009u, 0x01u);
    mfp_reg_write8(b, 0xE88015u, 0x01u);
    x68k_bus_mfp_rtc_alarm(b, 1);
    assert((mfp_reg_read8(b, 0xE88001u) & 0x01u) == 0);
    assert((b->mfp.iprb & 0x01u) != 0);
    assert(mfp_ch_pending_test(&b->mfp, MFP_CH_ALARM));
    x68k_bus_mfp_rtc_alarm(b, 0);
    assert((mfp_reg_read8(b, 0xE88001u) & 0x01u) != 0);

    /* 12. GPIP2(POW SW): ix68kの電源投入時はON=LOW。AER bit2が選ぶ両エッジで
     * IERB/IPRB/IMRB/ch2へ反映され、CPU RESET相当では外部入力レベルを保持する。 */
    assert((mfp_reg_read8(b, 0xE88001u) & 0x04u) == 0);
    mfp_reg_write8(b, 0xE88003u, 0x04u); /* rising */
    mfp_reg_write8(b, 0xE88009u, 0x04u);
    mfp_reg_write8(b, 0xE88015u, 0x04u);
    x68k_bus_mfp_power_switch(b, 0); /* OFF: LOW→HIGH */
    assert((mfp_reg_read8(b, 0xE88001u) & 0x04u) != 0);
    assert((b->mfp.iprb & 0x04u) != 0);
    assert(mfp_ch_pending_test(&b->mfp, MFP_CH_POWER_SWITCH));
    mfp_reg_write8(b, 0xE8800Du, 0x00u); /* IPRB clear */
    assert(!mfp_ch_pending_test(&b->mfp, MFP_CH_POWER_SWITCH));
    mfp_reg_write8(b, 0xE88003u, 0x00u); /* falling */
    x68k_bus_mfp_power_switch(b, 1); /* ON: HIGH→LOW */
    assert((mfp_reg_read8(b, 0xE88001u) & 0x04u) == 0);
    assert((b->mfp.iprb & 0x04u) != 0);
    assert(mfp_ch_pending_test(&b->mfp, MFP_CH_POWER_SWITCH));

    /* 13. peek8は非侵襲: x68k_bus_peek8()を1000回呼んでも内部スナップショットが
     *     変化しないこと（副作用ゼロ、MCPデバッガのメモリダンプ用）。 */
    time_t before = (time_t)x68k_bus_rtc_now(b);
    x68k_bus_rtc_set(b, (int64_t)before); /* 基準を固定 */
    wr(&hooks, RTC_MODE_ADDR, 0x00u);     /* HOLDでスナップショットを固定 */
    bcd_time_t peek_before = read_counters(&hooks);
    for (int i = 0; i < 1000; i++)
        (void)x68k_bus_peek8(b, 0xE8A001u);
    bcd_time_t peek_after = read_counters(&hooks);
    assert_bcd_eq(peek_before, peek_after);
    wr(&hooks, RTC_MODE_ADDR, 0x08u); /* FREEへ戻す */

    /* 14. ワード/ロングアクセス: 奇数側レジスタのみを反映する
     *     （XEiJ MMD_RTC mmdRws/mmdRls準拠）。 */
    wr(&hooks, RTC_MODE_ADDR, 0x00u); /* HOLDで固定した状態で確認 */
    uint16_t wv = 0;
    assert(hooks.read_word(hooks.ctx, 0xE8A000u, &wv));
    assert(wv == rd(&hooks, 0xE8A001u));
    uint32_t lv = 0;
    assert(hooks.read_long(hooks.ctx, 0xE8A000u, &lv));
    assert(lv == (((uint32_t)rd(&hooks, 0xE8A001u) << 16) | rd(&hooks, 0xE8A003u)));
    wr(&hooks, RTC_MODE_ADDR, 0x08u);

    /* 15. IOCS整合: x68k_bus_rtc_set()後、MMIOから再構成した時刻と
     *     x68k_bus_rtc_now()が一致すること（時計が1つであることの回帰テスト）。 */
    time_t chk = make_time(2025, 6, 15, 8, 20, 30);
    x68k_bus_rtc_set(b, (int64_t)chk);
    wr(&hooks, RTC_MODE_ADDR, 0x00u);
    bcd_time_t rebuilt = read_counters(&hooks);
    assert(rebuilt.sec == expected_from_time(chk).sec &&
          rebuilt.min == expected_from_time(chk).min &&
          rebuilt.hour == expected_from_time(chk).hour);
    assert((int64_t)x68k_bus_rtc_now(b) == (int64_t)chk);
    wr(&hooks, RTC_MODE_ADDR, 0x08u);

    /* 16. リセット非破壊: CPU RESET相当(x68k_bus_reset_peripheral)を跨いでも
     *     offset_secが保持されること（設計レビューでの指摘事項）。 */
    int64_t before_reset = x68k_bus_rtc_now(b);
    x68k_bus_reset_peripheral(b);
    int64_t after_reset = x68k_bus_rtc_now(b);
    assert(before_reset == after_reset || (after_reset - before_reset) <= 1);

    x68k_bus_destroy(b);
    printf("rtc_rp5c15_test: OK\n");
    return 0;
}
