#include "bus/bus_internal.h"
#include <string.h>

#define CRTC_OSC_LOW_HZ       38863630u
#define CRTC_OSC_HIGH_HZ      69551990u
#define CRTC_OSC_VGA_HZ       50350000u
#define CRTC_MAX_FRAME_US     40000u
#define CRTC_FALLBACK_RATE_HZ 60u

/* X68030 Inside Out CRTC図4/5の8ドットクロック経路。HF=0ではHRLを無視する。
 * 図5の第三行はHF=0と印字されるが、図4・CRTMOD17・既存表示分周の整合から
 * HF=1/HRL=1の経路として扱う。 */
static int crtc_8dot_clock(const x68k_bus_t *bus, uint64_t *divider, uint64_t *osc_hz) {
    uint8_t r20_lo = bus->crtc_regs[0x29];
    int hf = (r20_lo >> 4) & 1;
    int hd = r20_lo & 3;
    int hrl = (bus->crtc_regs[0xE007] >> 1) & 1;
    static const uint8_t low_divider[4] = {64, 32, 64, 64};
    static const uint8_t high_hrl0_divider[3] = {48, 24, 16};
    static const uint8_t high_hrl1_divider[3] = {64, 32, 16};

    if (!hf) {
        *divider = low_divider[hd];
        *osc_hz = CRTC_OSC_LOW_HZ;
        return 1;
    }
    if (hd == 3) {
        *divider = 16;
        *osc_hz = CRTC_OSC_VGA_HZ;
        return 1;
    }
    *divider = hrl ? high_hrl1_divider[hd] : high_hrl0_divider[hd];
    *osc_hz = CRTC_OSC_HIGH_HZ;
    return 1;
}

void x68k_bus_crtc_get_frame_timing(const x68k_bus_t *bus, x68k_crtc_frame_timing_t *out) {
    out->numerator = 1;
    out->denominator = CRTC_FALLBACK_RATE_HZ;
    out->valid = 0;

    uint8_t r20_hi = bus->crtc_regs[0x28];
    uint8_t r20_lo = bus->crtc_regs[0x29];
    uint16_t r00 = ((uint16_t)bus->crtc_regs[0x00] << 8) | bus->crtc_regs[0x01];
    uint16_t r04 = ((uint16_t)bus->crtc_regs[0x08] << 8) | bus->crtc_regs[0x09];
    uint16_t r05 = ((uint16_t)bus->crtc_regs[0x0A] << 8) | bus->crtc_regs[0x0B];
    uint16_t r06 = ((uint16_t)bus->crtc_regs[0x0C] << 8) | bus->crtc_regs[0x0D];
    uint16_t r07 = ((uint16_t)bus->crtc_regs[0x0E] << 8) | bus->crtc_regs[0x0F];
    int hf = (r20_lo >> 4) & 1;
    int vd = (r20_lo >> 2) & 3;
    uint64_t divider, osc_hz;

    /* Inside Out 図7: HF=0/VD=00はノンインターレース、HF=1/VD=00は
     * 二度読み、HF=1/VD=01はノンインターレース。残りはインターレース。 */
    if ((r20_hi == 0 && r20_lo == 0) || r00 == 0 || r04 == 0 || !(vd == 0 || (hf && vd == 1)) ||
        r05 >= r06 || r06 >= r07 || r07 > r04 || !crtc_8dot_clock(bus, &divider, &osc_hz))
        return;

    uint64_t numerator = divider * ((uint64_t)r00 + 1u) * ((uint64_t)r04 + 1u);
    if (numerator > (uint64_t)CRTC_MAX_FRAME_US * osc_hz / 1000000u)
        return;
    out->numerator = numerator;
    out->denominator = osc_hz;
    out->valid = 1;
}

static uint32_t palette_grb555_to_argb(uint16_t grb) {
    unsigned g = (grb >> 11) & 0x1Fu, r = (grb >> 6) & 0x1Fu, b = (grb >> 1) & 0x1Fu;
    return 0xFF000000u | ((uint32_t)((r << 3) | (r >> 2)) << 16) |
           ((uint32_t)((g << 3) | (g >> 2)) << 8) | (uint32_t)((b << 3) | (b >> 2));
}

const uint8_t *x68k_bus_get_gvram(const x68k_bus_t *bus) {
    return bus->gvram;
}

const uint8_t *x68k_bus_get_tvram(const x68k_bus_t *bus) {
    return bus->tvram;
}

const uint8_t *x68k_bus_get_sp_vram(const x68k_bus_t *bus) {
    return bus->sp_ram;
}

void x68k_bus_set_raster_y_live(x68k_bus_t *bus, int live) {
    bus->crtc.raster_y_live = live ? 1 : 0;
}

void x68k_bus_set_raster_pal_live(x68k_bus_t *bus, int live) {
    bus->crtc.raster_pal_live = live ? 1 : 0;
}

void x68k_bus_take_raster_palette_writes(x68k_bus_t *bus, unsigned *gfx, unsigned *text) {
    *gfx = bus->crtc.pal_vdisp_gfx_writes;
    *text = bus->crtc.pal_vdisp_text_writes;
    bus->crtc.pal_vdisp_gfx_writes = 0;
    bus->crtc.pal_vdisp_text_writes = 0;
}

int x68k_bus_get_tvram_scroll_y(const x68k_bus_t *bus) {
    /* CRTC R11 (word at 0xE80016–0xE80017): bits 9-0 = テキスト Y スクロール。
     * 縦方向は「フレーム先頭（垂直映像期間開始）でラッチ」が実機の正しい挙動
     * （XEiJ crtR11TxYZero 準拠）。
     * ラスターが未初期化/ラスタ同期無効時（y_latched==0）は従来どおりライブ値を返す。
     */
    if (!bus->crtc.raster_y_live && bus->crtc.raster.y_latched)
        return bus->crtc.raster.tx_y_zero;
    uint16_t r11 = ((uint16_t)bus->crtc_regs[0x16] << 8) | bus->crtc_regs[0x17];
    return (int)(r11 & 0x3FFu);
}

/* CRTC R10 (0xE80014–0xE80015): bits 9-0 = テキスト X スクロール。
 * 横方向はラスタ描画の瞬間の値をそのまま使い、ラッチしない。
 */
int x68k_bus_get_tvram_scroll_x(const x68k_bus_t *bus) {
    uint16_t r10 = ((uint16_t)bus->crtc_regs[0x14] << 8) | bus->crtc_regs[0x15];
    return (int)(r10 & 0x3FFu);
}

/* CRTC レジスタ番号 reg（R12/R13/R14...）の 10bit スクロール値を読む。
 * レジスタ N は crtc_regs[N*2](高バイト)/[N*2+1](低バイト) に格納される。
 */
static int gvram_reg_scroll(const x68k_bus_t *bus, int reg_num) {
    int off = reg_num * 2;
    uint16_t v = ((uint16_t)bus->crtc_regs[off] << 8) | bus->crtc_regs[off + 1];
    return (int)(v & 0x3FFu);
}

/* グラフィックYスクロール（R13/R15/R17/R19、idx=0..3がそれぞれ対応）。
 * 縦方向はテキスト Y と同様、垂直映像期間開始でラッチした値を使う。
 * raster.gr_y_zero[idx]はcrtc.cのcrtc_raster_emit_line()がgvram_reg_scroll(bus,13+2*idx)と
 * 同じ対応表でラッチ済み。ラスター未初期化/無効時は従来どおりライブ値にフォールバックする。
 */
static int gvram_scroll_y(const x68k_bus_t *bus, int idx) {
    if (!bus->crtc.raster_y_live && bus->crtc.raster.y_latched)
        return bus->crtc.raster.gr_y_zero[idx];
    return gvram_reg_scroll(bus, 13 + 2 * idx);
}

