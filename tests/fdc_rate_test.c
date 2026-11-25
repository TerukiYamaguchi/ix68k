/*
 * fdc.c 転送速度（$E94007 bit4）・回転数（$E94005 bit3-0）と媒体プロファイルの照合テスト。
 * ドライブの転送速度が媒体と合わなければ、READ ID は MA、READ DATA は ND で失敗する。
 * bus/CPU非依存: dmac_ch0_fdc_transfer()はスタブ化する。
 */
#include "bus/bus_internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SECTOR_BYTES 8192u

static uint8_t g_read_buf[MAX_SECTOR_BYTES];

int dmac_ch0_fdc_transfer(x68k_bus_t *b) {
    fdc_t *f = &b->fdc;
    assert(f->dma_dir == 1);
    uint32_t n = f->dma_remaining;
    for (uint32_t i = 0; i < n; i++)
        g_read_buf[i] = fdc_read_data(f);
    return 1;
}

/* ドライブdrvを300rpmにする$E94005 bit3-0（NとN+1 mod 4の同時アサート）。 */
static uint8_t option_300rpm(int drv) {
    return (uint8_t)((1u << drv) | (1u << ((drv + 1) & 3)));
}

static x68k_bus_t *new_bus_with(const fdc_media_profile_t *p, int drv) {
    x68k_bus_t *b = calloc(1, sizeof(*b));
    assert(b != NULL);
    b->fdc.bus = b;
    b->fdc.disk[drv] = calloc(1, p->total_bytes);
    assert(b->fdc.disk[drv] != NULL);
    b->fdc.disk_bytes[drv] = (int)p->total_bytes;
    b->fdc.profile[drv] = p;
    fdc_set_option(&b->fdc, p->rpm300 ? option_300rpm(drv) : 0);
    return b;
}

static void free_bus(x68k_bus_t *b, int drv) {
    free(b->fdc.disk[drv]);
    free(b);
}

/* C0,物理H0の先頭セクタのREAD DATA。成功ならST0のIC=0。 */
static int read_ok(x68k_bus_t *b, int drv, const fdc_media_profile_t *p) {
    static uint8_t ram[16];
    static uint8_t regs[32];
    fdc_t *f = &b->fdc;
    const fdc_sector_entry_t *sec;
    (void)fdc_profile_track_sectors(p, 0, 0, &sec);
    const uint8_t cmd[9] = {0x46u, (uint8_t)drv, 0, fdc_profile_id_h(p, 0, 0, 0),
                            fdc_profile_id_r(p, 0, 0, 0), sec[0].n, fdc_profile_id_r(p, 0, 0, 0),
                            0x35u, 0xFFu};
    for (int i = 0; i < 9; i++)
        fdc_write_data(f, ram, regs, cmd[i]);
    f->phase = FDC_IDLE;
    return (f->res[0] & 0xC0u) == 0;
}

/* READ ID。MAで失敗した場合は0。 */
static int read_id_ok(x68k_bus_t *b, int drv) {
    static uint8_t ram[16];
    static uint8_t regs[32];
    fdc_t *f = &b->fdc;
    fdc_write_data(f, ram, regs, 0x4Au);
    fdc_write_data(f, ram, regs, (uint8_t)drv);
    int ok = (f->res[0] & 0xC0u) == 0 && (f->res[1] & 0x01u) == 0;
    f->phase = FDC_IDLE;
    return ok;
}

/* 全プロファイルについて、転送速度（$E94007 bit4）が媒体と一致するモードだけがアクセスできる。
 * 回転数は媒体に合わせた状態（2HQは300rpm、他は360rpm）で試す。 */
