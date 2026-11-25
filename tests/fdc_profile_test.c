/*
 * fdc.c 媒体プロファイルの単体テスト。
 * bus/CPU非依存: dmac_ch0_fdc_transfer()はスタブ化する。
 */
#include "bus/bus_internal.h"
#include <assert.h>
#include <stdio.h>

#include <stdlib.h>
#include <string.h>

static uint8_t *g_read_buf = NULL; /* READ系がホストへ供給する先 */

/* 実DMACの汎用転送エンジン相当: READ方向のみを模擬する（WRITE系は使わない）。 */
int dmac_ch0_fdc_transfer(x68k_bus_t *b) {
    fdc_t *f = &b->fdc;
    assert(f->dma_dir == 1);
    uint32_t n = f->dma_remaining; /* fdc_read_data()が消費して減るため先に控える */
    for (uint32_t i = 0; i < n; i++)
        g_read_buf[i] = fdc_read_data(f);
    return 1;
}

/* 既存の標準2HDオフセット式（fdc.cのREAD/WRITE/SCAN）と全セクタで一致する。 */
static void test_2hd_matches_legacy_formula(void) {
    const fdc_media_profile_t *p = &fdc_profile_2hd;
    uint32_t expect_index = 0;
    assert(p->total_bytes == 1261568u);
    for (int cyl = 0; cyl < FDC_2HD_CYLS; cyl++) {
        for (int hd = 0; hd < FDC_2HD_HEADS; hd++) {
            for (int sec = 1; sec <= 8; sec++) {
                fdc_sector_loc_t loc;
                assert(fdc_profile_locate(p, cyl, hd, hd, sec, 3, &loc) == 0);
                assert(loc.offset == (uint32_t)((cyl * 2 + hd) * 8 + (sec - 1)) * 1024u);
                assert(loc.length == 1024u);
                assert(loc.index == expect_index++);
            }
        }
    }
    assert(expect_index == FDC_2HD_SECTORS);
}

static void test_2hd_rejects_out_of_range(void) {
    const fdc_media_profile_t *p = &fdc_profile_2hd;
    fdc_sector_loc_t loc = {0xAAAAAAAAu, 0xBBBBBBBBu, 0xCCCCCCCCu};
    assert(fdc_profile_locate(p, FDC_2HD_CYLS, 0, 0, 1, 3, &loc) == -1);
    assert(fdc_profile_locate(p, -1, 0, 0, 1, 3, &loc) == -1);
    assert(fdc_profile_locate(p, 0, 2, 2, 1, 3, &loc) == -1);
    assert(fdc_profile_locate(p, 0, 0, 1, 1, 3, &loc) == -1); /* ID Hが物理Hと不一致 */
    assert(fdc_profile_locate(p, 0, 0, 0, 0, 3, &loc) == -1);
    assert(fdc_profile_locate(p, 0, 0, 0, 9, 3, &loc) == -1);
    assert(fdc_profile_locate(p, 0, 0, 0, 1, 2, &loc) == -1); /* Nが媒体IDと不一致 */
    assert(fdc_profile_locate(NULL, 0, 0, 0, 1, 3, &loc) == -1);
    assert(fdc_profile_locate(p, 0, 0, 0, 1, 3, NULL) == -1);
    assert(loc.offset == 0xAAAAAAAAu && loc.length == 0xBBBBBBBBu && loc.index == 0xCCCCCCCCu);
}

/* 9セクタ/トラックのテスト用プロファイル（2HS相当、総1440セクタ）。 */
static fdc_media_profile_t make_9sec_profile(void) {
    fdc_media_profile_t p = {.name = "T9",
                             .cyls = 80,
                             .heads = 2,
                             .sectors = 9,
                             .track_bytes = 9 * 1024,
                             .total_bytes = 80 * 2 * 9 * 1024};
    for (int i = 0; i < 9; i++)
        p.sec[i] = (fdc_sector_entry_t){(uint8_t)(10 + i), 3, (uint16_t)(i * 1024), 1024};
    return p;
}

static void test_sector_count_and_9sec_locate(void) {
    fdc_media_profile_t p = make_9sec_profile();
    fdc_sector_loc_t loc;
    assert(fdc_profile_sector_count(&fdc_profile_2hd) == FDC_2HD_SECTORS);
    assert(fdc_profile_sector_count(&p) == 1440u);
    assert(fdc_profile_sector_count(NULL) == 0u);
    assert(fdc_profile_locate(&p, 79, 1, 1, 18, 3, &loc) == 0);
    assert(loc.index == 1439u);
    assert(loc.offset == 1474560u - 1024u);
    assert(fdc_profile_locate(&p, 0, 0, 0, 1, 3, &loc) == -1); /* R=1は2HS系に存在しない */
}