/* GVRAM の実画面レイアウト（『Inside X68000』
 * 図4）。1ドット=常に1ワード。 */
#define GVRAM_PLANE_SIZE     0x80000u /* 512ドットモードの1プレーン = 512行×1024バイト */
#define GVRAM_ROW_BYTES_512  1024u    /* 512ドット × 2バイト */
#define GVRAM_ROW_BYTES_1024 2048u    /* 1024ドット × 2バイト */
#define GVRAM_ROW_MASK_512   511u     /* 実画面512行での球面巻き戻し */
#define GVRAM_ROW_MASK_1024  1023u    /* 実画面1024行での球面巻き戻し */

/*
 * CRTC動作ポート ($E80481) bit1 = グラフィック画面高速クリア。
 *
 * 実機は「次のV-DISP立ち上がりから1フレーム（インターレース時2フレーム）かけて、
 * 表示ラスタ1本につき1行ずつ消す」が、ix68k は非同期タイミングモデルを
 * 持たないため、トリガ書き込み時に全行をまとめて消す（bit3のラスタコピーとは
 * 異なりbit1は書き込み時の単発トリガのまま。）。消す「行の集合」は
 * XEiJ CRTC.crtRapidClear() と同一にしてある。
 *
 * 縦: 表示高さ分だけ（実画面全体ではない。『Inside X68000』
 * 図28）。ページごとの
 *     Yスクロール(R13/R15/R17/R19)を加えて実画面内で球面的に巻き戻す。
 * 横: 実画面の幅いっぱい。XEiJ と同じ簡略化（図28
 * の「表示画面外の左右が消え残る」 挙動は再現しない）。
 */
void x68k_bus_crtc_rapid_clear(x68k_bus_t *bus) {
    int w, h;
    x68k_bus_get_display_size(bus, &w, &h); /* w は使わないが出力引数として必要 */
    (void)w;
    if (h <= 0)
        return;

    if (x68k_bus_is_gvram_1024dot(bus)) {
        /* 1024ドットモード: R21 の CP ビットを無視し、GVRAM 2MB
         * を単一空間として 扱う。Yスクロールは R13 (page0) のみ有効（XEiJ
         * crtRapidClear の crtMemoryModeCurr >= 4 分岐）。 */
        int sy = gvram_scroll_y(bus, 0);
        for (int y = 0; y < h; y++) {
            uint32_t row = (uint32_t)((y + sy) & (int)GVRAM_ROW_MASK_1024);
            memset(bus->gvram + row * GVRAM_ROW_BYTES_1024, 0, GVRAM_ROW_BYTES_1024);
        }
        return;
    }

    /* 512ドットモード: R21 bits3-0（CP、crtc_regs[0x2B]
     * の下位ニブル。ラスタコピーと
     * 同じビット）で選択された物理プレーンのみを消す。
     * ★x68k_bus_get_gvram_pages() は表示優先順(R1
     * GP0-3)に並べ替えるため、out[] の
     *   添字が物理ページ番号と一致しない。ここでは流用せず生のプレーン番号を使う。
     * ★Yスクロールの添字も「物理プレーン番号」であり表示ページ順ではない
     *   （256色モードの表示page1がR17を使うのとは別の対応。XEiJ準拠）。
     */
    uint8_t cc = (uint8_t)(bus->crtc_regs[0x2B] & 0x0Fu);
    for (int p = 0; p < 4; p++) {
        if (!((cc >> p) & 1u))
            continue; /* このプレーンはクリアしない */
        uint32_t base = (uint32_t)p * GVRAM_PLANE_SIZE;
        int sy = gvram_scroll_y(bus, p); /* p=0..3 → R13/R15/R17/R19 */
        for (int y = 0; y < h; y++) {
            uint32_t row = (uint32_t)((y + sy) & (int)GVRAM_ROW_MASK_512);
            memset(bus->gvram + base + row * GVRAM_ROW_BYTES_512, 0, GVRAM_ROW_BYTES_512);
        }
    }
}

/*
 * CRTC動作ポート ($E80481) bit3 = テキストラスタコピー実行。
 * R22高バイト($E8002C)のラスタブロックをR22低バイト($E8002D)へコピーする
 * （1ラスタブロック=4ピクセル行=512バイト/プレーン）。対象プレーンはR21
 * bits3-0（crtc_regs[0x2B]下位ニブル）。
 *
 * 呼び出し元は2箇所ある: ポート書き込み時（即時1回、MPX68K f59c07dの互換経路と
 * 同じ）と、bit3が1の間の各水平フロントポーチ（crtc_raster_emit_line()）。XEiJ CRTC.java（crtRasterCopyOn/crtDoRasterCopy、L233-235コメント）は
 * bit3を「0を書くまで1のまま保持されるMPU側スイッチであり、0→1のトリガーでも
 * CRTCの動作終了フラグでもない」と明記しており、R22を書き換えなければ同じ内容の
 * コピーを毎フロントポーチ繰り返すだけなので冪等（呼びすぎても副作用はない）。
 */
void x68k_bus_crtc_raster_copy(x68k_bus_t *bus) {
    uint8_t src = bus->crtc_regs[0x2Cu];
    uint8_t dst = bus->crtc_regs[0x2Du];
    uint32_t src_off = (uint32_t)src * 512u;
    uint32_t dst_off = (uint32_t)dst * 512u;
    uint8_t cc = (uint8_t)(bus->crtc_regs[0x2B] & 0x0Fu);
    for (int p = 0; p < 4; p++) {
        if (!((cc >> p) & 1u))
            continue; /* このプレーンはコピーしない */
        uint8_t *plane = bus->tvram + (uint32_t)p * 0x20000u;
        if (src_off + 512u <= 0x20000u && dst_off + 512u <= 0x20000u)
            memcpy(plane + dst_off, plane + src_off, 512u);
    }
}

/* ビデオコントローラ
 * R1（$E82500、下位8ビット=GP3-GP0）でページ間表示優先順位を
 * 並べ替える。GP0フィールドの値=最前面に来るページ番号…GP3=最背面
 * （『Inside X68000』 3591行/3648-3665行）。
 * 4ページモードはページ番号の単純な並べ替え。2ページモードは実機仕様で
 * GP1:GP0（下位4bit）が "0100"=page0優先／"1110"=page1優先 の2パターンのみ
 * 定義されている（同3662-3664行）ため、その2値だけを解釈する。 */
static void gvram_apply_priority(const x68k_bus_t *bus, gvram_page_t out[4], int npages) {
    uint8_t r1_lo = bus->crtc_regs[0x2501];
    if (npages == 4) {
        int gp[4] = {r1_lo & 0x3, (r1_lo >> 2) & 0x3, (r1_lo >> 4) & 0x3, (r1_lo >> 6) & 0x3};
        int used[4] = {0, 0, 0, 0};
        gvram_page_t reordered[4];
        int filled = 0;
        for (int i = 0; i < 4; i++) {
            int p = gp[i];
            if (!used[p]) {
                reordered[filled++] = out[p];
                used[p] = 1;
            }
        }
        for (int p = 0; p < 4 && filled < 4; p++)
            if (!used[p]) {
                reordered[filled++] = out[p];
                used[p] = 1;
            }
        for (int i = 0; i < 4; i++)
            out[i] = reordered[i];
    } else if (npages == 2) {
        if ((r1_lo & 0x0F) == 0x0E) { /* GP1:GP0 = 1110 → page1優先 */
            gvram_page_t tmp = out[0];
            out[0] = out[1];
            out[1] = tmp;
        }
        /* 0100(page0優先)はデフォルト順と一致。それ以外の値は未定義として無視
         */
    }
}

