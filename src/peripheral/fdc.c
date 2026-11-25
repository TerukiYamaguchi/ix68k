#include "bus/bus_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void fdc_media_changed(fdc_t *fdc) {
    if (fdc->media_gen == UINT64_MAX) {
        fdc->media_gen_overflow = true;
        return;
    }
    fdc->media_gen++;
}

/* ============================================================
 * FDC (µPD72065B) 最小実装
 * ============================================================
 *
 * X68000 2HD フォーマット: 77 cyl × 2 head × 8 sec × 1024 bytes = 1,261,568 bytes
 * XDF はこのレイアウトのまま生ダンプ。
 * セクタオフセット = ((cyl*2+head)*8 + (sec-1)) * sec_size
 *
 * FDC レジスタ（µPD72065B は D0-D7 接続、チップ A0 = バス A1）:
 *   $E94001 (FDC+0x0001): Main Status Register (MSR) — 読み出し専用 (A1=0)
 *   $E94003 (FDC+0x0003): Data Register — 読み書き (A1=1)
 *   $E94005 (FDC+0x0005): Drive Status (読み)
 *   $E94007 (FDC+0x0007): Drive Select (書き)
 *
 * DMA (HD63450) channel 0 MAR: $E8400D/$E8400E/$E8400F
 *   → crtc_regs[0x400D] / [0x400E] / [0x400F]
 * READ/WRITE DATA実行時の実転送は通常dmac_ch0_fdc_transfer()（dmac.c）が
 * 汎用エンジン経由で行う。fdc_drain_exec_phase()はCCR STARTで武装されて
 * いない異常系のみのフォールバック（下記参照）。
 *
 * SCAN EQUAL(opcode 0x11、IOCS _B_VERIFY用)はWRITE DATAと同じDMA方向
 * （ホスト→FDC）だが、ディスクへは書き込まず記録データと目的データを
 * 1バイトずつ比較する。dma_cmpフラグで区別する。
 */

static int fdc_cmd_len_tab(uint8_t b0) {
    switch (b0 & 0x1F) {
    case 0x02:
        return 9; /* READ TRACK */
    case 0x03:
        return 3; /* SPECIFY */
    case 0x04:
        return 2; /* SENSE DRIVE STATUS */
    case 0x05:
        return 9; /* WRITE DATA */
    case 0x06:
        return 9; /* READ DATA */
    case 0x07:
        return 2; /* RECALIBRATE */
    case 0x08:
        return 1; /* SENSE INTERRUPT STATUS */
    case 0x09:
        return 9; /* WRITE DELETED DATA */
    case 0x0A:
        return 2; /* READ ID */
    case 0x0C:
        return 9; /* READ DELETED DATA */
    case 0x0D:
        return 6; /* FORMAT TRACK */
    case 0x0F:
        return 3; /* SEEK */
    case 0x11:
        return 9; /* SCAN EQUAL。IPLROMXV $FF8B90 の moveq #$08,d0 で9バイト送出を実測 */
    case 0x19:
        return 9; /* SCAN LOW OR EQUAL（実行は未実装。長さのみ登録して
                   * コマンドフェーズを完走させ、default:のInvalid Command応答へ
                   * 落とす。長さ1のままだと2バイト目の送出待ちでCBが下りず
                   * ホスト側が無限ループする） */
    case 0x1D:
        return 9; /* SCAN HIGH OR EQUAL（同上） */
    default:
        return 1;
    }
}

/* X68000 FDC unit番号 → disk配列インデックス変換。
 * ROM逆アセンブル（$FF93FC: unit = PDA & 3）と『Inside X68000』
 * （$E94005/$E94007とも「内蔵ドライブは0と1」と明記、スクランブルの記載なし）
 * の両方でunit==driveの恒等対応と確認済み（スクランブル無し）。ここで
 * unit!=driveのスクランブルを入れると、_B_DRVSNS経由のブートプローブ
 * （SENSE DRIVE STATUSでunit=0を送り、内蔵FD0=drive0の応答を期待する）と
 * 食い違い、ROM委譲時の起動が失敗する。 */
static int fdc_unit_to_drive(int unit) {
    return unit & 3;
}

/* 標準2HD: 77cyl x 2head x 8sec x 1024byte、R=1..8、N=3。 */
const fdc_media_profile_t fdc_profile_2hd = {
    .name = "2HD",
    .rate_kbps = FDC_RATE_500_KBPS,
    .cyls = FDC_2HD_CYLS,
    .heads = FDC_2HD_HEADS,
    .id_h_offset = 0,
    .sectors = 8,
    .track_bytes = 8 * 1024,
    .total_bytes = FDC_2HD_CYLS * FDC_2HD_HEADS * 8 * 1024,
    .sec = {{1, 3, 0 * 1024, 1024},
            {2, 3, 1 * 1024, 1024},
            {3, 3, 2 * 1024, 1024},
            {4, 3, 3 * 1024, 1024},
            {5, 3, 4 * 1024, 1024},
            {6, 3, 5 * 1024, 1024},
            {7, 3, 6 * 1024, 1024},
            {8, 3, 7 * 1024, 1024}},
};

/* 9SCSET系の媒体（一次資料: extern/9scdrv/9SCSET_/9SCCALL2/9SCCALL.DOC の
 * 「物理フォーマット仕様」）。
 * 80cyl x 2head、1トラック9セクタ x 1024byte（N=3）の2HS/2HDEは総1,474,560バイト。 */
#define FDC_9SC_CYLS         80
#define FDC_9SC_SECTORS_9    9
#define FDC_9SC_SECTOR_BYTES 1024
#define FDC_9SC_TRACK_BYTES  (FDC_9SC_SECTORS_9 * FDC_9SC_SECTOR_BYTES)
#define FDC_9SC_TOTAL_BYTES  (FDC_9SC_CYLS * FDC_2HD_HEADS * FDC_9SC_TRACK_BYTES)
#define FDC_9SC_SECTOR(r, i) \
    {(r), 3, (uint16_t)((i) * FDC_9SC_SECTOR_BYTES), FDC_9SC_SECTOR_BYTES}

/* 2HS: IDのセクタ番号を10..18にして標準2HDと区別する。ただしシリンダ0・ヘッド0の
 * 先頭セクタ(IPL)だけは標準2HDと同じR=1（9SCFMT.Xの実FORMAT TRACKで確認。BR32D.2HSの
 * 生ダンプ先頭も9SCFMT IPL）。 */
const fdc_media_profile_t fdc_profile_2hs = {
    .name = "2HS",
    .rate_kbps = FDC_RATE_500_KBPS,
    .cyls = FDC_9SC_CYLS,
    .heads = FDC_2HD_HEADS,
    .id_h_offset = 0,
    .boot_r = 1,
    .sectors = FDC_9SC_SECTORS_9,
    .track_bytes = FDC_9SC_TRACK_BYTES,
    .total_bytes = FDC_9SC_TOTAL_BYTES,
    .sec = {FDC_9SC_SECTOR(10, 0), FDC_9SC_SECTOR(11, 1), FDC_9SC_SECTOR(12, 2),
            FDC_9SC_SECTOR(13, 3), FDC_9SC_SECTOR(14, 4), FDC_9SC_SECTOR(15, 5),
            FDC_9SC_SECTOR(16, 6), FDC_9SC_SECTOR(17, 7), FDC_9SC_SECTOR(18, 8)},
};

/* 2HDE（新形式）: IDのサーフェス番号（H）を0->128、1->129にして区別する。
 * ただしシリンダ0・ヘッド0の先頭セクタ(IPL)だけは標準2HDと同じH=0,R=1（9SCFMT.Xの
 * 実FORMAT TRACKで確認）。R=2..9は資料が変更点をHのみと述べていることと、実
 * FORMAT TRACKの出力(R=2..9,H=128)で確認済み。 */
const fdc_media_profile_t fdc_profile_2hde = {
    .name = "2HDE",
    .rate_kbps = FDC_RATE_500_KBPS,
    .cyls = FDC_9SC_CYLS,
    .heads = FDC_2HD_HEADS,
    .id_h_offset = 128,
    .boot_r = 1,
    .sectors = FDC_9SC_SECTORS_9,
    .track_bytes = FDC_9SC_TRACK_BYTES,
    .total_bytes = FDC_9SC_TOTAL_BYTES,
    .sec = {FDC_9SC_SECTOR(1, 0), FDC_9SC_SECTOR(2, 1), FDC_9SC_SECTOR(3, 2),
            FDC_9SC_SECTOR(4, 3), FDC_9SC_SECTOR(5, 4), FDC_9SC_SECTOR(6, 5),
            FDC_9SC_SECTOR(7, 6), FDC_9SC_SECTOR(8, 7), FDC_9SC_SECTOR(9, 8)},
};