static void test_rate_matrix(void) {
    const fdc_media_profile_t *ps[] = {&fdc_profile_2hd,  &fdc_profile_2hs,  &fdc_profile_2hde,
                                       &fdc_profile_2ht,  &fdc_profile_2hc,  &fdc_profile_2dd9,
                                       &fdc_profile_2dd8, &fdc_profile_2hq,  &fdc_profile_2hdez};
    for (size_t i = 0; i < sizeof(ps) / sizeof(ps[0]); i++) {
        for (int is_2dd = 0; is_2dd < 2; is_2dd++) {
            x68k_bus_t *b = new_bus_with(ps[i], 0);
            fdc_set_density(&b->fdc, is_2dd);
            int match = ps[i]->rate_kbps == (is_2dd ? FDC_RATE_250_KBPS : FDC_RATE_500_KBPS);
            assert(read_id_ok(b, 0) == match);
            assert(read_ok(b, 0, ps[i]) == match);
            free_bus(b, 0);
        }
    }
}

/* 2DD系は250kbps、それ以外は500kbps（9SCSET.DOCの3モード）。 */
static void test_profile_rates(void) {
    assert(fdc_profile_2dd9.rate_kbps == FDC_RATE_250_KBPS);
    assert(fdc_profile_2dd8.rate_kbps == FDC_RATE_250_KBPS);
    assert(fdc_profile_2hd.rate_kbps == FDC_RATE_500_KBPS);
    assert(fdc_profile_2hc.rate_kbps == FDC_RATE_500_KBPS);
    assert(fdc_profile_2hq.rate_kbps == FDC_RATE_500_KBPS);
}

/* ドライブ1でも$E94007 bit4の状態で判定される（ドライブ番号に依らない）。 */
static void test_rate_applies_to_other_drive(void) {
    x68k_bus_t *b = new_bus_with(&fdc_profile_2dd9, 1);
    assert(!read_ok(b, 1, &fdc_profile_2dd9));
    fdc_set_density(&b->fdc, 1);
    assert(read_ok(b, 1, &fdc_profile_2dd9));
    free_bus(b, 1);
}

/* FORMAT TRACKも媒体と合わないモードではNDで、トラックを書かない。 */
static void test_format_requires_matching_mode(void) {
    static uint8_t ram[16];
    static uint8_t regs[32];
    const fdc_media_profile_t *p = &fdc_profile_2dd9;
    x68k_bus_t *b = new_bus_with(p, 0);
    const uint8_t cmd[6] = {0x4Du, 0, 2, 9, 0x54u, 0xE5u};
    for (int i = 0; i < 6; i++)
        fdc_write_data(&b->fdc, ram, regs, cmd[i]);
    assert((b->fdc.res[0] & 0xC0u) == 0x40u && (b->fdc.res[1] & 0x04u));
    assert(b->fdc.disk[0][0] == 0);
    free_bus(b, 0);
}

/* RESETで転送速度が既定(500kbps)へ戻る。 */
static void test_reset_restores_default_mode(void) {
    x68k_bus_t *b = new_bus_with(&fdc_profile_2dd9, 0);
    fdc_set_density(&b->fdc, 1);
    assert(read_ok(b, 0, &fdc_profile_2dd9));
    fdc_reset(&b->fdc);
    assert(!read_ok(b, 0, &fdc_profile_2dd9));
    free_bus(b, 0);
}

/* 500kbps媒体のデータ転送は回転数も合っていなければNDで失敗する。READ IDは回転数を問わない */
static void test_rotation_matrix(void) {
    const fdc_media_profile_t *ps[] = {&fdc_profile_2hd, &fdc_profile_2hc, &fdc_profile_2hq};
    for (size_t i = 0; i < sizeof(ps) / sizeof(ps[0]); i++) {
        for (int slow = 0; slow < 2; slow++) {
            x68k_bus_t *b = new_bus_with(ps[i], 0);
            fdc_set_option(&b->fdc, slow ? option_300rpm(0) : 0);
            assert(read_ok(b, 0, ps[i]) == (slow == ps[i]->rpm300));
            assert(read_id_ok(b, 0));
            free_bus(b, 0);
        }
    }
}