/* 1232を超えるセクタ番号でもDDAMのトラック単位クリアが正しい範囲に効く。 */
static void test_ddam_clear_track_9sec(void) {
    static fdc_t f; /* ゼロ初期化 */
    fdc_media_profile_t p = make_9sec_profile();
    f.profile[0] = &p;
    for (int i = 0; i < FDC_DDAM_MAX_BYTES; i++)
        f.ddam[0][i] = 0xFFu;
    fdc_ddam_clear_track(&f, 0, 79, 1); /* index 1431..1439 */
    for (uint32_t i = 0; i < FDC_DDAM_MAX_SECTORS; i++) {
        int bit = (f.ddam[0][i / 8] >> (i % 8)) & 1;
        assert(bit == !(i >= 1431u && i <= 1439u));
    }
    fdc_ddam_clear_track(&f, 0, 80, 0); /* 範囲外は無操作 */
    fdc_ddam_clear_track(&f, 0, 0, 2);
    assert(((f.ddam[0][0] >> 0) & 1) == 1);
    fdc_ddam_clear_drive(&f, 0);
    for (int i = 0; i < FDC_DDAM_MAX_BYTES; i++)
        assert(f.ddam[0][i] == 0);
}

/* ---- プロファイル定義の整合性とFDCコマンド経由の動作（2HS/2HDE/2HT） ---- */

static void check_profile_consistency(const fdc_media_profile_t *p) {
    uint32_t next = 0;
    assert(p->sectors >= 1 && p->sectors <= FDC_PROFILE_MAX_SECTORS);
    for (int i = 0; i < p->sectors; i++) {
        assert(p->sec[i].byte_offset == next); /* トラック内で隙間・重なり無し */
        assert(p->sec[i].byte_length == (128u << p->sec[i].n));
        next += p->sec[i].byte_length;
    }
    assert(next == p->track_bytes);
    if (p->boot_track_sectors) { /* (C0,H0)の別配置はトラック枠に収まり隙間・重なり無し */
        uint32_t boot_next = 0;
        assert(p->boot_track_sectors <= FDC_PROFILE_MAX_SECTORS);
        for (int i = 0; i < p->boot_track_sectors; i++) {
            assert(p->boot_track_sec[i].byte_offset == boot_next);
            assert(p->boot_track_sec[i].byte_length == (128u << p->boot_track_sec[i].n));
            boot_next += p->boot_track_sec[i].byte_length;
        }
        assert(boot_next <= p->track_bytes);
    }
    assert(p->total_bytes == (uint32_t)p->cyls * p->heads * p->track_bytes);
    assert(fdc_profile_sector_count(p) <= FDC_DDAM_MAX_SECTORS);
}

static void test_profile_definitions(void) {
    check_profile_consistency(&fdc_profile_2hd);
    check_profile_consistency(&fdc_profile_2hs);
    check_profile_consistency(&fdc_profile_2hde);
    check_profile_consistency(&fdc_profile_2ht);
    check_profile_consistency(&fdc_profile_2hc);
    check_profile_consistency(&fdc_profile_2dd9);
    check_profile_consistency(&fdc_profile_2dd8);
    check_profile_consistency(&fdc_profile_2hq);
    check_profile_consistency(&fdc_profile_2hdez);
    assert(fdc_profile_2hc.total_bytes == 1228800u);
    assert(fdc_profile_2dd9.total_bytes == 737280u);
    assert(fdc_profile_2dd8.total_bytes == 655360u);
    assert(fdc_profile_2hq.total_bytes == 1474560u);
    assert(fdc_profile_2hdez.total_bytes == 1474560u);
    assert(fdc_profile_2hd.total_bytes == 1261568u);
    assert(fdc_profile_2hs.total_bytes == 1474560u);
    assert(fdc_profile_2hde.total_bytes == 1474560u);
    assert(fdc_profile_2ht.total_bytes == 1638400u);
}

static void fill_pattern(uint8_t *img, uint32_t bytes) {
    for (uint32_t i = 0; i < bytes; i++)
        img[i] = (uint8_t)((i * 2654435761u) >> 24);
}