/* 2HT: 1トラックに可変長2セクタ（R=1,N=4: 2048byte、R=2,N=6: 8192byte、計10240byte）。
 * 一次資料は 2HTCPY/2HTCPY.S の _2HT_ID/_2HT_ID2 と各トラックのREAD/WRITE。 */
#define FDC_2HT_TRACK_BYTES (2048 + 8192)
const fdc_media_profile_t fdc_profile_2ht = {
    .name = "2HT",
    .rate_kbps = FDC_RATE_500_KBPS,
    .cyls = FDC_9SC_CYLS,
    .heads = FDC_2HD_HEADS,
    .id_h_offset = 0,
    .sectors = 2,
    .track_bytes = FDC_2HT_TRACK_BYTES,
    .total_bytes = FDC_9SC_CYLS * FDC_2HD_HEADS * FDC_2HT_TRACK_BYTES,
    .sec = {{1, 4, 0, 2048}, {2, 6, 2048, 8192}},
    /* (C0,H0)はIPL用に2HSと同じ配置（9SCFMT.Xの実FORMAT TRACKで確認）。全容量1599K
     * （仕様書の表）= 1600K - 1K(R=10相当の1セクタ)とも整合する。 */
    .boot_track_sectors = FDC_9SC_SECTORS_9,
    .boot_track_sec = {FDC_9SC_SECTOR(1, 0), FDC_9SC_SECTOR(11, 1), FDC_9SC_SECTOR(12, 2),
                       FDC_9SC_SECTOR(13, 3), FDC_9SC_SECTOR(14, 4), FDC_9SC_SECTOR(15, 5),
                       FDC_9SC_SECTOR(16, 6), FDC_9SC_SECTOR(17, 7), FDC_9SC_SECTOR(18, 8)},
};

/* 512バイト/セクタ(N=2)の媒体。R=1始まり、IDのHは物理ヘッド。9SCFMT.Xの実出力で確認
 * （2HCはIOCS _B_FORMAT経由、2DD/9・2DD/8・2HQはFDC FORMAT TRACK）。(C0,H0)の特例は無い。 */
#define FDC_512_SECTOR_BYTES 512
#define FDC_512_SECTOR(r, i) \
    {(r), 2, (uint16_t)((i) * FDC_512_SECTOR_BYTES), FDC_512_SECTOR_BYTES}
#define FDC_512_MEDIA(nm, rate_, rpm300_, nsec, ...)                                 \
    {                                                                                \
        .name = (nm),                                                                \
        .rate_kbps = (rate_),                                                        \
        .rpm300 = (rpm300_),                                                         \
        .cyls = FDC_9SC_CYLS,                                                        \
        .heads = FDC_2HD_HEADS,                                                      \
        .id_h_offset = 0,                                                            \
        .sectors = (nsec),                                                           \
        .track_bytes = (nsec) * FDC_512_SECTOR_BYTES,                                \
        .total_bytes = FDC_9SC_CYLS * FDC_2HD_HEADS * (nsec) * FDC_512_SECTOR_BYTES, \
        .sec = {__VA_ARGS__},                                                        \
    }

const fdc_media_profile_t fdc_profile_2hc = FDC_512_MEDIA(
    "2HC", FDC_RATE_500_KBPS, 0, 15, FDC_512_SECTOR(1, 0), FDC_512_SECTOR(2, 1),
    FDC_512_SECTOR(3, 2), FDC_512_SECTOR(4, 3), FDC_512_SECTOR(5, 4), FDC_512_SECTOR(6, 5),
    FDC_512_SECTOR(7, 6), FDC_512_SECTOR(8, 7), FDC_512_SECTOR(9, 8), FDC_512_SECTOR(10, 9),
    FDC_512_SECTOR(11, 10), FDC_512_SECTOR(12, 11), FDC_512_SECTOR(13, 12),
    FDC_512_SECTOR(14, 13), FDC_512_SECTOR(15, 14));

const fdc_media_profile_t fdc_profile_2dd9 = FDC_512_MEDIA(
    "2DD/9", FDC_RATE_250_KBPS, 0, 9, FDC_512_SECTOR(1, 0), FDC_512_SECTOR(2, 1),
    FDC_512_SECTOR(3, 2), FDC_512_SECTOR(4, 3), FDC_512_SECTOR(5, 4), FDC_512_SECTOR(6, 5),
    FDC_512_SECTOR(7, 6), FDC_512_SECTOR(8, 7), FDC_512_SECTOR(9, 8));

const fdc_media_profile_t fdc_profile_2dd8 =
    FDC_512_MEDIA("2DD/8", FDC_RATE_250_KBPS, 0, 8, FDC_512_SECTOR(1, 0), FDC_512_SECTOR(2, 1),
                  FDC_512_SECTOR(3, 2), FDC_512_SECTOR(4, 3), FDC_512_SECTOR(5, 4),
                  FDC_512_SECTOR(6, 5), FDC_512_SECTOR(7, 6), FDC_512_SECTOR(8, 7));

/* 2HQ(IBM 1.44MB): 回転数・転送速度が2HDと違い、内蔵ドライブの9SCFMT.Xは
 * 「このドライブではIBMフォーマットできません」で中止するため、イメージの読み書きのみ対応。
 * IDは(C0,H0)の1トラックのみ実出力で確認（N=2,SC=18,R=1..18）。 */
const fdc_media_profile_t fdc_profile_2hq = FDC_512_MEDIA(
    "2HQ", FDC_RATE_500_KBPS, 1, 18, FDC_512_SECTOR(1, 0), FDC_512_SECTOR(2, 1),
    FDC_512_SECTOR(3, 2), FDC_512_SECTOR(4, 3), FDC_512_SECTOR(5, 4), FDC_512_SECTOR(6, 5),
    FDC_512_SECTOR(7, 6), FDC_512_SECTOR(8, 7), FDC_512_SECTOR(9, 8), FDC_512_SECTOR(10, 9),
    FDC_512_SECTOR(11, 10), FDC_512_SECTOR(12, 11), FDC_512_SECTOR(13, 12),
    FDC_512_SECTOR(14, 13), FDC_512_SECTOR(15, 14), FDC_512_SECTOR(16, 15),
    FDC_512_SECTOR(17, 16), FDC_512_SECTOR(18, 17));

/* 2HDEZ（2HDE98: FDISK.SYS v0.98互換）: 最終的な配置は(C0,H0)の特例が無い素直な2HDE
 * （H=128/129,R=1..9）。9SCFMT.X(Z)は全160トラックを2HDE同様の(C0,H0)特例付きで書いた後、
 * 最後に(C0,H0)をH=128,R=1..9へ書き直す。その途中の特例配置もFORMAT TRACKは受理する。 */
const fdc_media_profile_t fdc_profile_2hdez = {
    .name = "2HDEZ",
    .rate_kbps = FDC_RATE_500_KBPS,
    .cyls = FDC_9SC_CYLS,
    .heads = FDC_2HD_HEADS,
    .id_h_offset = 128,
    .format_boot_r = 1,
    .sectors = FDC_9SC_SECTORS_9,
    .track_bytes = FDC_9SC_TRACK_BYTES,
    .total_bytes = FDC_9SC_TOTAL_BYTES,
    .sec = {FDC_9SC_SECTOR(1, 0), FDC_9SC_SECTOR(2, 1), FDC_9SC_SECTOR(3, 2),
            FDC_9SC_SECTOR(4, 3), FDC_9SC_SECTOR(5, 4), FDC_9SC_SECTOR(6, 5),
            FDC_9SC_SECTOR(7, 6), FDC_9SC_SECTOR(8, 7), FDC_9SC_SECTOR(9, 8)},
};

uint32_t fdc_profile_sector_count(const fdc_media_profile_t *p) {
    return p ? (uint32_t)p->cyls * p->heads * p->sectors : 0;
}

int fdc_profile_track_sectors(const fdc_media_profile_t *p, int cyl, int phys_hd,
                              const fdc_sector_entry_t **sec) {
    if (cyl == 0 && phys_hd == 0 && p->boot_track_sectors) {
        *sec = p->boot_track_sec;
        return p->boot_track_sectors;
    }
    *sec = p->sec;
    return p->sectors;
}

