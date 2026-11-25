/*
 * fdc.c FORMAT TRACK(opcode 0x0D)の単体テスト。
 * bus/CPU非依存: dmac_ch0_fdc_transfer()はWRITE方向のスタブとしてg_idsを供給する。
 */
#include "bus/bus_internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include "platform/win_compat.h" /* mkstemp()/write()/close() 相当 */
#else
#include <unistd.h>
#endif

#define FORMAT_FILL 0xE5u
#define PATTERN     0x5Au

static uint8_t g_ids[FDC_PROFILE_MAX_SECTORS * 4]; /* DMACがFDCへ送るIDタプル列 */
static int g_dma_calls;

/* 実DMACのch0 WRITE方向を模擬する: dma_remainingバイトをg_idsから送る。 */
int dmac_ch0_fdc_transfer(x68k_bus_t *b) {
    fdc_t *f = &b->fdc;
    static uint8_t ram[16];
    static uint8_t regs[32];
    assert(f->dma_dir == 0);
    uint32_t n = f->dma_remaining;
    g_dma_calls++;
    for (uint32_t i = 0; i < n; i++)
        fdc_write_data(f, ram, regs, g_ids[i]);
    return 1;
}

static x68k_bus_t *new_bus_with(const fdc_media_profile_t *p) {
    x68k_bus_t *b = calloc(1, sizeof(*b));
    assert(b != NULL);
    b->fdc.bus = b;
    b->fdc.disk[0] = malloc(p->total_bytes);
    assert(b->fdc.disk[0] != NULL);
    memset(b->fdc.disk[0], PATTERN, p->total_bytes);
    b->fdc.disk_bytes[0] = (int)p->total_bytes;
    b->fdc.profile[0] = p;
    /* ドライブ0を媒体の転送速度に合わせる */
    fdc_set_density(&b->fdc, p->rate_kbps == FDC_RATE_250_KBPS);
    return b;
}

static void free_bus(x68k_bus_t *b) {
    free(b->fdc.disk[0]);
    free(b);
}

/* プロファイルの正規ID列をg_idsへ作る。 */
static void make_ids(const fdc_media_profile_t *p, int cyl, int hd) {
    memset(g_ids, 0, sizeof(g_ids));
    const fdc_sector_entry_t *sec;
    int count = fdc_profile_track_sectors(p, cyl, hd, &sec);
    for (int i = 0; i < count; i++) {
        g_ids[i * 4 + 0] = (uint8_t)cyl;
        g_ids[i * 4 + 1] = fdc_profile_id_h(p, cyl, hd, i);
        g_ids[i * 4 + 2] = fdc_profile_id_r(p, cyl, hd, i);
        g_ids[i * 4 + 3] = sec[i].n;
    }
}

/* FORMAT TRACK(0x4D)を発行する。ヘッドはcur_cylの位置にあるものとする。 */
static void send_format(x68k_bus_t *b, int hd, int n, int sc) {
    static uint8_t ram[16];
    static uint8_t regs[32];
    const uint8_t cmd[6] = {
        0x4Du, (uint8_t)((hd & 1) << 2), (uint8_t)n, (uint8_t)sc, 0x54u, FORMAT_FILL};
    for (int i = 0; i < 6; i++)
        fdc_write_data(&b->fdc, ram, regs, cmd[i]);
}

static int track_is(const x68k_bus_t *b, const fdc_media_profile_t *p, int cyl, int hd,
                    uint8_t v) {
    const uint8_t *t = b->fdc.disk[0] + (uint32_t)(cyl * p->heads + hd) * p->track_bytes;
    for (uint32_t i = 0; i < p->track_bytes; i++)
        if (t[i] != v)
            return 0;
    return 1;
}

static void expect_ok(const x68k_bus_t *b) {
    assert(b->fdc.phase == FDC_RESULT);
    assert(b->fdc.res_len == 7 && b->fdc.irq == 1);
    assert(b->fdc.res[0] == 0 && b->fdc.res[1] == 0 && b->fdc.res[2] == 0);
}