/* 250kbps媒体（2DD）は回転数を問わない。 */
static void test_2dd_ignores_rotation(void) {
    for (int slow = 0; slow < 2; slow++) {
        x68k_bus_t *b = new_bus_with(&fdc_profile_2dd9, 0);
        fdc_set_density(&b->fdc, 1);
        fdc_set_option(&b->fdc, slow ? option_300rpm(0) : 0);
        assert(read_ok(b, 0, &fdc_profile_2dd9));
        free_bus(b, 0);
    }
}

/* ドライブNが300rpmになるのはNとN+1 mod 4の同時アサートだけ。他ドライブの組や単独ビット、
 * 排出用の上位ビットは360rpmのまま。 */
static void test_rotation_option_pairs(void) {
    for (int drv = 0; drv < 4; drv++) {
        x68k_bus_t *b = new_bus_with(&fdc_profile_2hq, drv);
        fdc_set_option(&b->fdc, 0);
        assert(!read_ok(b, drv, &fdc_profile_2hq));
        fdc_set_option(&b->fdc, option_300rpm(drv));
        assert(read_ok(b, drv, &fdc_profile_2hq));
        fdc_set_option(&b->fdc, (uint8_t)(option_300rpm(drv) | 0xF0u));
        assert(read_ok(b, drv, &fdc_profile_2hq));
        fdc_set_option(&b->fdc, (uint8_t)(1u << drv));
        assert(!read_ok(b, drv, &fdc_profile_2hq));
        fdc_set_option(&b->fdc, option_300rpm((drv + 1) & 3));
        assert(!read_ok(b, drv, &fdc_profile_2hq));
        free_bus(b, drv);
    }
}

/* RESETで回転数は360rpmへ戻る。 */
static void test_reset_restores_rotation(void) {
    x68k_bus_t *b = new_bus_with(&fdc_profile_2hq, 0);
    assert(read_ok(b, 0, &fdc_profile_2hq));
    fdc_reset(&b->fdc);
    assert(!read_ok(b, 0, &fdc_profile_2hq));
    free_bus(b, 0);
}

/* READ IDのカーソルはドライブごとで、RESETで先頭へ戻る。 */
static void test_read_id_cursor_per_drive_and_reset(void) {
    static uint8_t ram[16];
    static uint8_t regs[32];
    x68k_bus_t *b = new_bus_with(&fdc_profile_2hc, 0);
    b->fdc.disk[1] = calloc(1, fdc_profile_2hc.total_bytes);
    assert(b->fdc.disk[1] != NULL);
    b->fdc.disk_bytes[1] = (int)fdc_profile_2hc.total_bytes;
    b->fdc.profile[1] = &fdc_profile_2hc;
    for (int i = 0; i < 3; i++) {
        fdc_write_data(&b->fdc, ram, regs, 0x4Au);
        fdc_write_data(&b->fdc, ram, regs, 0);
        assert(b->fdc.res[5] == i + 1);
        b->fdc.phase = FDC_IDLE;
    }
    fdc_write_data(&b->fdc, ram, regs, 0x4Au);
    fdc_write_data(&b->fdc, ram, regs, 1);
    assert(b->fdc.res[5] == 1); /* ドライブ1は先頭から */
    b->fdc.phase = FDC_IDLE;
    fdc_reset(&b->fdc);
    fdc_write_data(&b->fdc, ram, regs, 0x4Au);
    fdc_write_data(&b->fdc, ram, regs, 0);
    assert(b->fdc.res[5] == 1);
    b->fdc.phase = FDC_IDLE;
    free(b->fdc.disk[1]);
    free_bus(b, 0);
}

int main(void) {
    test_rate_matrix();
    test_profile_rates();
    test_rate_applies_to_other_drive();
    test_format_requires_matching_mode();
    test_reset_restores_default_mode();
    test_rotation_matrix();
    test_2dd_ignores_rotation();
    test_rotation_option_pairs();
    test_reset_restores_rotation();
    test_read_id_cursor_per_drive_and_reset();
    printf("fdc_rate_test: OK\n");
    return 0;
}
