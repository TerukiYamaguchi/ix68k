#include "bus/bus_internal.h"
#include <limits.h>
#include <string.h>

/* ---- USART RX FIFO（キーボード押下/離上バイト列） ---- */

#define MFP_IMRA_USART_RX   0x10u /* IMR-A/IER-A bit4 = RCV BF（受信バッファフル） */
#define MFP_IERB_TIMERD     0x10u /* IER-B/IMR-B bit4 = Timer D 割り込みイネーブル */
#define MFP_IERA_TIMERA     0x20u /* IER-A/IMR-A bit5 = Timer A 割り込みイネーブル */
#define MFP_TACR_EVENTCOUNT 0x08u /* TACR bit3 = イベントカウントモード */
#define MFP_IERA_RASTER                                                    \
    0x40u /* IER-A/IMR-A bit6 = ラスタ一致(GPIP6/CIRQ)割り込みイネーブル \
           */
#define MFP_IERA_HSYNC        0x80u /* IER-A/IMR-A bit7 = H-SYNC(GPIP7)割り込みイネーブル */
#define MFP_IERB_GPIP4        0x40u /* IER-B/IMR-B bit6 = GPIP4/V-DISP割り込みイネーブル */
#define MFP_IERB_POWER_SWITCH 0x04u /* IER-B/IMR-B bit2 = GPIP2/POW SW */
#define MFP_IERB_ALARM        0x01u /* IER-B/IMR-B bit0 = GPIP0/RTC ALARM */
#define MFP_IERA_TIMERB       0x01u /* IER-A/IMR-A bit0 = Timer B割り込みイネーブル */

/* USART TX（UDR書き込み）のキーボードコマンド分類（XEiJ MC68901.java L1453-1467 準拠）。
 * $80-$FF=LED（~値の下位7bit、1で点灯）、$54-$57=LED明るさ、$60-$6F=リピート開始時間、
 * $70-$7F=リピート間隔。$40-$53・$58-$5F（MSCTRL、ロック、TV制御等）は状態を持たない。 */
#define KBD_CMD_LED_MIN       0x80u
#define KBD_LED_MASK          0x7Fu
#define KBD_CMD_BRIGHT_MIN    0x54u
#define KBD_CMD_BRIGHT_MAX    0x57u
#define KBD_BRIGHT_MASK       0x03u
#define KBD_CMD_DELAY_MIN     0x60u
#define KBD_CMD_DELAY_MAX     0x6Fu
#define KBD_CMD_RATE_MIN      0x70u
#define KBD_CMD_RATE_MAX      0x7Fu
#define KBD_REPEAT_VALUE_MASK 0x0Fu

#define MFP_HZ 4000000u /* MFP(MC68901)クロックは実機で常時4MHz固定 */

