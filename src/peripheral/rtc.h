#ifndef X68K_RTC_H
#define X68K_RTC_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "bus/guest_clock.h"

/*
 * RTC (RP5C15): 0xE8A000-0xE8BFFF（32バイト周期、奇数アドレス）。
 * XEiJ MemoryMappedDevice.MMD_RTC_FIRST/NEXT・RP5C15.java 準拠。
 * レジスタはXEiJに合わせてbank0[32]/bank1[32]をaddr&0x1Fで直接引く
 * フラット配列で持つ（奇数indexのみ意味を持つ）。
 */
typedef struct {
    const guest_clock_t *clock;
    int64_t offset_sec; /* ゲスト壁時計に加算する秒オフセット。
                            IOCS _DATESET/_TIMESET(iocs.c)と共有する唯一の時刻源。 */
    uint8_t bank0[32];  /* MODE bit0=0（BANK0）選択時のカウンタレジスタ。
                            addr&0x1F個の生ニブル、奇数indexのみ使用。 */
    uint8_t bank1[32];  /* MODE bit0=1（BANK1）選択時のレジスタ。値を持つオフセットだけ使用。 */
    int weekday_offset; /* WDAYレジスタ書き込みで生じる、localtime()のtm_wdayからの
                            ずれ（0-6、XEiJ rtcWeekGap相当）。日付進行では変化しない
                            ため、実時間の経過に対しては曜日が自動的に正しく進む。 */
    uint8_t mode;       /* $E8A01B: bit3=FREE(1)/HOLD(0)、bit2=アラーム出力ON/OFF
                            （MFP GPIP0へ配線済み）、bit0=バンク選択。有効ビットのみの
                            マスク0x0D（bit1は未定義）。 */
    uint8_t test;       /* $E8A01D: 書込専用。bit3-0で時刻の進みを加速する（rtc_now参照）。 */
    int64_t test_anchor_unix; /* TEST切替時の論理時計。 */
    uint64_t test_anchor_us;  /* TEST切替時の単調時計。 */
    uint8_t reset_ctl; /* $E8A01F: bit3/2だけ内部保持、bit1/0はone-shot。CPU読出しは常に0。 */
    uint64_t alarm_write_inhibit_until_us; /* RESET bit0後のalarm書込禁止期限（単調時計）。 */
    uint8_t alarm_written_mask; /* BANK1 alarm各桁を書いたか。0=RESET直後の強制一致。 */
    time_t snap_epoch;          /* bank0を最後に再構築した時点のrtc_now()値。FREE中の
                                    読み出しごとの再計算を、秒が変わった時だけに絞る。 */
    int snap_dirty;             /* HOLD中にbank0へ書き込みがあったか（HOLD→FREE遷移で
                                    rtc_commit()するかどうかの判定に使う）。 */
} rp5c15_t;

/* MODEレジスタの有効ビット（bit1は未定義のため0x0Fでなく0x0D）。
 * bit2のALARMはrtc_alarm_low()経由でMFP GPIP0へ反映される。 */
#define RTC_MODE_FREE  0x08u /* bit3: 1=FREE(自走), 0=HOLD(表示保持・読み書き用に凍結) */
#define RTC_MODE_ALARM 0x04u /* bit2: アラーム出力ON/OFF */
#define RTC_MODE_BANK1 0x01u /* bit0: 1=BANK1選択 */
#define RTC_MODE_MASK  0x0Du

/* create時（x68k_bus_create()）にのみ呼ぶ。CPU RESET命令では触らない
 * （XEiJ RP5C15.rtcReset()が空実装であることを確認済み）。 */
void rtc_init(rp5c15_t *r, const guest_clock_t *clock);

/* $E8A001-$E8A01F（奇数）へのCPUバイトアクセス。呼び出し側（x68k_bus.c）が
 * 偶数アドレスを弾く（バスエラー）ため、ここは奇数addrのみを受け取る前提。 */
uint8_t rtc_reg_read8(rp5c15_t *r, uint32_t addr);
void rtc_reg_write8(rp5c15_t *r, uint32_t addr, uint8_t val);

/* 非侵襲peek（MCPデバッガ用）。rtc_syncを呼ばず、副作用なしで現在のbank0/bank1の
 * 生値を返す。 */
uint8_t rtc_reg_peek8(const rp5c15_t *r, uint32_t addr);

/* IOCS _DATEGET/_TIMEGET/MMIO読み出しが共有する時刻源。 */
int64_t rtc_now(const rp5c15_t *r);

/* IOCS _DATESET/_TIMESETが呼ぶ。offset_sec = target_unix - guest_unix。 */
void rtc_set(rp5c15_t *r, int64_t target_unix);
void rtc_snapshot_rebase(rp5c15_t *r, int64_t saved_now);

/* RP5C15 ALARM端子の現在レベル。true=オープンドレイン駆動中（LOW）。
 * MFP GPIP0への反映はx68k_bus_mfp_rtc_alarm()（x68k_bus.c）が行う。 */
bool rtc_alarm_low(rp5c15_t *r);

#endif /* X68K_RTC_H */