uint8_t fdc_profile_id_r(const fdc_media_profile_t *p, int cyl, int phys_hd, int i) {
    if (i == 0 && cyl == 0 && phys_hd == 0 && p->boot_r)
        return p->boot_r;
    const fdc_sector_entry_t *sec;
    fdc_profile_track_sectors(p, cyl, phys_hd, &sec);
    return sec[i].r;
}

uint8_t fdc_profile_id_h(const fdc_media_profile_t *p, int cyl, int phys_hd, int i) {
    if (i == 0 && cyl == 0 && phys_hd == 0 && p->boot_r)
        return (uint8_t)phys_hd;
    return (uint8_t)(phys_hd + p->id_h_offset);
}

int fdc_profile_locate(const fdc_media_profile_t *p, int cyl, int phys_hd, int id_h, int r,
                       int n, fdc_sector_loc_t *loc) {
    if (!p || !loc)
        return -1;
    if (cyl < 0 || cyl >= p->cyls || phys_hd < 0 || phys_hd >= p->heads)
        return -1;
    const fdc_sector_entry_t *sec;
    int count = fdc_profile_track_sectors(p, cyl, phys_hd, &sec);
    for (int i = 0; i < count; i++) {
        const fdc_sector_entry_t *e = &sec[i];
        if (fdc_profile_id_h(p, cyl, phys_hd, i) != id_h ||
            fdc_profile_id_r(p, cyl, phys_hd, i) != r || e->n != n)
            continue;
        uint32_t track = (uint32_t)(cyl * p->heads + phys_hd);
        loc->offset = track * p->track_bytes + e->byte_offset;
        loc->length = e->byte_length;
        /* DDAMの通し番号はトラックあたりp->sectors個まで。それを超える(2HTの
         * ブートトラック)セクタは隣のトラックと衝突するためDDAM対象外にする。 */
        loc->index =
            (uint32_t)i < p->sectors ? track * p->sectors + (uint32_t)i : FDC_SECTOR_NO_INDEX;
        return 0;
    }
    return -1;
}

static uint32_t ddam_index(int cyl, int hd, int sec);

static const fdc_media_profile_t *fdc_profile_of(const fdc_t *f, int drv) {
    return f->profile[drv] ? f->profile[drv] : &fdc_profile_2hd;
}

/* ドライブdrvの実効転送速度（$E94007 bit4: 1=250kbps、0=500kbps）が挿入媒体のプロファイルと
 * 一致するか。不一致の媒体は、実機ドライブと同様にIDを読めない
 * （READ ID: MA、その他: ND）。 */
static int fdc_media_matches(const fdc_t *f, int drv) {
    int rate = f->density_2dd ? FDC_RATE_250_KBPS : FDC_RATE_500_KBPS;
    return fdc_profile_of(f, drv)->rate_kbps == rate;
}

/* ドライブdrvの回転数が300rpmか（$E94005 bit3-0: ドライブNはNとN+1 mod 4の同時アサート）。 */
static int fdc_spindle_is_300rpm(const fdc_t *f, int drv) {
    uint8_t pair = (uint8_t)((1u << drv) | (1u << ((drv + 1) & 3)));
    return (f->option_bits & pair) == pair;
}

/* データ転送系（READ/WRITE/SCAN）で、ドライブの回転数が500kbps媒体と合うか。
 * 250kbps媒体と、媒体なしは照合しない。 */
static int fdc_rotation_matches(const fdc_t *f, int drv) {
    const fdc_media_profile_t *p = fdc_profile_of(f, drv);
    if (p->rate_kbps != FDC_RATE_500_KBPS)
        return 1;
    return p->rpm300 == (fdc_spindle_is_300rpm(f, drv) ? 1 : 0);
}

/* READ/WRITE/SCAN共通のセクタ解決。CHRNからイメージ内のオフセットと長さを求め、
 * 減算形でイメージ範囲内かを検査する（加算形は巨大なオフセットでラップし得る）。成功は0、ND相当の失敗は-1。
 * strict=0は標準2HDの既存READ/WRITE DATA(0x06/0x05)のみで、従来どおりCHRNの
 * 検証をせず式でオフセットを求める（受理範囲を変えない）。それ以外（deleted系・
 * SCAN・標準2HD以外のプロファイル）は媒体のIDと厳密に照合する。 */
static int fdc_resolve_sector(const fdc_t *f, int drv, int strict, int cyl, int hd, int head,
                              int sec, int N, fdc_sector_loc_t *loc) {
    const fdc_media_profile_t *p = fdc_profile_of(f, drv);
    if (!fdc_media_matches(f, drv) || !fdc_rotation_matches(f, drv))
        return -1;
    if (strict || p != &fdc_profile_2hd) {
        if (fdc_profile_locate(p, cyl, hd, head, sec, N, loc) != 0)
            return -1;
    } else {
        if (N < 0 || N > 7)
            return -1;
        loc->length = (uint32_t)(128 << N);
        loc->offset = (uint32_t)((cyl * 2 + hd) * 8 + (sec - 1)) * loc->length;
        loc->index = ddam_index(cyl, hd, sec);
    }
    uint32_t bytes = (uint32_t)f->disk_bytes[drv];
    if (loc->offset > bytes || loc->length > bytes - loc->offset)
        return -1;
    return 0;
}

/* DDAMビットマップのインデックス変換（標準2HDの既存READ/WRITE DATA経路用）。
 * 既存のdisk_off計算と同じ式(cyl*FDC_2HD_HEADS+hd)*8+(sec-1)。範囲外は
 * FDC_SECTOR_NO_INDEXを返し、呼び出し側(ddam_get/set/clear)はこれを無操作に
 * フォールバックする（呼び出し側の検証漏れを事故にしない
 * 防御的多重化）。 */
static uint32_t ddam_index(int cyl, int hd, int sec) {
    if (cyl < 0 || cyl >= FDC_2HD_CYLS || hd < 0 || hd >= FDC_2HD_HEADS || sec < 1 || sec > 8)
        return FDC_SECTOR_NO_INDEX;
    return (uint32_t)((cyl * FDC_2HD_HEADS + hd) * 8 + (sec - 1));
}

/* idxがドライブのプロファイルの総セクタ数（かつビットマップ容量）に収まるか。 */
static int ddam_in_range(const fdc_t *f, int drv, uint32_t idx) {
    return idx < fdc_profile_sector_count(fdc_profile_of(f, drv)) && idx < FDC_DDAM_MAX_SECTORS;
}

static int ddam_get(const fdc_t *f, int drv, uint32_t idx) {
    if (!ddam_in_range(f, drv, idx))
        return 0; /* 範囲外は通常DAM扱い */
    return (f->ddam[drv][idx / 8] >> (idx % 8)) & 1;
}

static void ddam_set(fdc_t *f, int drv, uint32_t idx) {
    if (!ddam_in_range(f, drv, idx))
        return;
    f->ddam[drv][idx / 8] |= (uint8_t)(1u << (idx % 8));
}

static void ddam_clear(fdc_t *f, int drv, uint32_t idx) {
    if (!ddam_in_range(f, drv, idx))
        return;
    f->ddam[drv][idx / 8] &= (uint8_t)~(1u << (idx % 8));
}

/* fdc_format_complete()から呼ばれる。1トラック分のDDAM状態を
 * 通常データへ戻す。呼び出し側でcyl/headの範囲確認済みで
 * あることを前提とするが、cyl/hdが媒体の範囲外なら何もしない（D3-1）。 */
void fdc_ddam_clear_track(fdc_t *f, int drv, int cyl, int hd) {
    if (drv < 0 || drv >= 4)
        return;
    const fdc_media_profile_t *p = fdc_profile_of(f, drv);
    if (cyl < 0 || cyl >= p->cyls || hd < 0 || hd >= p->heads)
        return;
    uint32_t base = (uint32_t)(cyl * p->heads + hd) * p->sectors;
    for (uint32_t i = 0; i < p->sectors; i++)
        ddam_clear(f, drv, base + i);
}

/* 排出・挿入時にドライブ全体のDDAM状態をクリアする（
 * 永続化しないためイジェクト・再挿入で通常データに戻る）。 */
void fdc_ddam_clear_drive(fdc_t *f, int drv) {
    if (drv < 0 || drv >= 4)
        return;
    memset(f->ddam[drv], 0, sizeof(f->ddam[drv]));
}