static uint32_t mfp_gcd_u32(uint32_t a, uint32_t b) {
    while (b) {
        uint32_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

/* TBCR bits[3:0]からディレイモードのプリスケール添字を返す（0 = 動作しない）。
 * 1000(イベントカウント)・1001-1111(パルス幅測定)はX68000ではTBIがL固定で
 * 動作しないため0を返す（『Inside X68000』、XEiJもパルス幅測定をキャンセル扱い）。 */
static int mfp_tb_delay_mode(uint8_t tbcr) {
    return (tbcr & 0x08u) ? 0 : (int)(tbcr & 0x07u);
}

/* ラスタ一致比較値（R09/$E80012-13の下位10bit）を返す。mfp_raster_line_event()の
 * 割り込み判定とGPIP bit6(CIRQ)のポーリング読み出し(mfp_reg_read8)が同じ比較式を
 * 共有するためのヘルパ。 */
static int mfp_raster_compare(const x68k_bus_t *b) {
    return (((int)b->crtc_regs[0x12] << 8) | b->crtc_regs[0x13]) & 0x3FF;
}

/* GPIP bit4(V-DISP)がHIGHか（垂直表示期間中）。HIGH区間は絶対ラスタ
 * [vdisp_start, vdisp_start+vdisp_lines)（XEiJ CRTC.java L528/L533の
 * crtVDispStart..crtVIdleStart-1と同一）。crtc_raster_emit_line()（crtc.c）が
 * ch6(V-DISP)割り込みを発火するのと同じ境界を共有する。呼び出し側でraster.validを
 * 先に見ること（valid=0時はvdisp_start=vdisp_lines=0に潰されるため）。 */
static int mfp_raster_vdisp_high(const x68k_bus_t *b) {
    const x68k_raster_t *bm = &b->crtc.raster;
    int rel = bm->line - bm->vdisp_start;
    return rel >= 0 && rel < bm->vdisp_lines;
}

/* GPIP bit7(H-SYNC)用のライン内水平パラメータをR00-R03から求める（1カラム=8ドット、
 * XEiJ CRTC.java L1777-1780と同型）。R00-R03は7-0の8bitフィールド。設定不正
 * （順序が守られていない等）なら0を返し、呼び出し側はフォールバック位相へ落ちる。 */
static int mfp_raster_h_params(const x68k_bus_t *b, int *h_total, int *h_sync, int *h_front) {
    int r00 = b->crtc_regs[0x01], r01 = b->crtc_regs[0x03];
    int r02 = b->crtc_regs[0x05], r03 = b->crtc_regs[0x07];
    if (!(r01 < r02 && r02 < r03 && r03 < r00))
        return 0;
    *h_total = r00 + 1;
    *h_sync = r01 + 1;
    *h_front = r00 - r03 - 4;
    if (*h_front < 1)
        *h_front = 1;
    if (*h_front + *h_sync > *h_total)
        *h_front = *h_total - *h_sync;
    return *h_sync > 0 && *h_sync < *h_total;
}

/* GPIP bit7(H-SYNC)がHIGHか（水平同期パルス期間中）。raster.cycle_in_frameから
 * ライン内位置（カラム単位）を復元し、[h_front, h_front+h_sync)の間だけHIGHにする
 * （フロントポーチ→H-SYNCパルス→バックポーチ+映像期間、XEiJ CRTC.java
 * L1953-1962と同型）。読み出し副作用は無い純関数。CPU実行チャンク(200cy)が
 * 1ラスタ(約294cy)より粗いため位相は量子化されるが、周期的ロックはしない。 */
static int mfp_raster_hsync_high(const x68k_bus_t *b, int h_total, int h_sync, int h_front) {
    const x68k_raster_t *bm = &b->crtc.raster;
    int64_t rem = ((int64_t)bm->cycle_in_frame * bm->lines_per_frame) % bm->cycles_per_frame;
    int col = (int)(rem * h_total / bm->cycles_per_frame);
    return col >= h_front && col < h_front + h_sync;
}

/* USART RX が CPU へ割り込みを要求中か。
 * MC68901 では IPR がイベントをラッチし、CPU への要求は IPR & IMR で決まる。 */
static int mfp_usart_rx_requesting(const x68k_bus_t *b) {
    return mfp_ch_pending_test(&b->mfp, MFP_CH_USART_RX) && (b->mfp.imra & MFP_IMRA_USART_RX);
}

static int mfp_usart_rx_fifo_empty(const x68k_bus_t *b) {
    return b->mfp.usart_rx_head == b->mfp.usart_rx_tail;
}

static uint8_t mfp_usart_rx_fifo_pop(x68k_bus_t *b) {
    uint8_t v = b->mfp.usart_rx_fifo[b->mfp.usart_rx_head];
    b->mfp.usart_rx_head = (b->mfp.usart_rx_head + 1) % MFP_USART_RX_FIFO_SIZE;
    return v;
}

/* 実機の USART 受信は 2400bps・10ビット/バイト固定。
 * FIFO先頭バイトをUDRラッチへ昇格できる最短間隔をCPUサイクル数で返す。 */
static int mfp_usart_rx_min_interval_cycles(const x68k_bus_t *b) {
    return (int)(b->cpu_hz / 240u);
}

/* armed かつ未pending・ラッチ未消費(BF=0)・FIFO非空・最短転送間隔経過済みなら、
 * ベクタ設定済みを確認してFIFO先頭バイトをUDRラッチへ昇格しpendingへ上げる。
 * RAM[VR|12 * 4] が非ゼロになった時点で初めて発火させる（ROM 起動シーケンス中、
 * ハンドラ未設定のうちに誤って割り込みが発火するのを防ぐ既存の安全策）。
 *
 * IMRA bit4 がマスク中は昇格しない。ROM のキー割り込みハンドラ
 * ($ff191e) は UDR 読み出し直後に IMRA bit4 を落とし、SR の IPL を戻してから
 * キー処理を行い、処理後に bit4 を戻す。この「処理中は割り込むな」の意思表示を
 * 守らないと、FIFO に次バイトがある間ハンドラがキー処理前に再入し、
 * バイトが LIFO 順に処理される（大文字が小文字化する・文字列が逆順になる）。
 *
 * BF=0・最短転送間隔経過の2条件は、TSR方式で割り込みベクタをフックし
 * ROM本来のハンドラへチェインするソフト（例: mint.x）がUDRを2回読む
 * パターンに対応するため。ラッチは読み出しだけでは前進せず、
 * 実際にホストの実時間相当が経過して初めて次バイトへ進む実機準拠の
 * 挙動にすることで、2回目の読み出しも1回目と同じ値を返すようにする。 */
static void mfp_usart_rx_try_load_latch(x68k_bus_t *b) {
    if (!b->mfp.usart_rx_armed || mfp_ch_pending_test(&b->mfp, MFP_CH_USART_RX) ||
        b->mfp.usart_rx_bf || mfp_usart_rx_fifo_empty(b) || b->mfp.usart_rx_load_wait > 0)
        return;
    if (!(b->mfp.imra & MFP_IMRA_USART_RX))
        return;
    uint32_t va = (uint32_t)(b->mfp.vr | 12u) * 4u;
    if (va + 3u < RAM_SIZE) {
        uint32_t handler = ((uint32_t)b->ram[va] << 24) | ((uint32_t)b->ram[va + 1] << 16) |
                           ((uint32_t)b->ram[va + 2] << 8) | (uint32_t)b->ram[va + 3];
        if (handler != 0u) {
            b->mfp.usart_rx_udr = mfp_usart_rx_fifo_pop(b);
            b->mfp.usart_rx_bf = 1;
            b->mfp.usart_rx_load_wait = mfp_usart_rx_min_interval_cycles(b);
            b->mfp.ipra |= 0x10u;
            mfp_ch_pending_set(&b->mfp, MFP_CH_USART_RX);
        }
    }
}

/* IERA/IMRA bit4 が揃った初回に armed 化し、キーボード電源投入バイトを FIFO へ積む
 * （実機のキーボードがリセット・電源投入・再接続時に送る $FF = LED 状態の
 * 設定要求コード。X68000 サービスマニュアル第6章「キーボードの LED 点灯
 * 制御」および XEiJ の実装に拠る）。
 * 以前はここで 0x69 を積んでいたが、0x69 は F7 の make コードであり、
 * 対応する break (0xE9) が来ないため、ゲスト側の _BITSNS で F7 が
 * 押下状態のまま固着する不具合の原因になっていた。 */
static void mfp_usart_rx_arm(x68k_bus_t *b) {
    b->mfp.usart_rx_armed = 1;
    x68k_bus_mfp_usart_rx_push(b, 0xFFu);
}

void x68k_bus_mfp_usart_rx_push(x68k_bus_t *b, uint8_t byte) {
    int next = (b->mfp.usart_rx_tail + 1) % MFP_USART_RX_FIFO_SIZE;
    if (next == b->mfp.usart_rx_head) {
        b->mfp.usart_rx_dropped++; /* FIFO 満杯: 破棄（XEiJ 準拠）。件数のみ記録 */
        return;
    }
    b->mfp.usart_rx_fifo[b->mfp.usart_rx_tail] = byte;
    b->mfp.usart_rx_tail = next;
    mfp_usart_rx_try_load_latch(b);
}

int x68k_bus_mfp_usart_rx_pending(const x68k_bus_t *b) {
    return b && (b->mfp.usart_rx_bf || b->mfp.usart_rx_head != b->mfp.usart_rx_tail);
}

uint32_t x68k_bus_mfp_usart_rx_dropped(const x68k_bus_t *b) {
    return b ? b->mfp.usart_rx_dropped : 0;
}

/* MCP PASTE 前のフラッシュ用（paste_text の残留文字クリア）。UDRラッチ・BF・
 * pendingも合わせてリセットしないと、クリア後に残ったラッチ値やpendingが
 * 次の読み出しに漏れ出す。 */
void x68k_bus_mfp_usart_rx_clear(x68k_bus_t *b) {
    b->mfp.usart_rx_head = 0;
    b->mfp.usart_rx_tail = 0;
    b->mfp.usart_rx_udr = 0;
    b->mfp.usart_rx_bf = 0;
    b->mfp.usart_rx_load_wait = 0;
    b->mfp.ipra &= ~0x10u;
    mfp_ch_pending_clear(&b->mfp, MFP_CH_USART_RX);
}

/* RESET命令相当のMFP初期化（実験的追加）。実機のMC68901は
 * RESET信号で全レジスタ・タイマ・pending状態が電源投入相当のゼロへ戻る。
 * soft_eoi_enabled/gpip_raster_enabledはCLI起動オプション由来のホスト側設定
 * であって実レジスタではないため保持する。gpip_fallback_phaseもGPIPピンの
 * 外部信号相当（内部レジスタのリセット対象外）として保持する。 */
void mfp_reset(x68k_bus_t *b) {
    mfp_t *m = &b->mfp;
    m->vr = 0;
    m->vr_s = 0;
    m->iera = 0;
    m->ierb = 0;
    m->ipra = 0;
    m->iprb = 0;
    m->isra = 0;
    m->isrb = 0;
    m->imra = 0;
    m->imrb = 0;
    m->tcdcr = 0;
    m->tcdr = 0;
    m->tddr = 0;
    m->tc = 0;
    m->tc_frac = 0;
    m->td = 0;
    m->td_frac = 0;
    m->tbcr = 0;
    m->tbdr = 0;
    m->tb = 0;
    m->tb_frac = 0;
    m->aer = 0;
    m->tacr = 0;
    m->tadr = 0;
    m->ta = 0;
    m->ch_pending = 0;
    m->usart_rx_armed = 0;
    memset(m->usart_rx_fifo, 0, sizeof(m->usart_rx_fifo));
    m->usart_rx_head = 0;
    m->usart_rx_tail = 0;
    m->usart_rx_udr = 0;
    m->usart_rx_bf = 0;
    m->usart_rx_load_wait = 0;
    m->synced_cycles = 0;
    m->chunk_active = 0;
}

static void mfp_sync_to_now(x68k_bus_t *b);

uint8_t mfp_reg_read8(x68k_bus_t *b, uint32_t addr) {
    mfp_sync_to_now(b);
    switch (addr) {
    case 0xE88007:
        return b->mfp.iera;
    case 0xE88009:
        return b->mfp.ierb;
    case 0xE8800Bu:
        return b->mfp.ipra;
    case 0xE8800Du:
        return b->mfp.iprb;
    case 0xE8800Fu:
        return b->mfp.isra;
    case 0xE88011u:
        return b->mfp.isrb;
    case 0xE88013u:
        return b->mfp.imra;
    case 0xE88015u:
        return b->mfp.imrb;
    /* 未使用bit2-0は0が読める（『Inside X68000』）。XEiJ(L971)はSも常に0を返すが、
     * 本実装は書かれた値を返す（実機準拠）。 */
    case 0xE88017u:
        return (uint8_t)(b->mfp.vr | (b->mfp.vr_s ? 0x08u : 0x00u));
    case 0xE8801Du:
        return b->mfp.tcdcr;
    case 0xE88023u:
        return b->mfp.tc; /* 現在カウンタ値を返す */
    case 0xE88025u:
        return b->mfp.td; /* 現在カウンタ値を返す（TCDRと同型パターン） */
    case 0xE88003u:
        return b->mfp.aer;
    case 0xE88019u:
        return b->mfp.tacr;
    case 0xE8801Fu:
        return b->mfp.ta; /* 現在カウンタ値を返す（TCDRと同型パターン） */
    case 0xE8801Bu:
        return (uint8_t)(b->mfp.tbcr &
                         0x0Fu); /* RST/TBO/未使用は0で読める（『Inside X68000』） */
    case 0xE88021u:
        return b->mfp.tb; /* 現在カウンタ値を返す（TCDR/TDDRと同型パターン） */
    /* GPIP: bit0=RTC ALARM、bit2=前面電源スイッチ、他は未接続時HIGH。
     * bit7 = H-SYNC: 水平表示期間 HIGH、水平同期パルス期間 LOW（『Inside X68000』、
     * XEiJ MC68901.java L70）。bit4 = V-DISP: 垂直表示期間 HIGH、垂直
     * 帰線期間 LOW（同、L68）。ROM のラスタコピーループ
     * （H-SYNC待ち）・パレット退避復帰やスプライトAPI（V-DISP待ち）が毎反復で
     * これらのビットの遷移を待つ。実機では1表示期間に多数のラスタを一気に
     * 転送する（待ちは僅か）。ix68k では memcpy が即時なので、
     * 読み出しごとに位相を進めて待ちループが必ず数回で抜けるようにする
     * （大半 HIGH、周期的に LOW）。
     * ★ bit6(CIRQ)と異なり、常時HIGH/常時LOWへのフォールバックは採れない
     * （IPLROMの複数箇所が bit4==0 待ち・bit4==1 待ちの両方を行っており、
     * 固定値だと必ずどちらかが永久ループする）。 */
    case 0xE88001u: {
        uint8_t gpip = 0xFFu;
        if (!b->mfp.gpip0_level)
            gpip &= ~0x01u;
        if (!b->mfp.gpip2_level)
            gpip &= ~0x04u;
        int use_raster = b->mfp.gpip_raster_enabled && b->crtc.raster.valid;
        int h_total = 0, h_sync = 0, h_front = 0;
        int use_raster_h = use_raster && b->crtc.raster.cycles_per_frame > 0 &&
                           b->crtc.raster.lines_per_frame > 0 &&
                           mfp_raster_h_params(b, &h_total, &h_sync, &h_front);
        /* bit4 = V-DISP: raster.valid=1ならCRTCラスターモデル由来。bit7 = H-SYNC:
         * 加えてR00-R03が妥当ならラスター由来。どちらもラスター由来のときは
         * フォールバック位相を進めない（読み出し副作用ゼロ）。
         * ★常時HIGH/常時LOWへのフォールバックは採れない（IPLROMの複数箇所が
         * bit4==0待ち・bit4==1待ちの両方を行っており、固定値だと必ずどちらかが
         * 永久ループする）。 */
        if (use_raster) {
            if (!mfp_raster_vdisp_high(b))
                gpip &= ~0x10u;
        }
        if (use_raster_h) {
            if (!mfp_raster_hsync_high(b, h_total, h_sync, h_front))
                gpip &= ~0x80u;
        }
        if (!use_raster || !use_raster_h) {
            b->mfp.gpip_fallback_phase = (b->mfp.gpip_fallback_phase + 1) & 0x0F;
            if (!use_raster && b->mfp.gpip_fallback_phase == 0)
                gpip &= ~0x10u; /* bit4: 16回に1回LOW */
            if (!use_raster_h && (b->mfp.gpip_fallback_phase & 0x01u))
                gpip &= ~0x80u; /* bit7: 2回に1回LOW */
        }
        /* bit3 = FMIRQ: OPM(YM2151)のFMIRQ生線を反映（アクティブLOW: 要求中=0）。
         * 割り込み経路(x68k_bus_mfp_opmirq)と同じb->opm.fmirq_lineを単一の真実源に
         * することで、割り込みとポーリングで見える状態が食い違わないようにする。 */
        if (b->opm.fmirq_line)
            gpip &= ~0x08u;
        /* bit6 = CIRQ: CRTCのラスタ割り込み要求線を反映（アクティブLOW: 要求中=0）。
         * 実機は「現在の走査線 == R09」の期間だけLOWになる1ラスタ幅のパルス
         * （『Inside X68000』、XEiJ CRTC.java L1842）。IPRA/pendingは参照しない:
         * ポーリングするソフトはIERA/IMRA bit6を落としているため、pendingを見ても
         * LOWにならず目的を達しない。
         * raster.valid=0（起動直後・モード切替中）は従来どおり常時HIGHにフォールバック。 */
        if (b->crtc.raster.valid && b->crtc.raster.line == mfp_raster_compare(b))
            gpip &= ~0x40u;
        return gpip;
    }
    case 0xE8802Bu: /* RSR: bit7=BF=受信バッファフル。UDRラッチ(BF)由来で返す
                     * （FIFOはまだ「線上」のバイト列でありUDRの状態とは別）。 */
        return b->mfp.usart_rx_bf ? 0x88u : 0x08u;
    case 0xE8802Du:
        return 0xFFu; /* TSR: bit7=BE=1 → 送信 ready */
    case 0xE8802Fu:   /* UDR: 受信データレジスタ（ラッチ）。
                       * 実機同様、読み出しではラッチの中身を消費/前進させない
                       * （次バイトが実際に届くまでは何度読んでも同じ値を返す）。
                       * TSR方式で割り込みベクタをフックしROM本来のハンドラへ
                       * チェインするソフト（例: mint.x）がUDRを2回読むパターンに
                       * 対応するため。BFのクリアと次バイトへの
                       * 昇格タイミングは分離し、昇格は mfp_usart_rx_try_load_latch
                       * （IMRA bit4再有効化時・mfp_tick時）にのみ委ねる。 */
        b->mfp.usart_rx_bf = 0;
        b->mfp.ipra &= ~0x10u;
        return b->mfp.usart_rx_udr;
    default:
        return 0x00u;
    }
}

/* IMR-A/IPR-A group（USART_RXを除く4ch）を imra & ipra で個別再評価する。
 * IMRA・IPRAいずれの書き込み後も呼ぶことで、マスク変更・pendingクリアの
 * どちら経由でも ch_pending を一貫させる。USART_RXはFIFO駆動の
 * 別経路（mfp_usart_rx_try_load_latch）で管理するためここには含めない。 */
static void mfp_reeval_group_a_pending(x68k_bus_t *b) {
    if (b->mfp.imra & b->mfp.ipra & MFP_IERA_TIMERA)
        mfp_ch_pending_set(&b->mfp, MFP_CH_TIMER_A);
    else
        mfp_ch_pending_clear(&b->mfp, MFP_CH_TIMER_A);
    if (b->mfp.imra & b->mfp.ipra & MFP_IERA_RASTER)
        mfp_ch_pending_set(&b->mfp, MFP_CH_RASTER);
    else
        mfp_ch_pending_clear(&b->mfp, MFP_CH_RASTER);
    if (b->mfp.imra & b->mfp.ipra & MFP_IERA_HSYNC)
        mfp_ch_pending_set(&b->mfp, MFP_CH_HSYNC);
    else
        mfp_ch_pending_clear(&b->mfp, MFP_CH_HSYNC);
    if (b->mfp.imra & b->mfp.ipra & MFP_IERA_TIMERB)
        mfp_ch_pending_set(&b->mfp, MFP_CH_TIMER_B);
    else
        mfp_ch_pending_clear(&b->mfp, MFP_CH_TIMER_B);
}

/* IMR-B/IPR-B group（4ch）を imrb & iprb で個別再評価する。用途は
 * mfp_reeval_group_a_pending と同じ。 */
static void mfp_reeval_group_b_pending(x68k_bus_t *b) {
    if (b->mfp.imrb & b->mfp.iprb & MFP_IERB_ALARM)
        mfp_ch_pending_set(&b->mfp, MFP_CH_ALARM);
    else
        mfp_ch_pending_clear(&b->mfp, MFP_CH_ALARM);
    if (b->mfp.imrb & b->mfp.iprb & MFP_IERB_POWER_SWITCH)
        mfp_ch_pending_set(&b->mfp, MFP_CH_POWER_SWITCH);
    else
        mfp_ch_pending_clear(&b->mfp, MFP_CH_POWER_SWITCH);
    if (b->mfp.imrb & b->mfp.iprb & 0x20u) /* Timer C */
        mfp_ch_pending_set(&b->mfp, MFP_CH_TIMER_C);
    else
        mfp_ch_pending_clear(&b->mfp, MFP_CH_TIMER_C);
    if (b->mfp.imrb & b->mfp.iprb & 0x08u) /* FMIRQ */
        mfp_ch_pending_set(&b->mfp, MFP_CH_FMIRQ);
    else
        mfp_ch_pending_clear(&b->mfp, MFP_CH_FMIRQ);
    if (b->mfp.imrb & b->mfp.iprb & MFP_IERB_TIMERD)
        mfp_ch_pending_set(&b->mfp, MFP_CH_TIMER_D);
    else
        mfp_ch_pending_clear(&b->mfp, MFP_CH_TIMER_D);
    if (b->mfp.imrb & b->mfp.iprb & MFP_IERB_GPIP4)
        mfp_ch_pending_set(&b->mfp, MFP_CH_GPIP4);
    else
        mfp_ch_pending_clear(&b->mfp, MFP_CH_GPIP4);
}

/* UDR書き込み（USART TX）= ホスト→キーボードのコマンドバイト。
 * 送信時間・TSR BEの遷移は再現せず、受けた瞬間にデコードして保持する。
 * 不変条件: usart_rx_* / ipra / ch_pending（RX側）には一切触れない。
 * UDRはRX/TXで同一アドレスだが実機でもレジスタは別なので、RXラッチは変えない。 */
static void mfp_usart_tx_write(x68k_bus_t *b, uint8_t val) {
    kbd_state_t *k = &b->kbd;
    b->mfp.usart_tx_count++;
    if (val >= KBD_CMD_LED_MIN)
        k->led = (uint8_t)(~val & KBD_LED_MASK);
    else if (val >= KBD_CMD_RATE_MIN && val <= KBD_CMD_RATE_MAX)
        k->repeat_rate = val & KBD_REPEAT_VALUE_MASK;
    else if (val >= KBD_CMD_DELAY_MIN && val <= KBD_CMD_DELAY_MAX)
        k->repeat_delay = val & KBD_REPEAT_VALUE_MASK;
    else if (val >= KBD_CMD_BRIGHT_MIN && val <= KBD_CMD_BRIGHT_MAX)
        k->led_bright = val & KBD_BRIGHT_MASK;
}

uint32_t x68k_bus_mfp_usart_tx_count(const x68k_bus_t *b) {
    return b ? b->mfp.usart_tx_count : 0;
}

const kbd_state_t *x68k_bus_kbd_state(const x68k_bus_t *b) {
    return b ? &b->kbd : NULL;
}

void mfp_reg_write8(x68k_bus_t *b, uint32_t addr, uint8_t val) {
    mfp_sync_to_now(b);
    switch (addr) {
    case 0xE8802Fu:
        mfp_usart_tx_write(b, val);
        break;
    case 0xE88007u:
        b->mfp.iera = val;
        /* IER-A bit4 = RCV BF enable。IMRA bit4 も設定済みで未 armed なら armed 化 */
        if (!b->mfp.usart_rx_armed && (val & 0x10u) && (b->mfp.imra & 0x10u))
            mfp_usart_rx_arm(b);
        break;
    case 0xE88009u:
        b->mfp.ierb = val;
        break;
    /* IPRA/IPRB も ISR と同様、「0」を書いたビットだけをクリアする。 */
    case 0xE8800Bu:
        b->mfp.ipra &= val;
        mfp_reeval_group_a_pending(b);
        break;
    case 0xE8800Du:
        b->mfp.iprb &= val;
        mfp_reeval_group_b_pending(b);
        break;
    /* MC68901のISRは「"0"を書いたビットだけがクリアされ、"1"を書いたビットは不変」
     * というAND意味論（『Inside X68000』、XEiJ MC68901.java L1184のコメント）。
     * ソフトウェアからセットすることはできない（セットするのはIACKのみ）。
     *   bclr.b #n,($00E8800F) → read-modify-writeで「bit nだけ0、他は1」を書く
     *     = bit nのみクリア
     *   clr.b  ($00E8800F)    → 0を書く = 全クリア（実機同様）
     *   move.b #$FF,($00E8800F) → 何も起きない */
    case 0xE8800Fu:
        b->mfp.isra &= val;
        break;
    case 0xE88011u:
        b->mfp.isrb &= val;
        break;
    case 0xE88013u:
        b->mfp.imra = val;
        /* IMR-A bit4 解除。IERA bit4 も設定済みで未 armed なら armed 化 */
        if (!b->mfp.usart_rx_armed && (val & 0x10u) && (b->mfp.iera & 0x10u))
            mfp_usart_rx_arm(b);
        /* ROM ハンドラ($ff1952)がキー処理を終えて bit4 を戻した時点で、
         * FIFO に残っている次バイトをUDRラッチへ昇格させる
         * （最短転送間隔が経過済みなら）。 */
        mfp_usart_rx_try_load_latch(b);
        /* IMR-A bit5/6/7/0 = Timer A/ラスタ一致/H-SYNC/Timer B。
         * マスク中に条件成立した分をアンマスク時にpendingへ昇格させないと、
         * IPRAが立ったままpendingが上がらず該当チャンネルの割り込みが
         * 永久に配送されなくなる（IMR-BのFMIRQで実測・特定）。
         * クリアだけでなくセットも行う。 */
        mfp_reeval_group_a_pending(b);
        break;
    case 0xE88015u:
        b->mfp.imrb = val;
        /* IMR 更新時：ペンディング条件をソースごとに独立して再評価する。
         * バイト全体のANDで判定すると、複数ソースが同時にIPRBビットを持つ
         * 場合に、一方のIMRBビットをマスクしても他方のビットがまだ一致
         * していれば pending がクリアされない取り違えが起きる。
         * クリアだけでなくセットも行う: マスク中に条件成立した分をアンマスク時
         * pendingへ昇格させないと、IPRBが立ったままpendingが上がらず該当
         * チャンネルの割り込みが永久に配送されなくなる。ZMUSICがFMIRQを
         * 「マスク→処理→アンマスク」で使う際にOPM Timer Aと競合し、
         * BGMが1音で止まる不具合として発現した。
         * Timer C(bit5)/FMIRQ(bit3)/Timer D/GPIP4を個別にチェックする。 */
        mfp_reeval_group_b_pending(b);
        break;
    case 0xE88017u:
        b->mfp.vr = val & 0xF0u; /* V7-V4。ベクタ合成側は従来どおり mfp_vr|ch */
        /* bit3 = S: 1=ソフトウェアEOI（ISR有効）、0=自動EOI（『Inside X68000』）。
         * bit2-0は未使用。
         * -mfp-soft-eoi off（既定）のときはXEiJ(MC68901.java L1278「ビット3は0に固定」)
         * と同じく常に自動EOIへ倒す（逃げ道）。 */
        b->mfp.vr_s = (b->mfp.soft_eoi_enabled && (val & 0x08u)) ? 1u : 0u;
        /* S を 1→0 に落としたときは ISR を捨てる。実機は「ISRの各ビットは意味を持たなく
         * なります」としか書いていないが、残留ビットをそのまま残すと
         * 「自動EOIに戻したのに古いISRビットが残って全チャンネルが止まる」という
         * 復帰不能状態を作り得るため、明示的にクリアする。 */
        if (!b->mfp.vr_s) {
            b->mfp.isra = 0u;
            b->mfp.isrb = 0u;
        }
        break;
    case 0xE8801Du:
        b->mfp.tcdcr = val;
        /* XEiJ: 非ゼロモード書き込みは常に初期値からリスタート（TC/TD双方）。
         * IOCS _TIMERDST(0x6B)はTCDCR書込のたびbits[6:4]を0b111へ固定するため、
         * 呼ぶたびTimer Cも初期値へリスタートされる。これはXEiJ実装（MC68901.java
         * MFP_TCDCR書込ハンドラ）にも同じ挙動があり実機の仕様として扱う。 */
        if ((val >> 4) & 7u) {
            b->mfp.tc = b->mfp.tcdr;
            b->mfp.tc_frac = 0;
        }
        if (val & 7u) {
            b->mfp.td = b->mfp.tddr;
            b->mfp.td_frac = 0;
        }
        break;
    case 0xE88023u:
        b->mfp.tcdr = val;
        /* XEiJ: タイマ停止中ならカウンタ値も同時に更新する */
        if (!((b->mfp.tcdcr >> 4) & 7u))
            b->mfp.tc = val;
        break;
    case 0xE88025u:
        b->mfp.tddr = val;
        /* XEiJ: タイマ停止中ならカウンタ値も同時に更新する（TCDRと同型パターン） */
        if (!(b->mfp.tcdcr & 7u))
            b->mfp.td = val;
        break;
    case 0xE88003u:
        /* AER bit4 = TAI(V-DISP)入力のエッジ選択。保持のみ: x68k_bus_mfp_vdisp_tick は
         * フレーム境界で無条件に1回カウントするため、エッジ選択そのものは
         * カウント判定に影響しない（他のGPIP系タイマ近似と同じ簡略化）。 */
        b->mfp.aer = val;
        break;
    case 0xE88019u:
        /* Timer A はイベントカウントモード(bit3)のみ対応（対応するIOCSコールが
         * 使うのはこれだけのため）。ディレイモード(bit3=0かつbits[2:0]≠0)は
         * CPUサイクル基準のカウントダウンを実装しておらず、値の保持のみ行う
         * （Timer B同様スコープ外）。
         * XEiJ準拠: モード遷移時にカウンタを強制リロードしない
         * （TADR書込み側の「停止中なら即時反映」ルールに委ねる）。 */
        b->mfp.tacr = val;
        break;
    case 0xE8801Fu:
        b->mfp.tadr = val;
        /* XEiJ: イベントカウントモードで動作中でなければカウンタ値も即時更新する
         * （TCDR/TDDRと同型パターン。ディレイモードは駆動していないため
         * 「非イベントカウント」を停止中とみなす） */
        if (!(b->mfp.tacr & MFP_TACR_EVENTCOUNT))
            b->mfp.ta = val;
        break;
    case 0xE8801Bu:
        b->mfp.tbcr = val;
        /* XEiJ(MFP_TBCR書込)・既存TCDCR書込と同型: ディレイモードへ入る書き込みは
         * 常に初期値からリスタートする。 */
        if (mfp_tb_delay_mode(val)) {
            b->mfp.tb = b->mfp.tbdr;
            b->mfp.tb_frac = 0;
        }
        break;
    case 0xE88021u:
        b->mfp.tbdr = val;
        /* XEiJ: タイマ停止中ならカウンタ値も同時に更新する（TCDR/TDDRと同型パターン） */
        if (!mfp_tb_delay_mode(b->mfp.tbcr))
            b->mfp.tb = val;
        break;
    default:
        break;
    }
}

static const int mfp_tc_prescale[8] = {0, 4, 10, 16, 50, 64, 100, 200};

void x68k_bus_mfp_set_tcdcr(x68k_bus_t *b, uint8_t val) {
    b->mfp.tcdcr = val;
    if ((val >> 4) & 7u) {
        b->mfp.tc = b->mfp.tcdr;
        b->mfp.tc_frac = 0;
    }
}

/*
 * MFP の USART RX 昇格待ち・Timer B/C/D を cpu_cycles 分進める。
 * 割り込み条件が成立したら mfp_ch_pending の該当ビット（Timer C はch5）を立てる。
 * main ループの実行チャンクごと（チャンク内同期時は mfp_sync_to_now() 経由でも）呼ぶ。
 */
void x68k_bus_mfp_tick(x68k_bus_t *b, int cpu_cycles) {
    /*
     * USART RX: 最短転送間隔のカウントダウンを進め、armed 状態でベクタが BIOS
     * によって設定済みになったらFIFO先頭バイトをUDRラッチへ昇格しpendingに
     * 上げる。RAM[VR|12 * 4] が非ゼロになった時点で発火する。
     */
    if (b->mfp.usart_rx_load_wait > 0) {
        b->mfp.usart_rx_load_wait -= cpu_cycles;
        if (b->mfp.usart_rx_load_wait < 0)
            b->mfp.usart_rx_load_wait = 0;
    }
    mfp_usart_rx_try_load_latch(b);

    /*
     * MFP クロック = 4MHz 固定。CPU クロックは b->cpu_hz（可変）。
     * 1 ダウンカウント = prescale 個の MFP クロック = prescale * (cpu_hz/MFP_HZ) 個の CPU
     * サイクル。整数演算: frac を ratio_num 倍してから prescale * ratio_den で割る
     * （ratio_num/ratio_den は MFP_HZ/cpu_hz の既約分数。既定10MHzでは 2/5 で従来と同じ）。
     */
    uint32_t g = mfp_gcd_u32(MFP_HZ, b->cpu_hz);
    int ratio_num = (int)(MFP_HZ / g);
    int ratio_den = (int)(b->cpu_hz / g);

    int tc_mode = (b->mfp.tcdcr >> 4) & 7;
    if (tc_mode) {
        int prescale = mfp_tc_prescale[tc_mode];
        b->mfp.tc_frac += cpu_cycles * ratio_num;
        int ticks = b->mfp.tc_frac / (prescale * ratio_den);
        b->mfp.tc_frac %= (prescale * ratio_den);

        while (ticks-- > 0) {
            b->mfp.tc--;
            if (b->mfp.tc == 0) {
                b->mfp.tc = b->mfp.tcdr; /* ロールオーバー: 初期値を再ロード */
                /* IER-B bit5 = Timer C 割り込みイネーブル */
                if (b->mfp.ierb & 0x20u) {
                    b->mfp.iprb |= 0x20u; /* IPR-B bit5 セット */
                    /* IMR-B bit5 = Timer C 割り込みマスク解除 */
                    if (b->mfp.imrb & 0x20u) {
                        /*
                         * ベクタ番号 = VR[7:4] | 5
                         * Timer C は XEiJ MFP_TIMER_C_LEVEL=5 → オフセット 5
                         */
                        mfp_ch_pending_set(&b->mfp, MFP_CH_TIMER_C);
                    }
                }
            }
        }
    }

    /* Timer D（ch4, IOCS 0x6B _TIMERDST）。
     * TCDCR bits[2:0]がTimer D制御（『Inside X68000』 6-2-2節）。Timer Cと独立に
     * 動くため、TC停止中でも処理する（早期returnしない）。 */
    int td_mode = b->mfp.tcdcr & 7;
    if (td_mode) {
        int prescale = mfp_tc_prescale[td_mode];
        b->mfp.td_frac += cpu_cycles * ratio_num;
        int ticks = b->mfp.td_frac / (prescale * ratio_den);
        b->mfp.td_frac %= (prescale * ratio_den);

        while (ticks-- > 0) {
            b->mfp.td--;
            if (b->mfp.td == 0) {
                b->mfp.td = b->mfp.tddr;
                if (b->mfp.ierb & MFP_IERB_TIMERD) {
                    b->mfp.iprb |= MFP_IERB_TIMERD;
                    if (b->mfp.imrb & MFP_IERB_TIMERD)
                        mfp_ch_pending_set(&b->mfp, MFP_CH_TIMER_D);
                }
            }
        }
    }

    /* Timer B（ch8、キーボードUSARTの伝送クロック源。IPLROM設定値TBCR=$01/TBDR=$0Dで
     * 4MHz/4/13 = 76.9kHz、TBO反転で38.4kHz = 2400bps×16。『Inside X68000』 6-2-1節/図13）。
     * Timer C/Dと独立に動くため早期returnしない。 */
    int tb_mode = mfp_tb_delay_mode(b->mfp.tbcr);
    if (tb_mode) {
        int prescale = mfp_tc_prescale[tb_mode];
        b->mfp.tb_frac += cpu_cycles * ratio_num;
        int ticks = b->mfp.tb_frac / (prescale * ratio_den);
        b->mfp.tb_frac %= (prescale * ratio_den);
        while (ticks-- > 0) {
            b->mfp.tb--;
            if (b->mfp.tb == 0) {
                b->mfp.tb = b->mfp.tbdr; /* ロールオーバー: 初期値を再ロード */
                if (b->mfp.iera & MFP_IERA_TIMERB) {
                    b->mfp.ipra |= MFP_IERA_TIMERB;
                    if (b->mfp.imra & MFP_IERA_TIMERB)
                        mfp_ch_pending_set(&b->mfp, MFP_CH_TIMER_B);
                }
            }
        }
    }
}

static void mfp_sync_to_now(x68k_bus_t *b) {
    if (!b->mfp.cycle_source || !b->mfp.chunk_active)
        return;
    int delta = b->mfp.cycle_source(b->mfp.cycle_ctx) - b->mfp.synced_cycles;
    if (delta <= 0)
        return;
    b->mfp.synced_cycles += delta;
    x68k_bus_mfp_tick(b, delta);
}

void x68k_bus_mfp_set_cycle_source(x68k_bus_t *b, int (*fn)(void *ctx), void *ctx) {
    b->mfp.cycle_source = fn;
    b->mfp.cycle_ctx = ctx;
    b->mfp.synced_cycles = 0;
}

void x68k_bus_mfp_chunk_begin(x68k_bus_t *b) {
    b->mfp.synced_cycles = 0;
    b->mfp.chunk_active = 1;
}

void x68k_bus_mfp_chunk_tick(x68k_bus_t *b, int consumed) {
    int rest = consumed - b->mfp.synced_cycles;
    b->mfp.synced_cycles = 0;
    b->mfp.chunk_active = 0;
    if (rest > 0)
        x68k_bus_mfp_tick(b, rest);
}

/* 1タイマの満了までのCPUサイクル数。tick()と同じ frac 加算式
 * (frac += cycles*num; ticks = frac/(prescale*den)) を逆算する。
 * カウンタ0は次の減算で255へ回り256カウント必要（tick()の uint8 減算と同じ）。 */
static int mfp_timer_cycles_to_expiry(int mode, uint8_t counter, int frac, int num, int den) {
    int64_t need_ticks = counter ? counter : 256;
    int64_t need_frac = need_ticks * mfp_tc_prescale[mode] * den - frac;
    int64_t cycles = (need_frac + num - 1) / num;
    if (cycles < 1)
        cycles = 1;
    return cycles > INT_MAX ? INT_MAX : (int)cycles;
}

int x68k_bus_mfp_cycles_to_next_event(const x68k_bus_t *b) {
    uint32_t g = mfp_gcd_u32(MFP_HZ, b->cpu_hz);
    int num = (int)(MFP_HZ / g);
    int den = (int)(b->cpu_hz / g);
    int best = INT_MAX;
    int c;

    int tc_mode = (b->mfp.tcdcr >> 4) & 7;
    if (tc_mode) {
        c = mfp_timer_cycles_to_expiry(tc_mode, b->mfp.tc, b->mfp.tc_frac, num, den);
        if (c < best)
            best = c;
    }
    int td_mode = b->mfp.tcdcr & 7;
    if (td_mode) {
        c = mfp_timer_cycles_to_expiry(td_mode, b->mfp.td, b->mfp.td_frac, num, den);
        if (c < best)
            best = c;
    }
    int tb_mode = mfp_tb_delay_mode(b->mfp.tbcr);
    if (tb_mode) {
        c = mfp_timer_cycles_to_expiry(tb_mode, b->mfp.tb, b->mfp.tb_frac, num, den);
        if (c < best)
            best = c;
    }
    return best;
}

/*
 * CRTC ラスターモデルから絶対ラスタ番号を受け取り、
 * ラスタ一致(ch14)/H-SYNC(ch15)割り込みを判定する。
 */
void mfp_raster_line_event(x68k_bus_t *b, int line) {
    if (b->mfp.iera & MFP_IERA_HSYNC) {
        b->mfp.ipra |= MFP_IERA_HSYNC;
        if (b->mfp.imra & MFP_IERA_HSYNC)
            mfp_ch_pending_set(&b->mfp, MFP_CH_HSYNC);
    }

    if (line == mfp_raster_compare(b) && (b->mfp.iera & MFP_IERA_RASTER)) {
        b->mfp.ipra |= MFP_IERA_RASTER;
        if (b->mfp.imra & MFP_IERA_RASTER)
            mfp_ch_pending_set(&b->mfp, MFP_CH_RASTER);
    }
}

/* IACKでチャンネル ch のベクタを引き渡すときのMFP内部状態更新。
 * 実機（『Inside X68000』）: ベクタを渡した時点でIPRの該当ビットが"0"に復帰し、
 * 同時にISRの該当ビットが"1"になる。自動EOIモード(S=0)ではISRは即座に"0"へ
 * 戻るので、そもそもセットしない。
 * ビット位置はIERA/IPRA/ISRA/IMRAがch15..ch8、IERB/...がch7..ch0。 */
void mfp_iack_accept(x68k_bus_t *b, int ch) {
    uint8_t mask = (uint8_t)(1u << (ch & 7));
    if (ch >= 8) {
        b->mfp.ipra &= (uint8_t)~mask;
        if (b->mfp.vr_s)
            b->mfp.isra |= mask;
    } else {
        b->mfp.iprb &= (uint8_t)~mask;
        if (b->mfp.vr_s)
            b->mfp.isrb |= mask;
    }
}

/* 現在in-serviceのチャンネルのうち最上位の番号を返す（-1 = なし）。
 * 実機仕様（『Inside X68000』「ISRA/ISRBが"0"でIPRA/IPRBが"1"になると、MFPは
 * 該当するビットの割り込み要求を行います」＋固定優先度ch15>…>ch0）を
 * 「ch nが要求できるのはn > (in-serviceの最上位)のときだけ」という単一の規則に
 * まとめたもの。同一チャンネルの再入禁止はn > nが偽になることで自動的に成立する。
 *
 * 下位チャンネルまでブロックする点はMFP章に明記が無いが、『Inside X68000』の
 * SCC節の同型記述に従い採用する。
 *
 * S=0（自動EOI、IPLROM既定のVR=$40）では常に-1を返す。したがって本関数を
 * 経由するすべての判定は「従来どおり全チャンネル許可」に退化する。
 * x68k_bus_mfp_pending_irq()とbus_iack()（x68k_bus.c）の両方から呼ぶ。
 * 両者のゲート条件が一致していなければならないため、staticにせず共有する。 */
int mfp_isr_top(const x68k_bus_t *b) {
    if (!b->mfp.vr_s)
        return -1;
    unsigned s = ((unsigned)b->mfp.isra << 8) | (unsigned)b->mfp.isrb;
    if (s == 0u)
        return -1;
    int top = 15;
    while (!(s & (1u << top)))
        top--;
    return top;
}

/* IRQ6 (MFP) が保留中かどうかを返す。
 * mfp_isr_top()が返す最上位in-serviceチャンネルより下位のビットをすべて
 * ブロック対象としてマスクする。USART RXだけはmfp_usart_rx_requesting()
 * （IMRA bit4のライブ再チェック付き）を使う既存の非対称をそのまま維持する。 */
int x68k_bus_mfp_pending_irq(const x68k_bus_t *b) {
    int top = mfp_isr_top(b); /* S=0のとき-1=全許可（従来と完全に同一の式になる） */
    uint16_t block = (top < 0) ? 0u : (uint16_t)((2u << top) - 1u);
    uint16_t pending = b->mfp.ch_pending & (uint16_t)~block;
    if (!mfp_usart_rx_requesting(b))
        pending &= (uint16_t)~(1u << MFP_CH_USART_RX);
    return pending != 0u;
}

/*
 * Timer A（ch13, IOCS 0x6C _VDISPST）の V-DISP イベントカウントを1フレーム分
 * 進める。実機では TAI 入力に CRTC の V-DISP 信号が直結され、垂直帰線ごとに
 * 1カウントダウンする（『Inside X68000』 6-2-1節）。ラスターが有効なフレームでは
 * 垂直映像期間終了のラスタ到達時に crtc_raster_emit_line()（crtc.c）から呼ばれ、
 * ラスター無効時のみ main.c のフレーム末尾から呼ばれる（いずれも1フレーム1回）。
 *
 * TACR のイベントカウントモード(bit3)は IPLROM・HUMAN.SYS・XEiJ いずれにも
 * 初期化するコードが存在せず、ドライバ/アプリ側が事前に設定する運用モデルと
 * 判明している。未設定（bit3=0）なら実機同様 Timer A は動かない。
 *
 * ch6(GPIP4/V-DISP)は Timer A の TAI と同じ物理線（V-DISP信号）を使うが
 * 独立したチャンネルで、IERB bit6 が立っていれば TACR の設定に関係なく
 * 毎フレーム発火する（XSPライブラリ等がvsync待ちに使用）。
 * AER bit4 のエッジ選択は Timer A 同様に保持のみで判定には使わない
 * （既存のV-DISP近似方針: 1フレーム=1回のエッジ通過とみなす簡略化）。
 */
void x68k_bus_mfp_vdisp_tick(x68k_bus_t *b) {
    if (b->mfp.ierb & MFP_IERB_GPIP4) {
        b->mfp.iprb |= MFP_IERB_GPIP4;
        if (b->mfp.imrb & MFP_IERB_GPIP4)
            mfp_ch_pending_set(&b->mfp, MFP_CH_GPIP4);
    }

    if (!(b->mfp.tacr & MFP_TACR_EVENTCOUNT))
        return;
    b->mfp.ta--;
    if (b->mfp.ta == 0) {
        b->mfp.ta = b->mfp.tadr; /* ロールオーバー: 初期値を再ロード */
        if (b->mfp.iera & MFP_IERA_TIMERA) {
            b->mfp.ipra |= MFP_IERA_TIMERA;
            if (b->mfp.imra & MFP_IERA_TIMERA)
                mfp_ch_pending_set(&b->mfp, MFP_CH_TIMER_A);
        }
    }
}

void x68k_bus_mfp_rtc_alarm(x68k_bus_t *b, int low) {
    uint8_t next = low ? 0u : 1u;
    uint8_t prev = b->mfp.gpip0_level;
    if (next == prev)
        return;
    b->mfp.gpip0_level = next;
    /* AER bit0: 1=0→1 (rising), 0=1→0 (falling). */
    bool rising = !prev && next;
    bool selected = (b->mfp.aer & 0x01u) ? rising : !rising;
    if (selected && (b->mfp.ierb & MFP_IERB_ALARM)) {
        b->mfp.iprb |= MFP_IERB_ALARM;
        if (b->mfp.imrb & MFP_IERB_ALARM)
            mfp_ch_pending_set(&b->mfp, MFP_CH_ALARM);
    }
}

void x68k_bus_mfp_power_switch(x68k_bus_t *b, int on) {
    uint8_t next = on ? 0u : 1u;
    uint8_t prev = b->mfp.gpip2_level;
    if (next == prev)
        return;
    b->mfp.gpip2_level = next;
    /* AER bit2: 1=0→1 (rising), 0=1→0 (falling). */
    bool rising = !prev && next;
    bool selected = (b->mfp.aer & MFP_IERB_POWER_SWITCH) ? rising : !rising;
    if (selected && (b->mfp.ierb & MFP_IERB_POWER_SWITCH)) {
        b->mfp.iprb |= MFP_IERB_POWER_SWITCH;
        if (b->mfp.imrb & MFP_IERB_POWER_SWITCH)
            mfp_ch_pending_set(&b->mfp, MFP_CH_POWER_SWITCH);
    }
}

/*
 * OPM(GPIP3/FMIRQ)割り込みのMFPへの反映。opm.cがOPMのFMIRQ生線の変化を
 * 検知するたびに呼ぶ。Timer C(本ファイルのx68k_bus_mfp_tick)と同型のパターン:
 * IER-B bit3(FMIRQイネーブル)が立っていればIPR-B bit3をラッチし、
 * IMR-B bit3もアンマスクならpendingへ昇格させる。
 *
 * DDRレジスタは ix68k に実装が無いため、GPIP3の実機同等のエッジ検出は
 * 行わず、OPMの生線レベルをそのまま反映する簡略化とする（asserted=0で
 * IPR-B/pendingを即座にクリア）。AERはTimer A(TAI/V-DISP)用に追加したが、
 * 保持のみでGPIP3の判定には使わない。
 */
void x68k_bus_mfp_opmirq(x68k_bus_t *b, int asserted) {
    if (asserted) {
        if (b->mfp.ierb & 0x08u) {
            b->mfp.iprb |= 0x08u;
            if (b->mfp.imrb & 0x08u)
                mfp_ch_pending_set(&b->mfp, MFP_CH_FMIRQ);
        }
    } else {
        b->mfp.iprb &= ~0x08u;
        mfp_ch_pending_clear(&b->mfp, MFP_CH_FMIRQ);
    }
}