static void expect_nd(const x68k_bus_t *b) {
    assert(b->fdc.phase == FDC_RESULT);
    assert((b->fdc.res[0] & 0xC0u) == 0x40u && (b->fdc.res[1] & 0x04u));
}

/* 正規ID列なら対象トラックだけがフィルされ、隣接トラックは不変。 */
static void test_format_profile(const fdc_media_profile_t *p, int cyl, int hd) {
    x68k_bus_t *b = new_bus_with(p);
    b->fdc.cur_cyl[0] = (uint8_t)cyl;
    make_ids(p, cyl, hd);
    g_dma_calls = 0;
    const fdc_sector_entry_t *sec;
    int count = fdc_profile_track_sectors(p, cyl, hd, &sec);
    send_format(b, hd, sec[0].n, count);
    assert(g_dma_calls == 1);
    expect_ok(b);
    assert(track_is(b, p, cyl, hd, FORMAT_FILL));
    if (hd == 0)
        assert(track_is(b, p, cyl, 1, PATTERN));
    else
        assert(track_is(b, p, cyl, 0, PATTERN));
    if (cyl > 0)
        assert(track_is(b, p, cyl - 1, hd, PATTERN));
    if (cyl + 1 < p->cyls)
        assert(track_is(b, p, cyl + 1, hd, PATTERN));
    free_bus(b);
}

/* 2HSの(C0,H0)はIPL用にR=1,11..18、それ以外のトラックはR=10..18。 */
static void test_format_2hs_boot_track(void) {
    const fdc_media_profile_t *p = &fdc_profile_2hs;
    make_ids(p, 0, 0);
    assert(g_ids[2] == 1 && g_ids[6] == 11 && g_ids[34] == 18);
    make_ids(p, 0, 1);
    assert(g_ids[2] == 10);
    make_ids(p, 1, 0);
    assert(g_ids[2] == 10);
    /* (C0,H0)にR=10始まりの通常列を送るとND */
    x68k_bus_t *b = new_bus_with(p);
    b->fdc.cur_cyl[0] = 0;
    make_ids(p, 1, 0);
    g_ids[0] = 0;
    send_format(b, 0, 3, 9);
    expect_nd(b);
    assert(track_is(b, p, 0, 0, PATTERN));
    free_bus(b);
}

/* 2HTの(C0,H0)は2HS配置(R=1,11..18 N=3)、他は R=1/N=4 + R=2/N=6 の2セクタ。
 * SCがトラックのセクタ数と違えばNDで、イメージは変わらない。 */
static void test_format_2ht_boot_track(void) {
    const fdc_media_profile_t *p = &fdc_profile_2ht;
    x68k_bus_t *b = new_bus_with(p);
    b->fdc.cur_cyl[0] = 0;
    make_ids(p, 0, 0);
    assert(g_ids[2] == 1 && g_ids[3] == 3 && g_ids[6] == 11 && g_ids[34] == 18);
    send_format(b, 0, 3, 9);
    expect_ok(b);
    assert(track_is(b, p, 0, 0, FORMAT_FILL)); /* トラック枠(10240)全体をフィル */
    assert(track_is(b, p, 0, 1, PATTERN));
    b->fdc.phase = FDC_IDLE;
    make_ids(p, 0, 0);
    send_format(b, 0, 3, 8); /* SCが9でない */
    expect_nd(b);
    b->fdc.phase = FDC_IDLE;
    b->fdc.cur_cyl[0] = 1;
    make_ids(p, 1, 0);
    assert(g_ids[3] == 4 && g_ids[7] == 6);
    send_format(b, 0, 4, 9); /* 通常トラックに9セクタ(SC過多)は拒否 */
    expect_nd(b);
    assert(track_is(b, p, 1, 0, PATTERN));
    free_bus(b);
}