/* RESET命令相当のFDC(µPD72065)初期化。
 * µPD72065B データシート(p3 "Pin Identification")の
 * RESET ピン説明「Sets FDC to idle state ... INT and DRQ are set to low」に従い、
 * フェーズ・コマンド/結果バッファ・実行データフェーズの転送状態・INT を落とす。
 * 回路図(OutsideX68000_schematic.pdf p10 コントロール回路図)でIC203(µPD720651S、
 * 40pin DIP)の1番ピン(RESET)がRESETネットに直結していることを確認済み
 * （`docs/ix68k-internals/design/fdc_design.md`）。ドライブ選択ラッチ
 * ($E94005系、PEDEC/IC213側)はµPD72065のレジスタではないためここでは触れない
 *
 * 保持するもの:
 *  - disk[]/disk_bytes[]/disk_path[]/num_drives : メディア挿入状態とドライブ構成。
 *    RESET 命令は電源断ではない（XEiJ fdcReset() も FDUnit.fduImage を保持）。
 *  - ddam[]/write_protect[] : 挿入・排出でのみ変化する。
 *  - cur_cyl[] : 機械的ヘッド位置。RESET では FLTR/STEP 出力が Low になるだけで
 *    ステップパルスは出ず、ヘッドは動かない（データシート p4 脚注）。XEiJ も
 *    FDUnit.fduPCN を触らない。
 *  - bus : 構造体配線。
 * st0 は 0 ではなく 0x80（SENSE INTERRUPT STATUS の「割り込み無し」番兵）へ
 * 戻す。0 にすると RESET 直後の SENSE INTERRUPT STATUS が「ドライブ0のシーク
 * 正常終了」を名乗り、irq=0 と矛盾する（memset を使わない理由）。
 *
 * フィールド追加時は必ず本関数の分類（リセット/保持）を再検討すること。 */
void fdc_reset(fdc_t *f) {
    f->phase = FDC_IDLE;
    memset(f->cmd, 0, sizeof(f->cmd));
    f->cmd_idx = 0;
    f->cmd_total = 0;
    memset(f->res, 0, sizeof(f->res));
    f->res_idx = 0;
    f->res_len = 0;
    f->st0 = 0x80u;
    f->sense_cyl = 0;
    f->irq = 0;
    f->dma_ptr = NULL;
    f->dma_remaining = 0;
    f->dma_dir = 0;
    f->dma_drv = 0;
    f->dma_offset = 0;
    f->dma_size = 0;
    f->dma_cmp = 0;
    f->scan_mismatch = 0;
    f->fmt_active = 0;
    f->density_2dd = 0;
    f->option_bits = 0;
    memset(f->id_cursor, 0, sizeof(f->id_cursor));
}

void fdc_set_density(fdc_t *f, int is_2dd) {
    f->density_2dd = is_2dd ? 1u : 0u;
}

void fdc_set_option(fdc_t *f, uint8_t val) {
    f->option_bits = val & FDC_OPTION_DRIVE_MASK;
}

uint8_t fdc_msr(const fdc_t *f) {
    switch (f->phase) {
    case FDC_IDLE:
        return 0x80u; /* RQM=1 */
    case FDC_CMD:
        return 0x90u; /* RQM=1, CB=1 */
    case FDC_EXEC:
        /* RQM=1, CB=1, DIO=dma_dir（READ中はFDC→CPUなのでDIO=1、WRITE中はCPU→FDCなのでDIO=0）
         */
        return f->dma_dir ? 0xD0u : 0x90u;
    case FDC_RESULT:
        return 0xD0u; /* RQM=1, DIO=1, CB=1 */
    default:
        return 0x80u;
    }
}

/* FORMAT TRACKのID列がトラックの配置と一致するか。alt_rが0以外なら、先頭セクタだけを
 * H=物理ヘッド,R=alt_rとして照合する（それ以外は通常配置）。 */
static int fdc_format_ids_match(const fdc_media_profile_t *p, const fdc_t *f, const uint8_t *id,
                                int count, const fdc_sector_entry_t *layout, uint8_t alt_r) {
    for (int i = 0; i < count; i++) {
        const uint8_t *t = &id[i * 4];
        uint8_t h = fdc_profile_id_h(p, f->fmt_cyl, f->fmt_hd, i);
        uint8_t r = fdc_profile_id_r(p, f->fmt_cyl, f->fmt_hd, i);
        if (i == 0 && alt_r) {
            h = (uint8_t)f->fmt_hd;
            r = alt_r;
        }
        if (t[0] != (uint8_t)f->fmt_cyl || t[1] != h || t[2] != r || t[3] != layout[i].n)
            return 0;
    }
    return 1;
}

/* FORMAT TRACK(0x0D)のIDタプル列を媒体プロファイルと照合し、一致すれば
 * 対象トラックをフィルバイトで初期化してDDAMを落とし、イメージへ書き戻す。
 * 任意のID並べ替え・シフトは永続化できないため、受理するのはプロファイルの
 * ID集合かつ順序と完全に一致する列だけ（設計書 §4 FORMAT）。結果はres[]へ構築する。
 * 標準2HDの予備シリンダ(cyl>=77)は、バッキングが無いため黙って成功扱いにする
 * （FORMAT.Xは cyl=77 まで要求する。IOCS _B_FORMATは実機ROM経由でここへ来る）。 */
static void fdc_format_complete(fdc_t *f) {
    int unit = f->fmt_unit;
    int drv = fdc_unit_to_drive(unit);
    const fdc_media_profile_t *p = fdc_profile_of(f, drv);
    const uint8_t *id = f->fmt_ids;
    int ok = 1;

    if (f->fmt_cyl >= p->cyls) {
        ok = (p == &fdc_profile_2hd);
        if (ok) {
            f->res[0] = (uint8_t)unit;
            f->res[1] = 0x00u;
            f->res[2] = 0x00u;
            f->res[3] = (uint8_t)f->fmt_cyl;
            f->res[4] = (uint8_t)f->fmt_hd;
            f->res[5] = 1;
            f->res[6] = 3;
            return;
        }
    }
    const fdc_sector_entry_t *layout;
    int count = fdc_profile_track_sectors(p, f->fmt_cyl, f->fmt_hd, &layout);
    ok = (f->fmt_sc == count) && fdc_format_ids_match(p, f, id, count, layout, 0);
    /* 2HDEZ: (C0,H0)の特例配置(先頭だけH=物理ヘッド,R=format_boot_r)も受理する */
    if (!ok && p->format_boot_r && f->fmt_cyl == 0 && f->fmt_hd == 0 && f->fmt_sc == count)
        ok = fdc_format_ids_match(p, f, id, count, layout, p->format_boot_r);
    uint32_t track = (uint32_t)(f->fmt_cyl * p->heads + f->fmt_hd);
    uint32_t off = track * p->track_bytes;
    if (ok && (off > (uint32_t)f->disk_bytes[drv] ||
               p->track_bytes > (uint32_t)f->disk_bytes[drv] - off))
        ok = 0;

    const uint8_t *last = &id[(f->fmt_sc - 1) * 4];
    f->res[3] = last[0];
    f->res[4] = last[1];
    f->res[5] = last[2];
    f->res[6] = last[3];
    if (!ok) {
        f->res[0] = (uint8_t)(0x40u | unit);
        f->res[1] = 0x04u; /* ND: 媒体のIDと一致しない */
        f->res[2] = 0x00u;
        return;
    }

    memset(f->disk[drv] + off, f->fmt_fill, (size_t)p->track_bytes);
    fdc_media_changed(f);
    fdc_ddam_clear_track(f, drv, f->fmt_cyl, f->fmt_hd);
    if (f->disk_path[drv]) {
        FILE *fp = fopen(f->disk_path[drv], "r+b");
        if (fp) {
            fseek(fp, (long)off, SEEK_SET);
            fwrite(f->disk[drv] + off, 1, (size_t)p->track_bytes, fp);
            fclose(fp);
        }
    }
    f->res[0] = (uint8_t)unit;
    f->res[1] = 0x00u;
    f->res[2] = 0x00u;
}

/* 実行データフェーズ(FDC_EXEC)完了処理。WRITE時はディスクイメージへの
 * write-back(fseek+fwrite)を行い、結果フェーズ(FDC_RESULT)へ遷移する。
 * res[]はfdc_exec()側で既に構築済み（コマンドパラメータのみに依存し
 * 転送データ自体には依存しないため、フェーズ開始時点で確定できる）。 */