static x68k_bus_t *new_bus_with(const fdc_media_profile_t *p) {
    x68k_bus_t *b = calloc(1, sizeof(*b));
    assert(b != NULL);
    b->fdc.bus = b;
    b->fdc.disk[0] = malloc(p->total_bytes);
    assert(b->fdc.disk[0] != NULL);
    fill_pattern(b->fdc.disk[0], p->total_bytes);
    b->fdc.disk_bytes[0] = (int)p->total_bytes;
    b->fdc.profile[0] = p;
    /* ドライブ0を媒体の転送速度に合わせる */
    fdc_set_density(&b->fdc, p->rate_kbps == FDC_RATE_250_KBPS);
    /* 300rpm媒体（2HQ）はドライブ0とドライブ1の選択ビットを同時アサート */
    fdc_set_option(&b->fdc, p->rpm300 ? 0x03u : 0x00u);
    return b;
}

static void free_bus(x68k_bus_t *b) {
    free(b->fdc.disk[0]);
    free(b);
}

/* READ(0x06)/READ DELETED(0x0C)を発行する。結果はb->fdc.res[]、データはg_read_bufへ。 */
static void send_read(x68k_bus_t *b, uint8_t opcode, int hd, int c, int h, int r, int n) {
    static uint8_t ram[16];
    static uint8_t regs[32];
    fdc_t *f = &b->fdc;
    const uint8_t cmd[9] = {opcode,     (uint8_t)((hd & 1) << 2),
                            (uint8_t)c, (uint8_t)h,
                            (uint8_t)r, (uint8_t)n,
                            (uint8_t)r, 0x35u,
                            0xFFu};
    for (int i = 0; i < 9; i++)
        fdc_write_data(f, ram, regs, cmd[i]);
    f->phase = FDC_IDLE;
}

#define RES_ST0_ND(b) (((b)->fdc.res[0] & 0xC0u) == 0x40u && ((b)->fdc.res[1] & 0x04u))

/* READ IDはトラックのセクタを順に返し、最後の次は先頭へ戻る。 */
static void test_read_id_per_profile(void) {
    static uint8_t ram[16];
    static uint8_t regs[32];
    const fdc_media_profile_t *ps[] = {&fdc_profile_2hd, &fdc_profile_2hs, &fdc_profile_2hde,
                                       &fdc_profile_2ht, &fdc_profile_2hc, &fdc_profile_2hq};
    for (size_t k = 0; k < sizeof(ps) / sizeof(ps[0]); k++) {
        for (int hd = 0; hd < 2; hd++) {
            x68k_bus_t *b = new_bus_with(ps[k]);
            const fdc_sector_entry_t *sec;
            int count = fdc_profile_track_sectors(ps[k], 0, hd, &sec);
            for (int i = 0; i < count * 2 + 1; i++) {
                int pos = i % count;
                fdc_write_data(&b->fdc, ram, regs, 0x4Au); /* MFM|READ ID */
                fdc_write_data(&b->fdc, ram, regs, (uint8_t)(hd << 2));
                assert(b->fdc.res[4] == fdc_profile_id_h(ps[k], 0, hd, pos));
                assert(b->fdc.res[5] == fdc_profile_id_r(ps[k], 0, hd, pos));
                assert(b->fdc.res[6] == sec[pos].n);
                b->fdc.phase = FDC_IDLE;
            }
            free_bus(b);
        }
    }
}