/* ビデオコントローラ R2 ($E82600) の GS4-0 (bit4-0) からページ page_idx
 * の表示 可否を判定する（『Inside X68000』
 * 図31、3-5-2節、動作検証で確認）。 npages
 * は現在のグラフィック画面モードのページ数（1/2/4）。 1ページ:
 * bit3-0が全て0ならOFF（bit4は同一意味の冗長ビット） 2ページ:
 * page0=bit1-0、page1=bit3-2（各2bit、00=OFF、それ以外=ON） 4ページ: page N = bit
 * N（1bit、0=OFF、1=ON）
 */
static int gvram_page_visible(const x68k_bus_t *bus, int npages, int page_idx) {
    uint8_t gs = bus->crtc_regs[0x2601] & 0x0Fu;
    if (npages == 1)
        return gs != 0;
    if (npages == 2)
        return page_idx == 0 ? ((gs & 0x03u) != 0) : ((gs & 0x0Cu) != 0);
    return (gs & (1u << page_idx)) != 0;
}

/* ビデオコントローラ R2 ($E82600) の TON
 * (bit5)。0でテキスト画面(TVRAM)レイヤーを
 * 非表示にする（『Inside X68000』 図31）。 */
int x68k_bus_get_text_visible(const x68k_bus_t *bus) {
    return (bus->crtc_regs[0x2601] >> 5) & 1u;
}

/* ビデオコントローラ R2 ($E82600) の SON
 * (bit6)。0でスプライト+BG画面レイヤーを
 * 非表示にする（『Inside X68000』 図31）。 */
int x68k_bus_get_sprite_visible(const x68k_bus_t *bus) {
    return (bus->crtc_regs[0x2601] >> 6) & 1u;
}

/* ビデオコントローラ R2（$E82600）の半透明・特殊プライオリティの選択。
 * 『Inside X68000』3-5-1/3-5-2に従い、AHは他の半透明選択ビットより優先する。
 * B/P=0は一次資料で未定義だが、参照実装XEiJの`_XHCT`系ルーチンに合わせて
 * 「パレット出力のLSB(Iビット)で領域指定するHALFモード」として扱う
 * （video_controller_design.md §5）。
 * G/G=1やH/P=0とB/P=0の組み合わせはXEiJでも別規則(_XHCG/_XWC)であり
 * 本実装のスコープ外のためNORMALへ落とす（§10.6.2）。
 *
 * 本関数はR2のビット解釈のみを行い、表示モード（解像度・色数）は一切
 * 参照しない。
 */
void x68k_bus_get_r2_state(const x68k_bus_t *bus, x68k_r2_state_t *out) {
    uint8_t r2_hi = bus->crtc_regs[0x2600];
    int ah = (r2_hi >> 6) & 1u;
    int exon = (r2_hi >> 4) & 1u;
    int half = (r2_hi >> 3) & 1u;
    int bp = (r2_hi >> 2) & 1u;
    int gg = (r2_hi >> 1) & 1u;
    int gt = r2_hi & 1u;

    out->effect = X68K_R2_EFFECT_NORMAL;
    out->lsb_source = X68K_R2_LSB_PAGE;
    out->blend_second = 0;
    out->blend_text = 0;

    if (!bp) {
        if (!ah && exon && half && !gg && gt) {
            out->effect = X68K_R2_EFFECT_HALF;
            out->lsb_source = X68K_R2_LSB_COLOR;
            out->blend_text = 1;
        }
        return;
    }
    if (ah) {
        out->effect = X68K_R2_EFFECT_HALF_PALETTE0;
        return;
    }
    if (!exon)
        return;
    if (!half) {
        out->effect = X68K_R2_EFFECT_SPECIAL_PRIORITY;
        return;
    }
    if (!gg && !gt)
        return;
    out->effect = X68K_R2_EFFECT_HALF;
    out->blend_second = gg;
    out->blend_text = gt;
}

int x68k_bus_get_gvram_pages(const x68k_bus_t *bus, gvram_page_t out[4]) {
    gvram_color_mode_t cm = x68k_bus_get_gvram_color_mode(bus);

    if (x68k_bus_is_gvram_1024dot(bus)) {
        /* 1024×1024: GVRAM 2MBは1ページ（1ドット1ワードのリニア面）として扱われ、
         * 画面4象限(左上/右上/左下/右下)にどのVRAMバンクを出すかはビデオコントローラ
         * R1（$E82500）下位バイトのGP0-GP3が動的に決める。バンク選択自体は
         * x68k_render_gvram_line() 側（page_sel）で行うため、ここでは base_off は
         * 使わない（0のまま）。R12/R13 のみが有効（R14-R19
         * は無視。動作検証で判明、XEiJ DrawingMode.java
         * 1024dot系描画ルーチン準拠）。
         * 1024ドットは16色のみで、R20の色数指定（256色・65536色）にかかわらず
         * 16色として扱う。 */
        out[0].base_off = 0;
        out[0].sx = gvram_reg_scroll(bus, 12);
        out[0].sy = gvram_scroll_y(bus, 0); /* R13 */
        out[0].mask = 0x0F;
        out[0].visible = gvram_page_visible(bus, 1, 0);
        out[0].page_sel = bus->crtc_regs[0x2501];
        return 1;
    }

    switch (cm) {
    case GVRAM_COLOR_16:
        /* page N = gvram + N*0x80000, スクロールは R(12+2N)/R(13+2N) */
        for (int n = 0; n < 4; n++) {
            out[n].base_off = n * 0x80000;
            out[n].sx = gvram_reg_scroll(bus, 12 + 2 * n);
            out[n].sy = gvram_scroll_y(bus, n); /* R13/R15/R17/R19 */
            out[n].mask = 0x0F;
            out[n].visible = gvram_page_visible(bus, 4, n);
            out[n].page_sel = 0; /* 512ドットモードでは未使用 */
        }
        gvram_apply_priority(bus, out, 4);
        return 4;
    case GVRAM_COLOR_256:
        /* page0=R12/R13, page1=R16/R17（★R14/R15ではない。3-3-1節） */
        out[0].base_off = 0;
        out[0].sx = gvram_reg_scroll(bus, 12);
        out[0].sy = gvram_scroll_y(bus, 0); /* R13 */
        out[0].mask = 0xFF;
        out[0].visible = gvram_page_visible(bus, 2, 0);
        out[0].page_sel = 0; /* 512ドットモードでは未使用 */
        out[1].base_off = 0x80000;
        out[1].sx = gvram_reg_scroll(bus, 16);
        out[1].sy = gvram_scroll_y(bus, 2); /* ★R17（idx=2、R13+2*2=R17） */
        out[1].mask = 0xFF;
        out[1].visible = gvram_page_visible(bus, 2, 1);
        out[1].page_sel = 0; /* 512ドットモードでは未使用 */
        gvram_apply_priority(bus, out, 2);
        return 2;
    case GVRAM_COLOR_65536:
    default:
        out[0].base_off = 0;
        out[0].sx = gvram_reg_scroll(bus, 12);
        out[0].sy = gvram_scroll_y(bus, 0); /* R13 */
        out[0].mask = 0;
        out[0].visible = gvram_page_visible(bus, 1, 0);
        out[0].page_sel = 0; /* 512ドットモードでは未使用 */
        return 1;
    }
}

int x68k_bus_gvram_priority_over_text(const x68k_bus_t *bus) {
    /* R1 上位バイト（crtc_regs[0x2500]）: bit7-6=未定義, bit5-4=SP, bit3-2=TX,
     * bit1-0=GR */
    uint8_t r1_hi = bus->crtc_regs[0x2500];
    int tx = (r1_hi >> 2) & 0x3;
    int gr = r1_hi & 0x3;
    return gr < tx;
}