/* 2HDEの(C0,H0)はIPL用にH=0,R=1、残りはH=128,R=2..9。他トラックはH=128/129,R=1..9。 */
static void test_format_2hde_boot_track(void) {
    const fdc_media_profile_t *p = &fdc_profile_2hde;
    make_ids(p, 0, 0);
    assert(g_ids[1] == 0 && g_ids[2] == 1 && g_ids[5] == 128 && g_ids[6] == 2);
    x68k_bus_t *b = new_bus_with(p);
    b->fdc.cur_cyl[0] = 0;
    send_format(b, 0, 3, 9);
    expect_ok(b);
    assert(track_is(b, p, 0, 0, FORMAT_FILL));
    free_bus(b);
    make_ids(p, 0, 1);
    assert(g_ids[1] == 129 && g_ids[2] == 1);
    make_ids(p, 1, 0);
    assert(g_ids[1] == 128 && g_ids[2] == 1);
    /* (C0,H0)に全てH=128の列を送るとND */
    b = new_bus_with(p);
    b->fdc.cur_cyl[0] = 0;
    make_ids(p, 1, 0);
    g_ids[0] = 0;
    send_format(b, 0, 3, 9);
    expect_nd(b);
    assert(track_is(b, p, 0, 0, PATTERN));
    free_bus(b);
}

/* 2HDEZ: 通常配置(H=128,R=1..9)に加え、全トラック書込み中の(C0,H0)特例配置
 * (先頭だけH=0,R=1)も受理する。他のトラックで特例配置は受理しない。 */
static void test_format_2hdez_transient_boot_track(void) {
    const fdc_media_profile_t *p = &fdc_profile_2hdez;
    x68k_bus_t *b = new_bus_with(p);
    b->fdc.cur_cyl[0] = 0;
    make_ids(p, 0, 0);
    assert(g_ids[1] == 128 && g_ids[2] == 1);
    send_format(b, 0, 3, 9);
    expect_ok(b);
    assert(track_is(b, p, 0, 0, FORMAT_FILL));
    free_bus(b);

    b = new_bus_with(p);
    b->fdc.cur_cyl[0] = 0;
    make_ids(p, 0, 0);
    g_ids[1] = 0; /* 特例配置: 先頭だけH=0,R=1 */
    send_format(b, 0, 3, 9);
    expect_ok(b);
    assert(track_is(b, p, 0, 0, FORMAT_FILL));
    free_bus(b);

    b = new_bus_with(p); /* (C1,H0)では特例配置は不可 */
    b->fdc.cur_cyl[0] = 1;
    make_ids(p, 1, 0);
    g_ids[1] = 0;
    send_format(b, 0, 3, 9);
    expect_nd(b);
    assert(track_is(b, p, 1, 0, PATTERN));
    free_bus(b);

    b = new_bus_with(p); /* 先頭以外がH=0の列は不可 */
    b->fdc.cur_cyl[0] = 0;
    make_ids(p, 0, 0);
    g_ids[1] = 0;
    g_ids[5] = 0;
    send_format(b, 0, 3, 9);
    expect_nd(b);
    free_bus(b);
}

static void test_format_all_profiles(void) {
    const fdc_media_profile_t *ps[] = {&fdc_profile_2hd,  &fdc_profile_2hs, &fdc_profile_2hde,
                                       &fdc_profile_2ht,  &fdc_profile_2hc, &fdc_profile_2dd9,
                                       &fdc_profile_2dd8, &fdc_profile_2hq, &fdc_profile_2hdez};
    for (size_t i = 0; i < sizeof(ps) / sizeof(ps[0]); i++) {
        test_format_profile(ps[i], 0, 0);
        test_format_profile(ps[i], 1, 1);
        test_format_profile(ps[i], ps[i]->cyls - 1, 1);
    }
}