static void fdc_exec_complete(fdc_t *f) {
    if (f->fmt_active) {
        /* FORMAT TRACK: res[]はfdc_format_complete()が検証結果に応じて構築する */
        fdc_format_complete(f);
        f->fmt_active = 0;
    } else if (f->dma_cmp) {
        /* SCAN EQUAL: ディスクへは書き戻さない。ST2にScan Hit(bit3)または
         * Scan Not Satisfied(bit2)を立てる。ROMの_B_VERIFY($FF8BA0)は
         * btst.l #$0a,d0 でSN(=D0 bit10)だけを見てベリファイ失敗を判定する。 */
        f->res[2] = f->scan_mismatch ? 0x04u : 0x08u;
        f->dma_cmp = 0;
    } else if (f->dma_dir == 0 && f->disk_path[f->dma_drv]) {
        fdc_media_changed(f);
        FILE *fp = fopen(f->disk_path[f->dma_drv], "r+b");
        if (fp) {
            fseek(fp, (long)f->dma_offset, SEEK_SET);
            fwrite(f->disk[f->dma_drv] + f->dma_offset, 1, (size_t)f->dma_size, fp);
            fclose(fp);
        }
    }
    f->dma_ptr = NULL;
    f->phase = FDC_RESULT;
    f->res_idx = 0;
    f->res_len = 7;
    f->irq = 1;
}

/* fdc_exec()が設定した実行データフェーズ(FDC_EXEC)を、fdc_read_data()/
 * fdc_write_data()経由で即座に内部ドレインする。通常経路はDMAC ch0の
 * 汎用エンジン(dmac_ch0_fdc_transfer)に置き換わっているため、現在は
 * ch0がCCR STARTで武装されていない異常系（計装ではゼロ例外）専用の
 * フォールバックとしてのみ使われる。stale registerによる誤転送を避け
 * つつ、FDCコマンドが必ず完了する（結果フェーズへ遷移する）ことを
 * 保証する安全網。 */
static void fdc_drain_exec_phase(fdc_t *f, uint8_t *ram, uint8_t *crtc_regs, uint32_t mar) {
    while (f->phase == FDC_EXEC) {
        if (f->dma_dir == 1) {
            ram[mar++] = fdc_read_data(f);
        } else {
            fdc_write_data(f, ram, crtc_regs, ram[mar++]);
        }
    }
}

/* ram/crtc_regs は f->bus からも辿れるが、あえて引数で受け取る。実際の
 * bus->ram/bus->crtc_regsはポインタでx68k_bus_create()内でヒープ確保されるため、
 * busを丸ごと組み立てずRECALIBRATE/SENSE系コマンドだけを検証したい単体テスト
 * （tests/fdc_probe_test.c）はスタック上の小さな代替バッファをそのまま渡せる。
 * f->bus経由の直接参照に変えると、そのテストでは常にNULLポインタ参照になる。 */