/*
 * R1上位バイトのGR/TX/SP優先順位（各2bit、00>01>10、11・同値は禁止）から、
 * GR・TX・SPの3レイヤの背面→前面描画順を返す（図17、3-2-1節）。
 * x68k_bus_gvram_priority_over_text() はGR/TXの2値判定のみでテスト互換のため
 * 残置し、SPを含む3レイヤの合成には本関数を使う。
 */
void x68k_bus_get_screen_order(const x68k_bus_t *bus, int order[3]) {
    uint8_t hi = bus->crtc_regs[0x2500];
    int prio[3] = {hi & 3, (hi >> 2) & 3, (hi >> 4) & 3}; /* [0]=GR,[1]=TX,[2]=SP の優先値 */
    /* 同値タイブレーク（本来は禁止設定だが安全側の既定順）:
     * 前面ほど TX>SP>GR、
     * すなわち既存デフォルト（R1全0でGR=TX=0となる想定外時）に近い挙動を保つ。
     */
    int tie[3] = {2, 0, 1};
    int idx[3] = {0, 1, 2};
    /* prio昇順→tie昇順で front-to-back に安定ソートする */
    for (int i = 0; i < 3; i++)
        for (int j = i + 1; j < 3; j++) {
            int a = idx[i], b = idx[j];
            if (prio[a] > prio[b] || (prio[a] == prio[b] && tie[a] > tie[b])) {
                int t = idx[i];
                idx[i] = idx[j];
                idx[j] = t;
            }
        }
    order[0] = idx[2]; /* back-to-front（front-to-backの逆順） */
    order[1] = idx[1];
    order[2] = idx[0];
}

void x68k_bus_record_putmes(x68k_bus_t *bus, int row, int col, int len, const char *text) {
    if (!bus || row < 0 || row >= 64 || col < 0 || col >= 128 || len <= 0)
        return;
    /* col+len が桁数上限を超える入力を記録時点でクランプする（呼び出し元の
     * 丸めに依存しない防御。col は上のガードで [0,PUTMES_TEXT_MAX) 保証済み）。
     * スナップショット/参照時のセル走査が範囲外の桁を読まないための前提。 */
    if (len > PUTMES_TEXT_MAX - col)
        len = PUTMES_TEXT_MAX - col;
    /* 同一(行,桁開始位置)への再描画で幅だけ変わった場合、古いエントリを残すと
     * 新エントリが覆いきれない末尾桁に古い文字が残留する。ただし1行が複数回の
     * PUTMES呼び出し(桁位置違い)で分割構成される
     * ケースもあるため（例:
     * 「無効なメディア」ダイアログの中止/再実行/無視行）、
     * 無効化は行全体ではなく同一開始桁のみに限定する。 */
    for (int i = 0; i < bus->putmes_ring_cnt; i++) {
        putmes_entry_t *old = &bus->putmes_ring[i];
        if (old->valid && old->row == (uint8_t)row && old->col == (uint8_t)col)
            old->valid = 0;
    }
    putmes_entry_t *e = &bus->putmes_ring[bus->putmes_ring_pos];
    e->valid = 1;
    e->row = (uint8_t)row;
    e->col = (uint8_t)col;
    e->len = (uint8_t)len;
    /* このフックは _B_PUTMES の実行「前」に呼ばれるため、描画後のTVRAM内容は
     * ここでは取れない。記録フレーム番号だけ控えておき、実際のスナップショットは
     * x68k_bus_putmes_snapshot_tick() が後続のフレーム境界で行う。 */
    e->snapped = 0;
    e->record_frame = bus->putmes_frame_counter;
    memcpy(e->text, text, (size_t)len);
    bus->putmes_ring_pos = (bus->putmes_ring_pos + 1) % PUTMES_RING_SIZE;
    if (bus->putmes_ring_cnt < PUTMES_RING_SIZE)
        bus->putmes_ring_cnt++;
}

/* (row,col) セルのTVRAM内容を4プレーン分まとめて out[64] へ読み出す。
 * out のレイアウトは「プレーン p のスキャンライン y = out[p*16+y]」。
 * row は画面上のテキスト行、scroll_y は R11 由来のYスクロール。
 * out_all_zero が非NULLなら、64バイトすべてが0かどうかを返す。
 * アドレス計算は x68k_bus_tvram_to_text() のセル読み出しと同一式。 */
static void putmes_cell_read(const x68k_bus_t *bus, int row, int col, int scroll_y,
                             uint8_t out[PUTMES_CELL_RAW_BYTES], int *out_all_zero) {
    int all_zero = 1;
    for (int p = 0; p < 4; p++) {
        const uint8_t *plane = bus->tvram + (uint32_t)p * TVRAM_PLANE_SIZE;
        for (int y = 0; y < 16; y++) {
            int tvram_row = (row * 16 + y + scroll_y) & 1023;
            /* tvram_row*128+col は最大 1023*128+127 = 0x1FFFF で、
             * 常にプレーン内（TVRAM_PLANE_SIZE 未満）に収まる。 */
            uint8_t v = plane[tvram_row * 128 + col];
            out[p * 16 + y] = v;
            if (v)
                all_zero = 0;
        }
    }
    if (out_all_zero)
        *out_all_zero = all_zero;
}

void x68k_bus_putmes_snapshot_tick(x68k_bus_t *bus) {
    if (!bus)
        return;
    /* まずこのフレーム分の実行が完了したことを記録する。 */
    bus->putmes_frame_counter++;
    int scroll_y = x68k_bus_get_tvram_scroll_y(bus);
    for (int i = 0; i < bus->putmes_ring_cnt; i++) {
        putmes_entry_t *e = &bus->putmes_ring[i];
        if (!e->valid || e->snapped)
            continue;
        /* 記録フレーム自身の境界（差1）ではまだ描画途中の可能性がある。
         * 差が2以上＝記録フレームの次のフレームまで完走してから取る。
         * 符号無し差分なのでカウンタのラップアラウンドでも正しく判定される。 */
        if ((uint32_t)(bus->putmes_frame_counter - e->record_frame) < 2u)
            continue;
        int has_nonspace = 0;
        int all_cells_zero = 1;
        for (int k = 0; k < (int)e->len; k++) {
            int cell_zero = 1;
            putmes_cell_read(bus, e->row, (int)e->col + k, scroll_y, e->raw[k], &cell_zero);
            if (!cell_zero)
                all_cells_zero = 0;
            if (e->text[k] != ' ')
                has_nonspace = 1;
        }
        e->snapped = 1;
        /* 非空白を描いたはずのセルが4プレーンとも全ゼロ＝猶予期間内に
         * 消去された（画面上に存在しない）ことが確定しているので捨てる。 */
        if (has_nonspace && all_cells_zero)
            e->valid = 0;
    }
}

void x68k_bus_set_ank_font_override(x68k_bus_t *bus, uint32_t addr) {
    if (!bus)
        return;
    bus->ank_font_override_addr = addr;
    bus->ank_font_override_valid = 1;
}

void x68k_bus_clear_ank_font_override(x68k_bus_t *bus) {
    if (!bus)
        return;
    bus->ank_font_override_valid = 0;
}

/* HIOCS PLUS等の常駐ドライバがIOCS $16(_FNTADR)を差し替えて
 * ANKフォントを実行時変更していると、x68k_bus_tvram_to_text() のROM固定
 * オフセット参照では実際の描画パターンと一致しない。差し替え後の実アドレスは
 * 常駐ドライバ次第でエミュレータ側からは分からないため、ゲスト側で実際に
 * _FNTADR を呼び、結果を _B_PRINT 経由で "FNTADR=XXXXXXXX;" として報告して
 * もらう（tools-x68k/fntadr/fntadr.s）。iocs.c の _B_PRINT($21) ハンドラが
 * 出力全文をここへ渡し、マーカーを検出したら x68k_bus_set_ank_font_override()
 * を呼ぶ。マーカーに一致しない通常の出力はそのまま無視する（何もしない）。
 * 戻り値はマーカーを検出・適用したら1、しなければ0（テスト用）。 */