/* ID列がプロファイルと食い違えば、イメージは変更されずND。 */
static void test_format_rejects_mismatch(void) {
    const fdc_media_profile_t *p = &fdc_profile_2hs;
    for (int field = 0; field < 4; field++) {
        x68k_bus_t *b = new_bus_with(p);
        b->fdc.cur_cyl[0] = 3;
        make_ids(p, 3, 1);
        g_ids[4 * 4 + field] ^= 0x01u; /* 5番目のIDの1要素だけ崩す */
        send_format(b, 1, 3, p->sectors);
        expect_nd(b);
        assert(track_is(b, p, 3, 1, PATTERN));
        free_bus(b);
    }
    /* 順序入れ替え（R=10..18 → 11,10,...） */
    x68k_bus_t *b = new_bus_with(p);
    b->fdc.cur_cyl[0] = 0;
    make_ids(p, 0, 0);
    g_ids[2] = 11;
    g_ids[6] = 10;
    send_format(b, 0, 3, p->sectors);
    expect_nd(b);
    assert(track_is(b, p, 0, 0, PATTERN));
    free_bus(b);
    /* 標準2HDのID(R=1..8)を2HSへ送る */
    b = new_bus_with(p);
    make_ids(&fdc_profile_2hd, 0, 0);
    send_format(b, 0, 3, 8);
    expect_nd(b);
    assert(track_is(b, p, 0, 0, PATTERN));
    free_bus(b);
}

static void test_format_rejects_bad_params_and_wp(void) {
    const fdc_media_profile_t *p = &fdc_profile_2hs;
    x68k_bus_t *b = new_bus_with(p);
    make_ids(p, 0, 0);
    g_dma_calls = 0;
    send_format(b, 0, 3, 0); /* SC=0 */
    expect_nd(b);
    b->fdc.phase = FDC_IDLE;
    send_format(b, 0, 3, FDC_PROFILE_MAX_SECTORS + 1);
    expect_nd(b);
    b->fdc.phase = FDC_IDLE;
    send_format(b, 0, 8, 9); /* N>7 */
    expect_nd(b);
    assert(g_dma_calls == 0);
    assert(track_is(b, p, 0, 0, PATTERN));

    b->fdc.phase = FDC_IDLE;
    b->fdc.write_protect[0] = 1;
    send_format(b, 0, 3, 9);
    assert((b->fdc.res[0] & 0xC0u) == 0x40u && (b->fdc.res[1] & 0x02u)); /* NW */
    assert(g_dma_calls == 0);
    assert(track_is(b, p, 0, 0, PATTERN));

    b->fdc.phase = FDC_IDLE;
    b->fdc.write_protect[0] = 0;
    free(b->fdc.disk[0]);
    b->fdc.disk[0] = NULL;
    send_format(b, 0, 3, 9);
    assert(b->fdc.res[0] == 0x48u);          /* Not Ready */
    b->fdc.disk[0] = malloc(p->total_bytes); /* free_bus用 */
    free_bus(b);
}

/* 範囲外シリンダ: 標準2HDの予備シリンダは成功no-op、他媒体はND。 */
static void test_format_out_of_range_cylinder(void) {
    x68k_bus_t *b = new_bus_with(&fdc_profile_2hd);
    b->fdc.cur_cyl[0] = FDC_2HD_CYLS;
    make_ids(&fdc_profile_2hd, FDC_2HD_CYLS, 0);
    memset(b->fdc.ddam[0], 0xAA, sizeof(b->fdc.ddam[0]));
    send_format(b, 0, 3, 8);
    expect_ok(b);
    assert(track_is(b, &fdc_profile_2hd, FDC_2HD_CYLS - 1, 1, PATTERN));
    for (size_t i = 0; i < sizeof(b->fdc.ddam[0]); i++) /* 予備シリンダはDDAM不変 */
        assert(b->fdc.ddam[0][i] == 0xAAu);
    free_bus(b);

    b = new_bus_with(&fdc_profile_2hs);
    b->fdc.cur_cyl[0] = 80;
    make_ids(&fdc_profile_2hs, 80, 0);
    send_format(b, 0, 3, 9);
    expect_nd(b);
    free_bus(b);
}

/* フォーマットしたトラックのDDAMだけが通常データへ戻る。 */
static void test_format_clears_ddam(void) {
    const fdc_media_profile_t *p = &fdc_profile_2hs;
    x68k_bus_t *b = new_bus_with(p);
    memset(b->fdc.ddam[0], 0xFF, sizeof(b->fdc.ddam[0]));
    b->fdc.cur_cyl[0] = 2;
    make_ids(p, 2, 1);
    send_format(b, 1, 3, 9);
    expect_ok(b);
    uint32_t base = (uint32_t)(2 * p->heads + 1) * p->sectors;
    for (uint32_t i = 0; i < 1440; i++) {
        int bit = (b->fdc.ddam[0][i / 8] >> (i % 8)) & 1;
        assert(bit == !(i >= base && i < base + p->sectors));
    }
    free_bus(b);
}