static void fdc_exec(fdc_t *f, uint8_t *ram, uint8_t *crtc_regs) {
    uint8_t c = f->cmd[0] & 0x1Fu;
    f->res_len = 0;
    f->irq = 0;
    f->dma_cmp = 0; /* 前コマンドの残留を持ち越さない */
    f->scan_mismatch = 0;
    f->fmt_active = 0;

    switch (c) {
    case 0x03: /* SPECIFY: SRT/HUT, HLT/NDM — タイミング設定のみ */
        break;

    case 0x07: /* RECALIBRATE: シリンダ 0 に移動 */
    {
        int unit = f->cmd[1] & 3;
        int phys = fdc_unit_to_drive(unit);
        if (adpcm_get_ct2(&f->bus->adpcm)) {
            /* OPM $1B bit6(CT2)=1: FDCのREADY端子強制。ROMの_B_RECALI
             * (D1.b=$FF、IOCS $47「強制レディ状態での検査」)によるドライブ
             * 物理存在プローブ専用モード(『Inside X68000』 3-2節)。この間は
             * メディア有無ではなく物理ドライブ存在(phys<num_drives)だけで
             * 成否を決める。ROM側はST0 bit4=EC(Equipment Check)のみで不在を
             * 判定する(X68k_IOCS_Data_Crystal.html $47参照)ため、NR(bit3)では
             * なくECを立てる点が肝。 */
            if (phys < f->num_drives) {
                f->cur_cyl[unit] = 0;
                f->st0 = (uint8_t)(0x20u | unit); /* NT + SE, EC=0 → ドライブ存在 */
                f->sense_cyl = 0;
            } else {
                f->st0 = (uint8_t)(0x70u | unit); /* IC=01(AT) + SE + EC → 物理不在 */
            }
        } else if (!f->disk[phys]) {
            /* 通常動作(CT2=0): Not Ready。ディスク未挿入だとシークできず ST0 の NR
             * ビットが立って中断する。ここを常時成功にしていると、Human68k の
             * FDD ドライバがアクセスシーケンス冒頭で未挿入を検出できず、
             * 後段の READ DATA まで進んでしまい誤ったエラーメッセージ
             * （「無効なメディアを使用しました」）につながる。 */
            f->st0 = (uint8_t)(0x48u | unit); /* IC=01 + NR */
        } else {
            f->cur_cyl[unit] = 0;
            f->st0 = (uint8_t)(0x20u | unit);
            f->sense_cyl = 0;
        }
        f->irq = 1;
        break;
    }

    case 0x08: /* SENSE INTERRUPT STATUS */
        /* µPD72065B: 割り込み pending なら ST0+PCN の2バイト、なければ ST0=0x80 の1バイトのみ
         */
        f->res[0] = f->st0;
        f->res[1] = f->sense_cyl;
        f->res_len = (f->st0 != 0x80u) ? 2 : 1;
        f->st0 = 0x80u;
        f->irq = 0;
        break;

    case 0x04: /* SENSE DRIVE STATUS (標準名称 SENSE DEVICE STATUS) */
    {
        int unit = f->cmd[1] & 3;
        int hd = (f->cmd[1] >> 2) & 1; /* 『Inside X68000』 図33: bit2=HD */
        int drv = fdc_unit_to_drive(unit);
        /* ST3 (図34): bit7 FT は未エミュレート(常に0)。
         * bit3 TS (Two Side) は X68000内蔵ドライブがハード的に常に2-sided
         * であることを示す信号なので常に1にする（ROM の _B_DRVSNS が
         * 期待する応答形式と一致させる）。bit2 HD はコマンドフェーズで指定された値を
         * そのまま返す。 */
        uint8_t st3 = (uint8_t)(unit | (hd << 2) | 0x08u);
        if (f->disk[drv])
            st3 |= 0x20u; /* RDY */
        if (f->cur_cyl[unit] == 0)
            st3 |= 0x10u; /* TR0 */
        if (f->disk[drv] && f->write_protect[drv])
            st3 |= 0x40u; /* WP: 図34 bit6。未挿入時は立てない（write_protect[]は
                           * 排出でクリアされ本来冗長だが、ddamと同じ防御的多重化 */
        f->res[0] = st3;
        f->res_len = 1;
        break;
    }

    case 0x0F: /* SEEK */
    {
        /* 『Inside X68000』 図30: C-PHASEの2バイト目は bit7-2:0,
         * bit1-0:US1/US0 のみで HD フィールドは存在しない（RECALIBRATEと
         * 同様）。
         *
         * CT2プローブ(case 0x07参照)はSEEKには未対応。ROMのドライブ物理
         * 存在チェックはRECALIBRATE(IOCS $47, D1.b=$FF)のみで行われることを
         * XEiJ/自機の起動トレースで確認済みで、SEEKがCT2=1で発行される経路は
         * 未確認。参照すべきST0値も不明なため、実際にROMがCT2中にSEEKを
         * 発行するトレースと突き合わせられるまで現状維持とする。 */
        int unit = f->cmd[1] & 3;
        int phys = fdc_unit_to_drive(unit);
        if (!f->disk[phys]) {
            /* RECALIBRATE と同じ理由で Not Ready を反映する */
            f->st0 = (uint8_t)(0x48u | (uint8_t)unit);
        } else {
            f->cur_cyl[unit] = f->cmd[2];
            f->st0 = (uint8_t)(0x20u | (uint8_t)unit);
            f->sense_cyl = f->cmd[2];
        }
        f->irq = 1;
        break;
    }

    case 0x0A: /* READ ID */
    {
        int drv = f->cmd[1] & 3;
        int hd = (f->cmd[1] >> 2) & 1;
        f->res[0] = (uint8_t)drv;
        f->res[1] = 0x00u;
        f->res[2] = 0x00u;
        const fdc_media_profile_t *prof = fdc_profile_of(f, drv);
        if (f->disk[drv] && !fdc_media_matches(f, drv)) {
            f->res[0] = (uint8_t)(0x40u | drv); /* AT */
            f->res[1] = 0x01u;                  /* MA: IDを読めない */
            f->res[3] = f->cur_cyl[drv];
            f->res[4] = (uint8_t)hd;
            f->res[5] = 0;
            f->res[6] = 0;
            f->res_len = 7;
            f->irq = 1;
            break;
        }
        const fdc_sector_entry_t *id_sec;
        int id_count = fdc_profile_track_sectors(prof, f->cur_cyl[drv], hd, &id_sec);
        int id_pos = f->id_cursor[drv] < id_count ? f->id_cursor[drv] : 0;
        f->id_cursor[drv] = (uint8_t)((id_pos + 1) % id_count);
        f->res[3] = f->cur_cyl[drv];
        f->res[4] = fdc_profile_id_h(prof, f->cur_cyl[drv], hd, id_pos);
        f->res[5] = fdc_profile_id_r(prof, f->cur_cyl[drv], hd, id_pos);
        f->res[6] = id_sec[id_pos].n;
        f->res_len = 7;
        f->irq = 1;
        break;
    }

    case 0x06: /* READ DATA (MT/MFM/SK 上位ビット込み) */
    case 0x0C: /* READ DELETED DATA(IOCS _B_READDL(0x4C)用)。
                * IPLROMXV $FF9068 の _B_READDL が `or.b #$0c,d1` して
                * `bra $FF8E4A`（_B_READ 本体）へ合流し発行する。0x06との差分は
                * DDAMビット参照とCM(ST2 bit6)応答のみ（design doc D3/D5）。
                * SK(byte0 bit5)は常に0、EOT(byte6)は常にRと同値の単一セクタ形
                * のみ対応（ROM委譲経路と一致。design doc D8、SK=1/MT/複数
                * セクタ探索は未対応）。 */
    {
        int deleted = (c == 0x0C);
        int unit = f->cmd[1] & 3;
        int drv = fdc_unit_to_drive(unit);
        int hd = (f->cmd[1] >> 2) & 1;
        int cyl = f->cmd[2];
        int head = f->cmd[3];
        int sec = f->cmd[4];
        int N = f->cmd[5];

        if (!f->disk[drv]) {
            /* Not Ready: 実機 FDC は NR ビットのみを立てて中断する（シーク/セクタ
             * 探索を行っていないため ST1/ST2 は 0）。ここに ND(0x04) を混ぜると
             * Human68k FDD ドライバが「無効なメディア」と誤判定し、正しい
             * 「ディスクが入っていません」ダイアログが出なくなる。 */
            f->res[0] = (uint8_t)(0x48u | unit); /* IC=01 + NR */
            f->res[1] = 0x00u;
            f->res[2] = 0x00u;
            f->res[3] = (uint8_t)cyl;
            f->res[4] = (uint8_t)head;
            f->res[5] = (uint8_t)sec;
            f->res[6] = (uint8_t)N;
            f->res_len = 7;
            f->irq = 1;
            break;
        }

        /* D4-1: シフト未定義動作の除去。N<=7は128..16384で全て定義済みであり、
         * 従来0x06が受け付けていた範囲の挙動は変わらない。 */
        if (N > 7) {
            f->res[0] = (uint8_t)(0x40u | unit); /* AT */
            f->res[1] = 0x04u;                   /* ND */
            f->res[2] = 0x00u;
            f->res[3] = (uint8_t)cyl;
            f->res[4] = (uint8_t)head;
            f->res[5] = (uint8_t)sec;
            f->res[6] = (uint8_t)N;
            f->res_len = 7;
            f->irq = 1;
            break;
        }

        /* deleted系(0x0C)は媒体のIDと厳密に照合する(0x11と同一条件)。
         * 既存0x06の受理範囲は変えない。範囲判定は減算形で
         * fdc_resolve_sector()内に集約している。 */
        fdc_sector_loc_t loc;
        if (fdc_resolve_sector(f, drv, deleted, cyl, hd, head, sec, N, &loc) != 0) {
            f->res[0] = (uint8_t)(0x40u | unit);
            f->res[1] = 0x04u;
            f->res[2] = 0x00u;
            f->res[3] = (uint8_t)cyl;
            f->res[4] = (uint8_t)head;
            f->res[5] = (uint8_t)sec;
            f->res[6] = (uint8_t)N;
            f->res_len = 7;
            f->irq = 1;
            break;
        }

        /* DDAM種別不一致判定(design doc D3/D3-2)。0x0Cはビットが0(通常DAM)
         * なら不一致、0x06はビットが1(削除データ)なら不一致。不一致時は
         * ST2にCM(bit6, $40)を立て、そのセクタのデータは転送された上で
         * res[5](R)は「次セクタ」ではなく「実行終了セクタ」自身になる
         * （XEiJ FDC.java 2831/3187行、fduCommandReadData/ReadDeletedDataの
         * R-Phase規則に同一文言で記載）。 */
        int is_deleted_sector = ddam_get(f, drv, loc.index);
        int mismatch = deleted ? !is_deleted_sector : is_deleted_sector;

        /* 実行データフェーズ(FDC_EXEC)を設定し、DMAC ch0の汎用エンジン
         * (dmac_ch0_fdc_transfer)経由でdisk[drv]→RAMへ転送する。res[]は
         * 転送データに依存せずコマンドパラメータのみで決まるため先に
         * 確定できる（res_len/irqはfdc_exec_complete()が実行データ
         * フェーズ完了時に設定する）。 */
        f->res[0] = (uint8_t)unit;            /* ST0: 正常終了 */
        f->res[1] = 0x00u;                    /* ST1 */
        f->res[2] = mismatch ? 0x40u : 0x00u; /* ST2: CM */
        f->res[3] = (uint8_t)cyl;
        f->res[4] = (uint8_t)head;
        f->res[5] = mismatch ? (uint8_t)sec : (uint8_t)(sec + 1);
        f->res[6] = (uint8_t)N;

        int ssize = (int)loc.length;
        f->dma_ptr = f->disk[drv] + loc.offset;
        f->dma_remaining = loc.length;
        f->dma_dir = 1; /* READ: disk→host */
        f->dma_drv = drv;
        f->dma_offset = loc.offset;
        f->dma_size = loc.length;
        f->phase = FDC_EXEC;

        /* DMAC ch0がCCR STARTで武装済みなら汎用エンジンで消費する。非武装
         * （stale呼び出し等の異常系、計装ではゼロ例外）ならフォール
         * バック(内部ドレイン、MARはCCR経由と同じレジスタを直接参照)に
         * 委ねる（stale register誤転送ガード）。 */
        if (!dmac_ch0_fdc_transfer(f->bus)) {
            uint32_t mar = ((uint32_t)crtc_regs[0x400D] << 16) |
                           ((uint32_t)crtc_regs[0x400E] << 8) | (uint32_t)crtc_regs[0x400F];
            if (mar >= 0x100u && mar + (uint32_t)ssize <= RAM_BUF_SIZE) {
                fdc_drain_exec_phase(f, ram, crtc_regs, mar);
            } else {
                /* MAR範囲外: 従来のmemcpy実装と同じくデータは動かさないが、
                 * 実行データフェーズは完了させ通常終了のres[]を返す。 */
                f->dma_remaining = 0;
                fdc_exec_complete(f);
            }
        }
        break;
    }

    case 0x05: /* WRITE DATA (MT/MFM 上位ビット込み) */
    case 0x09: /* WRITE DELETED DATA(IOCS _B_WRITED(0x49)用)。
                * IPLROMXV $FF8FD0 の _B_WRITED が `or.b #$09,d1` して
                * `bra $FF8D90`（_B_WRITE 本体）へ合流し発行する。0x05との差分は
                * DDAMビット更新のみ（WRITE系はCMを見ない、design doc D3/D5）。
                * SK/EOTの制約は0x06/0x0Cと同じ（design doc D8）。 */
    {
        int deleted = (c == 0x09);
        int unit = f->cmd[1] & 3;
        int drv = fdc_unit_to_drive(unit);
        int hd = (f->cmd[1] >> 2) & 1;
        int cyl = f->cmd[2];
        int head = f->cmd[3];
        int sec = f->cmd[4];
        int N = f->cmd[5];

        if (!f->disk[drv]) {
            /* Not Ready: READ DATA 分岐と同じ理由で ST1/ST2 は 0 にする */
            f->res[0] = (uint8_t)(0x48u | unit);
            f->res[1] = 0x00u;
            f->res[2] = 0x00u;
            f->res[3] = (uint8_t)cyl;
            f->res[4] = (uint8_t)head;
            f->res[5] = (uint8_t)sec;
            f->res[6] = (uint8_t)N;
            f->res_len = 7;
            f->irq = 1;
            break;
        }

        /* ライトプロテクト: ST0=AT(0x40)+ST1 bit1=NW(0x02)。ROMの_B_WRITE/
         * _B_WRITED/_B_FORMAT 共通セクタループ($FF8DCA / $FF912A の btst #$11)が
         * このビットを見てリトライせず即中断する。Not Readyの直後・N>7等の
         * ジオメトリ検証の前で判定する(XEiJ fduCommandWriteData()と同じ順序)。DDAMビット更新より前にあるため、0x09がWPで拒否
         * されたときDDAMビットは変化しない。 */
        if (f->write_protect[drv]) {
            f->res[0] = (uint8_t)(0x40u | unit);
            f->res[1] = 0x02u; /* NW */
            f->res[2] = 0x00u;
            f->res[3] = (uint8_t)cyl;
            f->res[4] = (uint8_t)head;
            f->res[5] = (uint8_t)sec;
            f->res[6] = (uint8_t)N;
            f->res_len = 7;
            f->irq = 1;
            break;
        }

        if (N > 7) {
            f->res[0] = (uint8_t)(0x40u | unit);
            f->res[1] = 0x04u;
            f->res[2] = 0x00u;
            f->res[3] = (uint8_t)cyl;
            f->res[4] = (uint8_t)head;
            f->res[5] = (uint8_t)sec;
            f->res[6] = (uint8_t)N;
            f->res_len = 7;
            f->irq = 1;
            break;
        }

        fdc_sector_loc_t loc;
        if (fdc_resolve_sector(f, drv, deleted, cyl, hd, head, sec, N, &loc) != 0) {
            f->res[0] = (uint8_t)(0x40u | unit);
            f->res[1] = 0x04u;
            f->res[2] = 0x00u;
            f->res[3] = (uint8_t)cyl;
            f->res[4] = (uint8_t)head;
            f->res[5] = (uint8_t)sec;
            f->res[6] = (uint8_t)N;
            f->res_len = 7;
            f->irq = 1;
            break;
        }

        /* 実行データフェーズ(FDC_EXEC)を設定し、DMAC ch0の汎用エンジン
         * (dmac_ch0_fdc_transfer)経由でRAM→disk[drv]へ転送する。ディスク
         * イメージへのwrite-back(fseek+fwrite)はfdc_exec_complete()が
         * 実行データフェーズ完了時に行う。 */
        f->res[0] = (uint8_t)unit;
        f->res[1] = 0x00u;
        f->res[2] = 0x00u; /* WRITE系はCMを見ない */
        f->res[3] = (uint8_t)cyl;
        f->res[4] = (uint8_t)head;
        f->res[5] = (uint8_t)(sec + 1);
        f->res[6] = (uint8_t)N;

        /* D3: DDAMビット更新。0x09はセット、0x05はクリア(通常データを
         * 書き直すため)。実機はコマンド受理時にIDフィールドを書くため、
         * データ転送の成否に関わらずここで確定させる。 */
        if (deleted)
            ddam_set(f, drv, loc.index);
        else
            ddam_clear(f, drv, loc.index);

        int ssize = (int)loc.length;
        f->dma_ptr = f->disk[drv] + loc.offset;
        f->dma_remaining = loc.length;
        f->dma_dir = 0; /* WRITE: host→disk */
        f->dma_drv = drv;
        f->dma_offset = loc.offset;
        f->dma_size = loc.length;
        f->phase = FDC_EXEC;

        /* DMAC ch0がCCR STARTで武装済みなら汎用エンジンで消費する。非武装
         * （異常系）ならフォールバック(内部ドレイン)に委ねる。 */
        if (!dmac_ch0_fdc_transfer(f->bus)) {
            uint32_t mar = ((uint32_t)crtc_regs[0x400D] << 16) |
                           ((uint32_t)crtc_regs[0x400E] << 8) | (uint32_t)crtc_regs[0x400F];
            if (mar + (uint32_t)ssize <= RAM_BUF_SIZE) {
                fdc_drain_exec_phase(f, ram, crtc_regs, mar);
            } else {
                f->dma_remaining = 0;
                fdc_exec_complete(f);
            }
        }
        break;
    }

    case 0x11: /* SCAN EQUAL（IOCS _B_VERIFY 0x41 用。MT/MFM上位ビット込み）
                *
                * IPLROMXV $FF8B48 の _B_VERIFY が発行する。ROM実測($FF8B6A-$FF8B82):
                *   byte0 = (D1.b & $C0) | $11   標準呼び出し(D1.b=$70)では $51
                *   byte1 = HD<<2 | US
                *   byte2..5 = C,H,R,N   （D2 = N<<24|C<<16|H<<8|R）
                *   byte6 = EOT = R      ★ 常にRと同値 → 1コマンド=1セクタ
                *   byte7 = GPL ($35 @ N=3/MFM)
                *   byte8 = STP = $FF    ★ READ/WRITE DATA用のDTL値の流用
                * EOT==R なので byte6/byte8 は読まない（マルチセクタSCANは未実装）。
                * マルチセクタ化はROM側の dbra d4 ループ($FF8BB6)が担当する
                * （READ/WRITE DATAと同じ構造）。
                *
                * DMA方向はWRITE DATAと同一（$FF92A0 が ch0 OCR に $32 = DIR=0 を書く）。
                * ホストメモリのバイトがDMAC経由でfdc_write_data()へ届くので、
                * dma_dir=0 のまま dma_cmp=1 を立てて「書かずに比較する」に分岐させる。 */
    {
        int unit = f->cmd[1] & 3;
        int drv = fdc_unit_to_drive(unit);
        int hd = (f->cmd[1] >> 2) & 1;
        int cyl = f->cmd[2];
        int head = f->cmd[3];
        int sec = f->cmd[4];
        int N = f->cmd[5];

        if (!f->disk[drv]) {
            /* Not Ready: READ/WRITE DATA と同じくNRのみ。ST1/ST2は0。 */
            f->res[0] = (uint8_t)(0x48u | unit);
            f->res[1] = 0x00u;
            f->res[2] = 0x00u;
            f->res[3] = (uint8_t)cyl;
            f->res[4] = (uint8_t)head;
            f->res[5] = (uint8_t)sec;
            f->res[6] = (uint8_t)N;
            f->res_len = 7;
            f->irq = 1;
            break;
        }

        /* ゲストは$E94003を直接操作できるためROM側の検証($FF92F6)には依存
         * できない。オフセット計算前に媒体のIDと厳密に照合する。
         * N>=24でのシフト
         * 未定義動作、R=0での負数キャストによるオフセットのラップアラウンド、
         * R=9の次ヘッドエイリアス、HD/H不一致を全てfdc_resolve_sector()が弾く。
         * 範囲チェックは減算形で、非標準サイズのイメージ向けの第二の安全網。 */
        fdc_sector_loc_t loc;
        if (fdc_resolve_sector(f, drv, 1, cyl, hd, head, sec, N, &loc) != 0) {
            f->res[0] = (uint8_t)(0x40u | unit);
            f->res[1] = 0x04u;
            f->res[2] = 0x00u;
            f->res[3] = (uint8_t)cyl;
            f->res[4] = (uint8_t)head;
            f->res[5] = (uint8_t)sec;
            f->res[6] = (uint8_t)N;
            f->res_len = 7;
            f->irq = 1;
            break;
        }

        /* res[2](ST2)は比較が終わるまで確定しないので fdc_exec_complete() が入れる。
         * res[5](R)は「最後に比較したセクタ」= sec。READ/WRITE DATAのように
         * sec+1 にはしない（XEiJ FDC.java:3715 fduEPhaseEnd が fduPRN を
         * そのまま返すのと同じ）。 */
        f->res[0] = (uint8_t)unit; /* ST0: NT */
        f->res[1] = 0x00u;
        f->res[2] = 0x00u;
        f->res[3] = (uint8_t)cyl;
        f->res[4] = (uint8_t)head;
        f->res[5] = (uint8_t)sec;
        f->res[6] = (uint8_t)N;

        int ssize = (int)loc.length;
        f->dma_ptr = f->disk[drv] + loc.offset;
        f->dma_remaining = loc.length;
        f->dma_dir = 0; /* OCR.DIR=0（ホスト→FDC）。実際にディスクへは書かない */
        f->dma_cmp = 1;
        f->scan_mismatch = 0;
        f->dma_drv = drv;
        f->dma_offset = loc.offset;
        f->dma_size = loc.length;
        f->phase = FDC_EXEC;

        if (!dmac_ch0_fdc_transfer(f->bus)) {
            uint32_t mar = ((uint32_t)crtc_regs[0x400D] << 16) |
                           ((uint32_t)crtc_regs[0x400E] << 8) | (uint32_t)crtc_regs[0x400F];
            if (mar + (uint32_t)ssize <= RAM_BUF_SIZE) {
                fdc_drain_exec_phase(f, ram, crtc_regs, mar);
            } else {
                /* SCANはデータ整合性検査そのものが目的のため、READ/WRITEの
                 * ように「転送できないが正常終了扱い」にすると1バイトも
                 * 比較せずにベリファイ成功(SH)という偽陽性を返してしまう。
                 * dma_cmpの
                 * SH/SN確定処理(fdc_exec_complete)は通さず、異常終了
                 * (ST0=AT, ST1=OR:Overrun)へ直接遷移する。 */
                f->dma_cmp = 0;
                f->dma_remaining = 0;
                f->dma_ptr = NULL;
                f->phase = FDC_RESULT;
                f->res_idx = 0;
                f->res[0] = (uint8_t)(0x40u | unit); /* AT */
                f->res[1] = 0x10u;                   /* OR: Overrun */
                f->res[2] = 0x00u;
                f->res_len = 7;
                f->irq = 1;
            }
        }
        break;
    }

    case 0x0D: /* FORMAT TRACK: cmd[1]=HD/US, cmd[2]=N, cmd[3]=SC, cmd[4]=GPL, cmd[5]=D。
                * 実行フェーズでSC個のC,H,R,Nタプルを受け取り(WRITE方向DMA)、完了時に
                * fdc_format_complete()が検証・フィルする。 */
    {
        int unit = f->cmd[1] & 3;
        int drv = fdc_unit_to_drive(unit);
        int hd = (f->cmd[1] >> 2) & 1;
        int sc = f->cmd[3];

        f->res[3] = f->cur_cyl[unit];
        f->res[4] = (uint8_t)hd;
        f->res[5] = 0;
        f->res[6] = f->cmd[2];
        f->res_len = 7;
        f->irq = 1;
        f->res[1] = 0x00u;
        f->res[2] = 0x00u;
        if (!f->disk[drv]) {
            f->res[0] = (uint8_t)(0x48u | unit); /* ST0: Not Ready(IC=01, NR) */
            break;
        }
        if (f->write_protect[drv]) {
            f->res[0] = (uint8_t)(0x40u | unit);
            f->res[1] = 0x02u; /* NW */
            break;
        }
        if (!fdc_media_matches(f, drv)) {
            f->res[0] = (uint8_t)(0x40u | unit);
            f->res[1] = 0x04u; /* ND */
            break;
        }
        if (sc < 1 || sc > FDC_PROFILE_MAX_SECTORS || f->cmd[2] > 7) {
            f->res[0] = (uint8_t)(0x40u | unit);
            f->res[1] = 0x04u; /* ND */
            break;
        }

        f->res_len = 0; /* 結果はfdc_exec_complete()が確定する */
        f->irq = 0;
        f->fmt_active = 1;
        f->fmt_unit = unit;
        f->fmt_hd = hd;
        f->fmt_cyl = f->cur_cyl[unit];
        f->fmt_fill = f->cmd[5];
        f->fmt_sc = sc;
        memset(f->fmt_ids, 0, sizeof(f->fmt_ids));
        f->dma_ptr = f->fmt_ids;
        f->dma_remaining = (uint32_t)sc * 4u;
        f->dma_dir = 0; /* WRITE: host→FDC */
        f->dma_drv = drv;
        f->dma_offset = 0;
        f->dma_size = 0;
        f->phase = FDC_EXEC;

        if (!dmac_ch0_fdc_transfer(f->bus)) {
            uint32_t mar = ((uint32_t)crtc_regs[0x400D] << 16) |
                           ((uint32_t)crtc_regs[0x400E] << 8) | (uint32_t)crtc_regs[0x400F];
            if (mar >= 0x100u && mar + f->dma_remaining <= RAM_BUF_SIZE) {
                fdc_drain_exec_phase(f, ram, crtc_regs, mar);
            } else {
                /* MAR範囲外: IDを受け取れないため、全ID未受領のまま検証へ進め
                 * ND(不一致)で終わらせる。 */
                f->dma_remaining = 0;
                fdc_exec_complete(f);
            }
        }
        break;
    }

    default:
        /* 未知コマンド: ST0=0x80 (IC=Invalid Command) を返して RESULT フェーズへ */
        f->res[0] = 0x80u;
        f->res_len = 1;
        break;
    }
}