int x68k_bus_scan_fntadr_report(x68k_bus_t *bus, const uint8_t *buf, int len) {
    static const char prefix[] = "FNTADR=";
    const int plen = (int)(sizeof(prefix) - 1);
    for (int i = 0; i + plen + 9 <= len; i++) {
        if (memcmp(buf + i, prefix, (size_t)plen) != 0)
            continue;
        uint32_t addr = 0;
        int ok = 1;
        for (int k = 0; k < 8; k++) {
            uint8_t c = buf[i + plen + k];
            uint8_t v;
            if (c >= '0' && c <= '9')
                v = (uint8_t)(c - '0');
            else if (c >= 'A' && c <= 'F')
                v = (uint8_t)(c - 'A' + 10);
            else if (c >= 'a' && c <= 'f')
                v = (uint8_t)(c - 'a' + 10);
            else {
                ok = 0;
                break;
            }
            addr = (addr << 4) | v;
        }
        if (ok && buf[i + plen + 8] == ';') {
            x68k_bus_set_ank_font_override(bus, addr);
            return 1;
        }
    }
    return 0;
}

void x68k_bus_putmes_clear(x68k_bus_t *bus) {
    if (!bus)
        return;
    /* ring_cnt を 0 にするだけでは走査対象外になった古いエントリが
     * valid=1 のまま残り、以後 ring_cnt が増えたときに復活してしまう。 */
    for (int i = 0; i < PUTMES_RING_SIZE; i++)
        bus->putmes_ring[i].valid = 0;
    bus->putmes_ring_pos = 0;
    bus->putmes_ring_cnt = 0;
    /* putmes_frame_counter はリセットしない（record_frame との差分判定は
     * カウンタの連続性だけに依存し、リングが空なら比較対象も無い）。 */
}

/* (row,col) セルの _B_PUTMES キャプチャ文字を返す（無ければ 0）。
 * 古い順に走査し、範囲が重なる場合は新しいエントリが勝つ（後勝ち）。
 * スナップショット済みのエントリは、描画直後のセル内容と現在のTVRAM内容が
 * 完全一致する場合のみ採用する（画面から消えたキャプチャを返さないため）。
 * 未スナップショット（記録直後の数フレーム）は検証基準が無いので採用する。
 */
static uint8_t putmes_lookup(const x68k_bus_t *bus, int row, int col, int scroll_y) {
    uint8_t result = 0;
    int n = bus->putmes_ring_cnt;
    int start = (bus->putmes_ring_pos - n + PUTMES_RING_SIZE) % PUTMES_RING_SIZE;
    for (int i = 0; i < n; i++) {
        const putmes_entry_t *e = &bus->putmes_ring[(start + i) % PUTMES_RING_SIZE];
        if (!e->valid || e->row != row || col < e->col || col >= (int)e->col + (int)e->len)
            continue;
        if (e->snapped) {
            uint8_t cur[PUTMES_CELL_RAW_BYTES];
            putmes_cell_read(bus, row, col, scroll_y, cur, NULL);
            if (memcmp(cur, e->raw[col - e->col], sizeof(cur)) != 0)
                continue;
        }
        result = (uint8_t)e->text[col - e->col];
    }
    return result;
}

/* ANKフォント差し替え報告(ank_font_override)が指すアドレスから
 * フォントパターンを読む。報告アドレスは実バスアドレス（RAMのフォント
 * バッファ、またはROM/CGROMそのもの）なので、bus_read_rom8()と同じ
 * オフセット変換（iocs_rom_offset）でiocs_romを引く。
 * x68k_bus_peek8()を使わないのは、crtc.cをx68k_bus.cとリンクしない
 * テストターゲット（sprite_render_test等）を壊さないため。 */
static uint8_t ank_font_read_byte(const x68k_bus_t *bus, uint32_t addr) {
    if (addr < RAM_BUF_SIZE)
        return bus->ram[addr];
    if (addr >= IOCS_ROM_BASE) {
        uint32_t off = iocs_rom_offset(addr);
        if (off < IOCS_ROM_SIZE)
            return bus->iocs_rom[off];
    }
    return 0;
}

int x68k_bus_tvram_to_text(const x68k_bus_t *bus, int cols, int rows, uint8_t *out,
                           int out_cap) {
    /* CGROM ANK: iocs_rom オフセット 0x3A000 から 256 エントリ × 16 バイト */
    /* インデックス = (ch + 0x80) & 0xFF  →  ch = (idx + 0x80) & 0xFF */
    const uint8_t *cgrom_ank = bus->iocs_rom + 0x3A000;
    /* HIOCS PLUS等がIOCS $16(_FNTADR)を差し替えてANKフォントを
     * 実行時変更していると、上のROM固定オフセットは実際に画面へ描かれている
     * パターンと一致しない。ank_font_override_valid が立っていれば、ゲスト側
     * 報告プログラム(tools-x68k/fntadr/fntadr.s)が申告した実アドレスから
     * x68k_bus_peek8()（副作用なし読み出し）でパターンを読む。 */
    int ank_override = bus->ank_font_override_valid;
    uint32_t ank_override_addr = bus->ank_font_override_addr;
    const uint8_t *tvram0 = bus->tvram; /* Plane 0 = 0xE00000 */
    int scroll_y = x68k_bus_get_tvram_scroll_y(bus);

    /* CGROM パターン →
     * 文字コードの逆引きテーブルを構築（256エントリ, 各16バイト） */
    /* ここでは線形探索する（1回の READ_BITMAP_TEXT で数千回の比較:
     * 十分高速） */

    int out_len = 0;
    for (int row = 0; row < rows && out_len < out_cap - 2; row++) {
        int line_start = out_len;
        int last_nonspace = -1;

        for (int col = 0; col < cols && out_len < out_cap - 1; col++) {
            /* _B_PUTMES キャプチャがあれば最優先で使う。反転表示セルは
             * ビットパターンが全ゼロ（＝下のall_zero早期return分岐）になったり
             * 別グリフに偶然一致したりし得るため、ピクセル解析より先に判定する。
             * ただし採用されるのは、描画後スナップショットと現在のTVRAM内容が
             * 一致する（＝まだ画面に出ている）キャプチャだけ。消えたセルは
             * ここで 0 が返り、下のピクセル解析へフォールバックする。
             */
            uint8_t cap = putmes_lookup(bus, row, col, scroll_y);
            if (cap) {
                out[out_len++] = cap;
                if (cap != ' ')
                    last_nonspace = out_len - line_start;
                continue;
            }

            /* TVRAM plane 0 からこの文字セルの 16 scanline バイトを読む */
            uint8_t cell[16];
            for (int y = 0; y < 16; y++) {
                int tvram_row = (row * 16 + y + scroll_y) & 1023;
                /* tvram_row*128+col は最大 1023*128+127 = 0x1FFFF で、
                 * 常にプレーン0の範囲（TVRAM_PLANE_SIZE 未満）に収まる。 */
                cell[y] = tvram0[tvram_row * 128 + col];
            }

            /* 全 0 → スペース */
            int all_zero = 1;
            for (int y = 0; y < 16; y++)
                if (cell[y]) {
                    all_zero = 0;
                    break;
                }
            if (all_zero) {
                out[out_len++] = ' ';
                continue;
            }

            /* CGROM 全エントリと照合。
             * ank_override時は実アドレスを ch(生の文字コード)*16 で直接引く
             * （_FNTADR実装のROM逆アセンブルで確認した式。d1=0で
             * ちょうどbase+0が返ることを確認済み）。ROM固定オフセット側は
             * 従来通りCGROM_XEiJ.DATのファイルレイアウトに合わせた
             * (idx+0x80)&0xFF 変換を使う。 */
            uint8_t found = '?';
            for (int idx = 0; idx < 256; idx++) {
                uint8_t pat_buf[16];
                const uint8_t *pat;
                uint8_t ch;
                if (ank_override) {
                    ch = (uint8_t)idx;
                    for (int y = 0; y < 16; y++)
                        pat_buf[y] = ank_font_read_byte(
                            bus, ank_override_addr + (uint32_t)ch * 16 + (uint32_t)y);
                    pat = pat_buf;
                } else {
                    ch = (uint8_t)((idx + 0x80) & 0xFF);
                    pat = cgrom_ank + idx * 16;
                }
                int match = 1;
                for (int y = 0; y < 16; y++) {
                    if (cell[y] != pat[y]) {
                        match = 0;
                        break;
                    }
                }
                if (match) {
                    found = ch;
                    break;
                }
            }
            out[out_len++] = found;
            if (found != ' ')
                last_nonspace = out_len - line_start;
        }

        /* 行末の trailing spaces を削除 */
        if (last_nonspace >= 0)
            out_len = line_start + last_nonspace;
        else
            out_len = line_start; /* 空行 */

        if (out_len < out_cap - 1)
            out[out_len++] = '\n';
    }
    if (out_len < out_cap)
        out[out_len] = '\0';
    return out_len;
}