/* FORMAT後に結果フェーズを読み切るとIDLEへ戻り、次コマンドを受け付ける。 */
static void test_format_result_phase_and_reset(void) {
    const fdc_media_profile_t *p = &fdc_profile_2hs;
    x68k_bus_t *b = new_bus_with(p);
    make_ids(p, 0, 0);
    send_format(b, 0, 3, 9);
    expect_ok(b);
    for (int i = 0; i < 7; i++)
        (void)fdc_read_data(&b->fdc);
    assert(b->fdc.phase == FDC_IDLE);
    assert(b->fdc.fmt_active == 0);
    fdc_reset(&b->fdc);
    assert(b->fdc.fmt_active == 0);
    free_bus(b);
}

/* ホストファイルのバイト列を読む（write-back検証用）。 */
static void read_host_file(const char *path, uint8_t *out, size_t n) {
    FILE *fp = fopen(path, "rb");
    assert(fp != NULL);
    assert(fread(out, 1, n, fp) == n);
    fclose(fp);
}

/* 標準2HD: 正常終了時はメモリ上のトラックに加え、ホストファイルへ書き戻される。
 * ライトプロテクト時はメモリ・ホストファイルとも不変（旧x68k_bus_fdc_format_track()
 * テストT28/T29の移行）。 */
static void test_format_2hd_writeback_and_wp(void) {
    const fdc_media_profile_t *p = &fdc_profile_2hd;
#ifdef _WIN32
    char path[300];
    win_get_temp_template(path, sizeof(path), "fdc_format_test.");
#else
    char path[] = "/tmp/fdc_format_test.XXXXXX";
#endif
    int fd = mkstemp(path);
    assert(fd >= 0);
    uint8_t *img = malloc(p->total_bytes);
    assert(img != NULL);
    memset(img, PATTERN, p->total_bytes);
    assert(write(fd, img, p->total_bytes) == (ssize_t)p->total_bytes);
    close(fd);
    free(img);

    x68k_bus_t *b = new_bus_with(p);
    b->fdc.disk_path[0] = strdup(path);
    assert(b->fdc.disk_path[0] != NULL);
    uint8_t on_disk[1024];

    b->fdc.write_protect[0] = 1;
    make_ids(p, 0, 0);
    send_format(b, 0, 3, 8);
    assert((b->fdc.res[0] & 0xC0u) == 0x40u && (b->fdc.res[1] & 0x02u)); /* NW */
    assert(track_is(b, p, 0, 0, PATTERN));
    read_host_file(path, on_disk, sizeof(on_disk));
    for (size_t i = 0; i < sizeof(on_disk); i++)
        assert(on_disk[i] == PATTERN);

    b->fdc.phase = FDC_IDLE;
    b->fdc.write_protect[0] = 0;
    send_format(b, 0, 3, 8);
    expect_ok(b);
    assert(track_is(b, p, 0, 0, FORMAT_FILL));
    read_host_file(path, on_disk, sizeof(on_disk));
    for (size_t i = 0; i < sizeof(on_disk); i++)
        assert(on_disk[i] == FORMAT_FILL);

    free(b->fdc.disk_path[0]);
    free_bus(b);
    remove(path);
}

int main(void) {
    test_format_all_profiles();
    test_format_2hs_boot_track();
    test_format_2hde_boot_track();
    test_format_2ht_boot_track();
    test_format_2hdez_transient_boot_track();
    test_format_rejects_mismatch();
    test_format_rejects_bad_params_and_wp();
    test_format_out_of_range_cylinder();
    test_format_clears_ddam();
    test_format_result_phase_and_reset();
    test_format_2hd_writeback_and_wp();
    printf("fdc_format_test: all passed\n");
    return 0;
}