static void test_2hs_read_and_ddam(void) {
    uint8_t buf[1024];
    g_read_buf = buf;
    x68k_bus_t *b = new_bus_with(&fdc_profile_2hs);
    const uint32_t last = 1474560u - 1024u; /* cyl79 H1 R18 */

    send_read(b, 0x06, 0, 0, 0, 1, 3); /* (C0,H0)の先頭はIPLのR=1（boot_r） */
    assert(b->fdc.res[0] == 0u && memcmp(buf, b->fdc.disk[0], 1024) == 0);
    send_read(b, 0x06, 0, 0, 0, 10, 3); /* (C0,H0)にはR=10が存在しない */
    assert(RES_ST0_ND(b));
    send_read(b, 0x06, 0, 0, 0, 11, 3); /* 2番目以降は通常どおりR=11.. */
    assert(b->fdc.res[0] == 0u && memcmp(buf, b->fdc.disk[0] + 1024u, 1024) == 0);
    send_read(b, 0x06, 1, 0, 1, 10, 3); /* (C0,H1)の先頭は通常のR=10 */
    assert(b->fdc.res[0] == 0u && memcmp(buf, b->fdc.disk[0] + 9u * 1024u, 1024) == 0);
    send_read(b, 0x06, 1, 79, 1, 18, 3); /* 末尾 */
    assert(b->fdc.res[0] == 0u && memcmp(buf, b->fdc.disk[0] + last, 1024) == 0);
    send_read(b, 0x06, 0, 5, 0, 1, 3); /* R=1は(C0,H0)以外の2HSに存在しない */
    assert(RES_ST0_ND(b));
    send_read(b, 0x06, 0, 80, 0, 10, 3); /* 範囲外シリンダ */
    assert(RES_ST0_ND(b));

    /* 末尾セクタ（通しセクタ番号1439）のDDAMビット。標準2HDの1232を超える。 */
    b->fdc.ddam[0][1439 / 8] |= (uint8_t)(1u << (1439 % 8));
    send_read(b, 0x0C, 1, 79, 1, 18, 3);
    assert(b->fdc.res[0] == 0u && b->fdc.res[2] == 0u); /* 削除データとして読める */
    send_read(b, 0x06, 1, 79, 1, 18, 3);
    assert(b->fdc.res[2] == 0x40u); /* 通常READは種別不一致でCM */
    free_bus(b);
}

static void test_2hde_id_head(void) {
    uint8_t buf[1024];
    g_read_buf = buf;
    x68k_bus_t *b = new_bus_with(&fdc_profile_2hde);
    send_read(b, 0x06, 0, 0, 0, 1, 3); /* (C0,H0)の先頭はIPL: H=0,R=1（オフセット無し） */
    assert(b->fdc.res[0] == 0u && memcmp(buf, b->fdc.disk[0], 1024) == 0);
    send_read(b, 0x06, 0, 0, 128, 1, 3); /* (C0,H0)にH=128,R=1は無い */
    assert(RES_ST0_ND(b));
    send_read(b, 0x06, 0, 0, 128, 2, 3); /* 2番目以降はH=128,R=2.. */
    assert(b->fdc.res[0] == 0u && memcmp(buf, b->fdc.disk[0] + 1024u, 1024) == 0);
    send_read(b, 0x06, 0, 0, 0, 2, 3); /* 2番目のIDのHは標準値ではない */
    assert(RES_ST0_ND(b));
    send_read(b, 0x06, 1, 0, 129, 1, 3); /* 物理H1はファイル内で次のトラック */
    assert(b->fdc.res[0] == 0u && memcmp(buf, b->fdc.disk[0] + 9u * 1024u, 1024) == 0);
    send_read(b, 0x06, 0, 1, 0, 1, 3); /* (C1,H0)のIDのHは128 */
    assert(RES_ST0_ND(b));
    send_read(b, 0x06, 0, 0, 129, 2, 3); /* 物理HとID Hの対応が違う */
    assert(RES_ST0_ND(b));
    free_bus(b);
}

/* 512バイト/セクタ(N=2)の媒体: R=1始まり、IDのHは物理ヘッド、(C0,H0)の特例なし。 */
static void test_512byte_media(const fdc_media_profile_t *p, int sectors) {
    static uint8_t buf[512];
    g_read_buf = buf;
    x68k_bus_t *b = new_bus_with(p);
    send_read(b, 0x06, 0, 0, 0, 1, 2);
    assert(b->fdc.res[0] == 0u && memcmp(buf, b->fdc.disk[0], 512) == 0);
    send_read(b, 0x06, 1, 79, 1, sectors, 2); /* 末尾 */
    assert(b->fdc.res[0] == 0u &&
           memcmp(buf, b->fdc.disk[0] + p->total_bytes - 512u, 512) == 0);
    send_read(b, 0x06, 0, 0, 0, sectors + 1, 2); /* 存在しないR */
    assert(RES_ST0_ND(b));
    send_read(b, 0x06, 0, 0, 0, 1, 3); /* Nが媒体IDと不一致 */
    assert(RES_ST0_ND(b));
    send_read(b, 0x06, 0, 80, 0, 1, 2); /* 範囲外シリンダ */
    assert(RES_ST0_ND(b));
    free_bus(b);
}