/*
 * グラフィックのメモリモード（色数・実画面サイズ）はビデオコントローラ
 * R0（$E82400）の bit2-0（GrMM）が一次情報。
 *
 *   0 = 512ドット16色 / 1 = 512ドット256色 / 3 = 512ドット65536色 /
 *   4 = 1024ドット16色（bit2 = 実画面1024×1024、bit1-0 = 色数）
 *
 * CRTC R20 の bit10-8 も同じ値を保持する。ROM が両者を必ずセットで書くため
 * （IOCS $91 = ROM $FFB114 は「R20上位バイト &= $F8; |= d1&7」と
 * 「$E82400 = d1&7」を続けて実行し、_CRTMOD のCRTC設定ルーチン $FF67D4 も
 * R20 をテーブル値で書いた直後に $E82400 = (R20>>8)&7 を書く）、
 * どちらを見ても答えは同じになる。参照実装 XEiJ VideoController.java も
 * REG1($E82400) の bit2-0 で判定している。
 *
 * かつては ROM _CRTMOD ハンドラが書く RAM ワークエリア $93C（CRTMOD モード番号）
 * から判定していたが、これは _CRTMOD/_G_CLR_ON を経由せず CRTC/ビデオコントローラ
 * を直接叩くソフトに追従できない。mint.x のグラフィック表示は set_crt_mode
 * （gvon.s）で $E82400 と R20 を直接書いてモードを切り替えるため、gキーで
 * 65536色→16色→256色と切り替えても $93C は動かず、256色の画面が65536色のまま
 * 描画されていた（画面が一切変化しない）。
 *
 * 未定義値 2/6（bit1=1,bit0=0）は bit1 を優先して65536色として扱う（XEiJ準拠）。
 */
gvram_color_mode_t x68k_bus_get_gvram_color_mode(const x68k_bus_t *bus) {
    /* 0xE82400 = crtc_regs[0x2400]（上位バイト）/ [0x2401]（下位バイト）。
     * GrMM は下位バイトの bit2-0。 */
    uint8_t grmm = bus->crtc_regs[0x2401] & 0x07u;
    if (grmm & 0x02u)
        return GVRAM_COLOR_65536;
    if (grmm & 0x01u)
        return GVRAM_COLOR_256;
    return GVRAM_COLOR_16;
}

int x68k_bus_is_gvram_1024dot(const x68k_bus_t *bus) {
    return (bus->crtc_regs[0x2401] >> 2) & 1u;
}

void x68k_bus_get_gfx_palette(const x68k_bus_t *bus, uint16_t pal[256]) {
    memcpy(pal, x68k_bus_get_gfx_palette_raw(bus), 256 * sizeof(*pal));
}

void x68k_bus_get_text_palette(const x68k_bus_t *bus, uint16_t pal[16]) {
    /* 0xE82200 = crtc_regs[0x2200]: テキストが参照する先頭16エントリ分
     * （ブロック0）だけを返す。実体は256エントリ共用テーブル、参照。
     */
    memcpy(pal, x68k_bus_get_text_palette_raw(bus), 16 * sizeof(*pal));
}

void x68k_bus_get_text_palette_all(const x68k_bus_t *bus, uint16_t pal[256]) {
    memcpy(pal, x68k_bus_get_text_palette_raw(bus), 256 * sizeof(*pal));
}

const uint16_t *x68k_bus_get_gfx_palette_raw(const x68k_bus_t *bus) {
    /* 単体テスト等のスタック上busはCRTCレジスタを直接設定する既存契約を保つ。
     * 実機パス(x68k_bus_create()/x68k_bus_write8)は常にcache_readyで、この走査に入らない。 */
    if (!bus->crtc.pal_cache_ready) {
        crtc_t *crtc = (crtc_t *)&bus->crtc;
        for (int i = 0; i < 256; i++)
            crtc->gfx_pal_raw[i] = ((uint16_t)bus->crtc_regs[0x2000 + i * 2] << 8) |
                                   bus->crtc_regs[0x2000 + i * 2 + 1];
    }
    return !bus->crtc.raster_pal_live && bus->crtc.pal_latched ? bus->crtc.gfx_pal_raw_latched
                                                               : bus->crtc.gfx_pal_raw;
}

const uint16_t *x68k_bus_get_text_palette_raw(const x68k_bus_t *bus) {
    if (!bus->crtc.pal_cache_ready) {
        crtc_t *crtc = (crtc_t *)&bus->crtc;
        for (int i = 0; i < 256; i++)
            crtc->text_pal_raw[i] = ((uint16_t)bus->crtc_regs[0x2200 + i * 2] << 8) |
                                    bus->crtc_regs[0x2200 + i * 2 + 1];
    }
    return !bus->crtc.raster_pal_live && bus->crtc.pal_latched ? bus->crtc.text_pal_raw_latched
                                                               : bus->crtc.text_pal_raw;
}

/* ARGB8888 変換済みパレット。書き込み時に x68k_bus_write8() が
 * eager更新済みのため、ここは単にポインタを返すだけ（コスト0）。 */
const uint32_t *x68k_bus_get_gfx_palette_argb(const x68k_bus_t *bus) {
    (void)x68k_bus_get_gfx_palette_raw(bus);
    if (!bus->crtc.pal_cache_ready) {
        crtc_t *crtc = (crtc_t *)&bus->crtc;
        for (int i = 0; i < 256; i++)
            crtc->gfx_pal_argb[i] = palette_grb555_to_argb(crtc->gfx_pal_raw[i]);
    }
    return !bus->crtc.raster_pal_live && bus->crtc.pal_latched ? bus->crtc.gfx_pal_argb_latched
                                                               : bus->crtc.gfx_pal_argb;
}

/* $E82200-$E823FF、256エントリ全体を返す（『Inside X68000』 図33）。
 * テキスト画面はこのうちエントリ0-15(ブロック0)だけを、スプライト+BG画面は
 * ブロック1-15(エントリ16-255)を参照する共用テーブル。 */