uint8_t fdc_read_data(fdc_t *f) {
    if (f->phase == FDC_EXEC) {
        /* READ DATA実行データフェーズ: disk[drv]から1バイト供給する。
         * dma_dir==0(WRITE中)の読み出しは実機でも想定外の操作のため0xFFを返す。 */
        if (f->dma_dir != 1 || f->dma_remaining == 0)
            return 0xFFu;
        uint8_t v = *f->dma_ptr++;
        if (--f->dma_remaining == 0)
            fdc_exec_complete(f);
        return v;
    }
    if (f->phase != FDC_RESULT)
        return 0xFFu;
    uint8_t v = f->res[f->res_idx++];
    if (f->res_idx >= f->res_len) {
        f->phase = FDC_IDLE;
        f->res_idx = 0;
        f->res_len = 0;
    }
    return v;
}

void fdc_write_data(fdc_t *f, uint8_t *ram, uint8_t *crtc_regs, uint8_t val) {
    if (f->phase == FDC_EXEC) {
        /* WRITE DATA実行データフェーズ: disk[drv]へ1バイト受領する。
         * dma_dir==1(READ中)の書き込みは実機でも想定外の操作のため無視する。 */
        if (f->dma_dir != 0 || f->dma_remaining == 0)
            return;
        if (f->dma_cmp) {
            /* SCAN EQUAL: ディスクの記録データと、DMACがホストメモリから
             * 送ってきた目的データを1バイトずつ比較する。ディスクは変更しない。
             * 目的データ(ホスト側)の $FF はドントケアで常に条件成立
             * （µPD765仕様。XEiJ FDC.java:3462 の `d != 0xff &&` に一致）。
             * 非対称である点に注意: ディスク側が $FF でも目的データが
             * $FF 以外なら不一致になる。 */
            if (val != 0xFFu && *f->dma_ptr != val)
                f->scan_mismatch = 1;
            f->dma_ptr++;
            if (--f->dma_remaining == 0)
                fdc_exec_complete(f);
            return;
        }
        *f->dma_ptr++ = val;
        if (--f->dma_remaining == 0)
            fdc_exec_complete(f);
        return;
    }
    if (f->phase == FDC_RESULT)
        return; /* 結果送信中は無視 */

    if (f->phase == FDC_IDLE) {
        f->cmd[0] = val;
        f->cmd_idx = 1;
        f->cmd_total = fdc_cmd_len_tab(val);
        f->res_len = 0;
        f->res_idx = 0;
        if (f->cmd_total == 1) {
            fdc_exec(f, ram, crtc_regs);
            f->phase = (f->res_len > 0) ? FDC_RESULT : FDC_IDLE;
        } else {
            f->phase = FDC_CMD;
        }
        return;
    }

    /* FDC_CMD: コマンドバイトを蓄積 */
    f->cmd[f->cmd_idx++] = val;
    if (f->cmd_idx >= f->cmd_total) {
        fdc_exec(f, ram, crtc_regs);
        f->phase = (f->res_len > 0) ? FDC_RESULT : FDC_IDLE;
    }
}