static void test_512byte_profiles(void) {
    test_512byte_media(&fdc_profile_2hc, 15);
    test_512byte_media(&fdc_profile_2dd9, 9);
    test_512byte_media(&fdc_profile_2dd8, 8);
    test_512byte_media(&fdc_profile_2hq, 18);
}

/* 2HDEZ: 最終配置は(C0,H0)の特例が無いH=128/129,R=1..9（2HDEとの違い）。 */
static void test_2hdez_plain_layout(void) {
    uint8_t buf[1024];
    g_read_buf = buf;
    x68k_bus_t *b = new_bus_with(&fdc_profile_2hdez);
    send_read(b, 0x06, 0, 0, 128, 1, 3); /* (C0,H0)の先頭もH=128,R=1 */
    assert(b->fdc.res[0] == 0u && memcmp(buf, b->fdc.disk[0], 1024) == 0);
    send_read(b, 0x06, 0, 0, 0, 1, 3); /* 2HDEの特例配置(H=0,R=1)は読み出せない */
    assert(RES_ST0_ND(b));
    send_read(b, 0x06, 1, 0, 129, 9, 3);
    assert(b->fdc.res[0] == 0u && memcmp(buf, b->fdc.disk[0] + 17u * 1024u, 1024) == 0);
    free_bus(b);
}

static void test_2ht_variable_sectors(void) {
    static uint8_t buf[8192];
    g_read_buf = buf;
    x68k_bus_t *b = new_bus_with(&fdc_profile_2ht);
    const uint32_t trk = 10240u;

    send_read(b, 0x06, 0, 1, 0, 1, 4); /* cyl1 H0(トラック2) R=1,N=4: 2048byte */
    assert(b->fdc.res[0] == 0u && memcmp(buf, b->fdc.disk[0] + 2u * trk, 2048) == 0);
    send_read(b, 0x06, 0, 1, 0, 2, 6); /* R=2,N=6: 8192byte。境界オフセット2048 */
    assert(b->fdc.res[0] == 0u && memcmp(buf, b->fdc.disk[0] + 2u * trk + 2048, 8192) == 0);
    send_read(b, 0x06, 1, 3, 1, 2, 6); /* cyl3 H1: トラック番号7 */
    assert(b->fdc.res[0] == 0u && memcmp(buf, b->fdc.disk[0] + 7u * trk + 2048, 8192) == 0);
    send_read(b, 0x06, 1, 79, 1, 2, 6); /* 末尾はイメージ終端ちょうど */
    assert(b->fdc.res[0] == 0u && memcmp(buf, b->fdc.disk[0] + 1638400u - 8192u, 8192) == 0);

    /* (C0,H0)は2HSと同じ配置: R=1,11..18 の9セクタ x N=3 */
    send_read(b, 0x06, 0, 0, 0, 1, 3);
    assert(b->fdc.res[0] == 0u && memcmp(buf, b->fdc.disk[0], 1024) == 0);
    send_read(b, 0x06, 0, 0, 0, 18, 3);
    assert(b->fdc.res[0] == 0u && memcmp(buf, b->fdc.disk[0] + 8u * 1024u, 1024) == 0);
    send_read(b, 0x06, 0, 0, 0, 1, 4); /* 通常トラックのIDは(C0,H0)に無い */
    assert(RES_ST0_ND(b));
    send_read(b, 0x06, 0, 0, 0, 2, 6);
    assert(RES_ST0_ND(b));
    send_read(b, 0x06, 1, 0, 1, 1, 3); /* 他のトラックに2HS配置は無い */
    assert(RES_ST0_ND(b));

    send_read(b, 0x06, 0, 1, 0, 1, 3); /* Nが媒体IDと不一致 */
    assert(RES_ST0_ND(b));
    send_read(b, 0x06, 0, 1, 0, 1, 6);
    assert(RES_ST0_ND(b));
    send_read(b, 0x06, 0, 1, 0, 3, 4); /* 存在しないR */
    assert(RES_ST0_ND(b));
    free_bus(b);
}

int main(void) {
    test_2hd_matches_legacy_formula();
    test_2hd_rejects_out_of_range();
    test_sector_count_and_9sec_locate();
    test_ddam_clear_track_9sec();
    test_profile_definitions();
    test_read_id_per_profile();
    test_2hs_read_and_ddam();
    test_2hde_id_head();
    test_512byte_profiles();
    test_2hdez_plain_layout();
    test_2ht_variable_sectors();
    printf("fdc_profile_test: OK\n");
    return 0;
}