const uint32_t *x68k_bus_get_text_palette_argb(const x68k_bus_t *bus) {
    (void)x68k_bus_get_text_palette_raw(bus);
    if (!bus->crtc.pal_cache_ready) {
        crtc_t *crtc = (crtc_t *)&bus->crtc;
        for (int i = 0; i < 256; i++)
            crtc->text_pal_argb[i] = palette_grb555_to_argb(crtc->text_pal_raw[i]);
    }
    return !bus->crtc.raster_pal_live && bus->crtc.pal_latched ? bus->crtc.text_pal_argb_latched
                                                               : bus->crtc.text_pal_argb;
}

unsigned x68k_bus_get_contrast(const x68k_bus_t *bus) {
    return bus->crtc_regs[0xE001] & 0x0Fu;
}

unsigned x68k_bus_get_contrast_scaled(const x68k_bus_t *bus) {
    return 45u - bus->crtc.contrast_attenuation;
}

/* XEiJ VideoController.java の45段階線形スケールに倣う。一次資料『Inside X68000』は
 * カーブそのものを規定していないため、これは参照実装に基づく近似である
 * （docs/ix68k-internals/design/video_controller_design.md §6）。 */
const uint8_t *x68k_bus_get_contrast_lut(x68k_bus_t *bus) {
    unsigned scaled = x68k_bus_get_contrast_scaled(bus);
    if (bus->crtc.contrast_lut_scaled != (int)scaled) {
        for (unsigned v = 0; v < 256u; v++)
            bus->crtc.contrast_lut[v] = (uint8_t)((v * scaled + 22u) / 45u);
        bus->crtc.contrast_lut_scaled = (int)scaled;
    }
    return bus->crtc.contrast_lut;
}

void x68k_bus_get_display_size(const x68k_bus_t *bus, int *w, int *h) {
    /* R20 は 0xE80028 (word): crtc_regs[0x28]=高バイト, crtc_regs[0x29]=低バイト */
    uint8_t r20_hi = bus->crtc_regs[0x28];
    uint8_t r20_lo = bus->crtc_regs[0x29];
    /* R20 ワード自体が完全に 0（起動直後、_CRTMOD
     * 未実行）の場合のみ未初期化とみなす。 HD=00/VD=00
     * はそれ自体が正当な値（256×256 表示、mode2/6/10/14 等）であり、 hd/vd
     * の2ビットだけで判定すると誤って512×512にフォールバックしてしまう
     * （全モード検証で発見）。 */
    if (r20_hi == 0 && r20_lo == 0) {
        *w = 512;
        *h = 512;
        return;
    }
    /* 水平方向は R20 の HD ビット(bit1-0)ではなく
     * R02/R03（水平映像期間の実カラム数） から直接算出する。XEiJ
     * (CRTC.java InitialStage.tick: hDisp = R03-R02) 準拠。 R20 の HD ビットは標準
     * CRTMOD の全モードでは HD カラム数と常に一致するため
     * 通常は等価だが、水平解像度ビットはオシレータ分周比の選択も兼ねており、
     * 標準 CRTMOD 表にない「正方形ドットモード」（otoko.c 等が CRTC
     * を直接 叩いて作る俗称"384x256"モード）では両者が食い違う。
     * R02/R03
     * から実カラム数を求める方が実際の可視ドット数と常に一致し、より正確。
     *
     * この食い違いは表示サイズ計算に閉じた話であり、スプライト/BGレイヤの
     * 表示可否判定（x68k_bus_get_sprite_scene()）はCRTC R20をそもそも参照しない
     * （$EB0810 はスプライトコントローラ自身の独立したレジスタのため）。
     * 過去にsprite.c側がこのR20の食い違いを根拠に誤ってR20で判定しようとし、
     * まさにOTOKO.X自身のスプライトを消してしまったことがある。 */
    uint16_t r02 = ((uint16_t)bus->crtc_regs[0x04] << 8) | bus->crtc_regs[0x05];
    uint16_t r03 = ((uint16_t)bus->crtc_regs[0x06] << 8) | bus->crtc_regs[0x07];
    int hd_columns = (int)r03 - (int)r02;
    *w = (hd_columns > 0) ? hd_columns * 8 : 256;
    int vd = (r20_lo >> 2) & 0x3;
    static const int vdtab[] = {256, 512, 1024, 512};
    *h = vdtab[vd];
    /* mode17/18（実画面1024・24.70kHz系、幅1024）は VD の2値だけでは表示高さ
     * 424/848 を区別できず、vdtab は常に1024を返してしまう。
     * 表示幅が1024（R02/R03由来）の場合に限り、R06/R07（垂直表示開始/終了）の
     * 差分から実際の行数を求める。ROMの_CRTMODテーブル実測値では mode17
     * (vd==1) は差分がそのまま424、mode18 (vd==2) は差分424をインターレースで
     * 2倍した848になる（『X68030 InsideOut』3節「インターレースは...垂直方向の
     * 表示ドット数を2倍にする」）。標準モード(幅256/512/768)はこの分岐に入らず
     * 無変更。mode18のインターレース自体（1ラスタおきの奇偶フィールド交互表示）は
     * x68k_bus_crtc_raster_frame_start() のラスターモデルでは未対応のまま残る
     * （表示高さの値だけをROM仕様どおりに修正する狭い対応、2026-09-02）。 */
    if (*w == 1024) {
        uint16_t r06 = ((uint16_t)bus->crtc_regs[0x0C] << 8) | bus->crtc_regs[0x0D];
        uint16_t r07 = ((uint16_t)bus->crtc_regs[0x0E] << 8) | bus->crtc_regs[0x0F];
        int vd_lines = (int)r07 - (int)r06;
        if (vd_lines > 0)
            *h = (vd == 2) ? vd_lines * 2 : vd_lines;
    }
}

/*
 * 表示1ドットが占める横幅を、69.552MHz系のドットクロック周期を1単位として返す。
 * XEiJ (CRTC.java CRT_DIVS/CRT_OSCS、実測値付きコメント) の分周比テーブル準拠。
 * hReso(R20 bit1-0)=0/1/2/3、HRL(0xE8E007 bit1)=0/1 の組み合わせで決まる。
 * hReso=2/3 (768dot/VGA系) は HRL の影響を受けない。
 *
 * 38.864MHz系(highReso=0、HRL無効)の実分周比は {8,4,8,8} だが、オシレータ周波数が
 * 異なるため 69.552MHz系とそのままでは比較できない。同じ表示ドット数のモードが同じ
 * 横幅になるよう 3/4 を掛けたスケールで扱う（15.98kHz系が実機では 31.50kHz系より
 * 横に広く映る差 — 1Hに占める表示期間が約84%対約70% — は無視する）。
 */
static int crtc_dot_width_units(int high_reso, int h_reso, int hrl) {
    static const int units_hrl0[4] = {6, 3, 2, 2};
    static const int units_hrl1[4] = {8, 4, 2, 2};
    static const int units_low_freq[4] = {6, 3, 6, 6}; /* 実分周比 {8,4,8,8} × 3/4 */
    if (!high_reso)
        return units_low_freq[h_reso & 0x3];
    return hrl ? units_hrl1[h_reso & 0x3] : units_hrl0[h_reso & 0x3];
}

/* CPUサイクル基準で走査線を追跡し、MFP割り込みとライン単位描画を駆動する。
 */
#define CRTC_DEFAULT_LINES_PER_FRAME \
    567 /* 31.5kHz標準相当。R04-R07が不正な間のフォールバック */

/* 走査線到達をMFPへ通知し、表示開始時の縦スクロール値をラッチする。
 */
static void crtc_raster_emit_line(x68k_bus_t *bus, int line) {
    mfp_raster_line_event(bus, line);

    /* CRTC動作ポート($E80481) bit3が1の間、各ラスタの水平フロントポーチ相当の
     * 時点でラスタコピーを継続実行する（XEiJ crtRasterCopyOn準拠）。
     * bit3はwrite8/write16経路でcrtc_regsへそのまま保持される（rapid_clearの
     * bit1と異なり自動クリアされない）ため、ここでは都度crtc_regsを見るだけでよい。 */
    if (bus->crtc_regs[0x481u] & 0x08u)
        x68k_bus_crtc_raster_copy(bus);

    x68k_raster_t *raster = &bus->crtc.raster;
    if (raster->valid && line == raster->vdisp_start) {
        uint16_t r11 = ((uint16_t)bus->crtc_regs[0x16] << 8) | bus->crtc_regs[0x17];
        raster->tx_y_zero = (int)(r11 & 0x3FFu);
        for (int i = 0; i < 4; i++) {
            raster->gr_y_zero[i] = gvram_reg_scroll(bus, 13 + 2 * i);
        }
        raster->y_latched = 1;
        if (!bus->crtc.raster_pal_live) {
            if (bus->crtc.pal_dirty || !bus->crtc.pal_latched) {
                memcpy(bus->crtc.gfx_pal_raw_latched, bus->crtc.gfx_pal_raw,
                       sizeof(bus->crtc.gfx_pal_raw));
                memcpy(bus->crtc.text_pal_raw_latched, bus->crtc.text_pal_raw,
                       sizeof(bus->crtc.text_pal_raw));
                memcpy(bus->crtc.gfx_pal_argb_latched, bus->crtc.gfx_pal_argb,
                       sizeof(bus->crtc.gfx_pal_argb));
                memcpy(bus->crtc.text_pal_argb_latched, bus->crtc.text_pal_argb,
                       sizeof(bus->crtc.text_pal_argb));
            }
            bus->crtc.pal_dirty = 0;
            bus->crtc.pal_latched = 1;
        }
    }
    if (raster->valid && line == raster->vdisp_start + raster->vdisp_lines) {
        x68k_bus_mfp_vdisp_tick(bus);
    }
}

/* フレーム先頭で呼ぶ。CRTC
 * R04-R07・表示高さからラスターパラメータを再導出する。 */
void x68k_bus_crtc_raster_frame_start(x68k_bus_t *bus, int cycles_per_frame, int display_h) {
    x68k_raster_t *raster = &bus->crtc.raster;
    unsigned target_attenuation = (15u - x68k_bus_get_contrast(bus)) * 3u;
    if (bus->crtc.contrast_attenuation < target_attenuation)
        bus->crtc.contrast_attenuation++;
    else if (bus->crtc.contrast_attenuation > target_attenuation)
        bus->crtc.contrast_attenuation--;
    bus->crtc.pal_latched = 0;

    uint16_t r04 = ((uint16_t)bus->crtc_regs[0x08] << 8) | bus->crtc_regs[0x09];
    uint16_t r05 = ((uint16_t)bus->crtc_regs[0x0A] << 8) | bus->crtc_regs[0x0B];
    uint16_t r06 = ((uint16_t)bus->crtc_regs[0x0C] << 8) | bus->crtc_regs[0x0D];
    uint16_t r07 = ((uint16_t)bus->crtc_regs[0x0E] << 8) | bus->crtc_regs[0x0F];

    int lines_per_frame = (int)r04 + 1;
    int vdisp_start = (int)r06 + 1;
    int vdisp_lines = (int)r07 - (int)r06;

    int valid = display_h > 0 && r05 < r06 && r06 < r07 && r07 <= r04 &&
                lines_per_frame >= 200 && lines_per_frame <= 1200 &&
                (vdisp_lines == display_h || vdisp_lines == display_h * 2);

    raster->display_h = display_h;
    raster->cycles_per_frame = cycles_per_frame;
    raster->valid = valid ? 1 : 0;
    if (valid) {
        raster->lines_per_frame = lines_per_frame;
        raster->vdisp_start = vdisp_start;
        raster->vdisp_lines = vdisp_lines;
        raster->dup = vdisp_lines / display_h;
    } else {
        raster->lines_per_frame = CRTC_DEFAULT_LINES_PER_FRAME;
        raster->vdisp_start = 0;
        raster->vdisp_lines = 0;
        raster->dup = 1;
    }

    raster->line = -1; /* まだどのラスタにも到達していない（最初のadvanceでline=0が発火する）
                        */
    raster->cycle_in_frame = 0;
    raster->y_latched = 0;
}

/* ラスターを cpu_cycles 分進める。新しく到達した絶対ラスタ番号を
 * out_lines[] へ返す。 */
int x68k_bus_crtc_raster_advance(x68k_bus_t *bus, int cpu_cycles, int *out_lines,
                                 int max_lines) {
    x68k_raster_t *raster = &bus->crtc.raster;
    raster->cycle_in_frame += cpu_cycles;

    int lines_per_frame =
        raster->lines_per_frame > 0 ? raster->lines_per_frame : CRTC_DEFAULT_LINES_PER_FRAME;
    int cycles_per_frame = raster->cycles_per_frame > 0 ? raster->cycles_per_frame : 1;

    /* target_lineをドリフト無く直接算出する。 */
    int target_line =
        (int)((int64_t)raster->cycle_in_frame * lines_per_frame / cycles_per_frame);
    if (target_line > lines_per_frame - 1)
        target_line = lines_per_frame - 1;

    int count = 0;
    while (raster->line < target_line) {
        raster->line++;
        crtc_raster_emit_line(bus, raster->line);
        if (count < max_lines)
            out_lines[count] = raster->line;
        count++;
    }
    return count;
}

/* フレーム末尾で呼ぶ。残りのラスタを一気に消化する（CPU停止時にラスターが
 * 止まってしまう問題への対策）。 */
int x68k_bus_crtc_raster_flush(x68k_bus_t *bus, int *out_lines, int max_lines) {
    x68k_raster_t *raster = &bus->crtc.raster;
    int lines_per_frame =
        raster->lines_per_frame > 0 ? raster->lines_per_frame : CRTC_DEFAULT_LINES_PER_FRAME;
    int target_line = lines_per_frame - 1;

    int count = 0;
    while (raster->line < target_line) {
        raster->line++;
        crtc_raster_emit_line(bus, raster->line);
        if (count < max_lines)
            out_lines[count] = raster->line;
        count++;
    }
    return count;
}

/* 絶対ラスタ番号 → 表示行（0..display_h-1）。映像期間外、または dup
 * の中間ラスタなら -1。 */
int x68k_bus_crtc_raster_display_row(const x68k_bus_t *bus, int line) {
    const x68k_raster_t *raster = &bus->crtc.raster;
    if (!raster->valid)
        return -1;
    int rel = line - raster->vdisp_start;
    if (rel < 0 || rel >= raster->vdisp_lines)
        return -1;
    if (raster->dup <= 1)
        return rel;
    if (rel % raster->dup != 0)
        return -1;
    return rel / raster->dup;
}

int x68k_bus_crtc_raster_valid(const x68k_bus_t *bus) {
    return bus->crtc.raster.valid;
}

int x68k_bus_get_dot_width_units(const x68k_bus_t *bus) {
    uint8_t r20_hi = bus->crtc_regs[0x28];
    uint8_t r20_lo = bus->crtc_regs[0x29];
    /* R20 ワードが完全に 0 の間は _CRTMOD 未実行（起動直後）。
     * x68k_bus_get_display_size() の 512x512 フォールバックと揃え、
     * 512ドットモード相当（基準ウィンドウ幅 768 になる値）を返す。 */
    if (r20_hi == 0 && r20_lo == 0)
        return crtc_dot_width_units(1, 1, 0);
    int high_reso = (r20_lo >> 4) & 0x1;
    int h_reso = r20_lo & 0x3;
    int hrl = (bus->crtc_regs[0xE007] >> 1) & 0x1;
    return crtc_dot_width_units(high_reso, h_reso, hrl);
}
