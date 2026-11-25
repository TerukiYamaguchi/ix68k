#ifndef X68K_MFP_H
#define X68K_MFP_H

#include <stdint.h>
#include "bus/x68k_bus.h"

#define MFP_USART_RX_FIFO_SIZE 32 /* キーボード押下/離上バイトの循環バッファ長 */

/* ---- mfp_ch_pending 用チャンネル番号 ---- */
typedef enum {
    MFP_CH_ALARM = 0,
    MFP_CH_POWER_SWITCH = 2,
    MFP_CH_FMIRQ = 3,
    MFP_CH_TIMER_D = 4,
    MFP_CH_TIMER_C = 5,
    MFP_CH_GPIP4 = 6,
    MFP_CH_TIMER_B = 8,
    MFP_CH_USART_RX = 12,
    MFP_CH_TIMER_A = 13,
    MFP_CH_RASTER = 14,
    MFP_CH_HSYNC = 15,
} mfp_channel_t;

/* MFP (MC68901): 0xE88001-0xE8802F 奇数アドレス */
typedef struct {
    uint8_t vr;           /* $E88017: Vector Register（上位4ビット = ベースベクタ、bit3=Sは
                              vr_s へ分離して管理する） */
    uint8_t vr_s;         /* VR bit3 (S)。1=ソフトウェアEOIモード（ISR有効）、
                             0=自動EOIモード（ISR無効・IPLROM既定） */
    int soft_eoi_enabled; /* 0=Sビット書き込みを無視して常に自動EOI（XEiJ互換・
                             逃げ道）。既定0＝off（実測でS=1書き込みが一切観測
                             されなかったため既定offとした）。
                             -mfp-soft-eoi on で有効化。 */
    uint8_t iera;         /* $E88007: IER-A */
    uint8_t ierb;         /* $E88009: IER-B（bit5 = Timer C） */
    uint8_t ipra;         /* $E8800B: IPR-A */
    uint8_t iprb;         /* $E8800D: IPR-B */
    uint8_t isra;         /* $E8800F: ISR-A（In-Service 実状態。S=1のときIACKでセットされ、
                             ソフトウェアEOI書き込み(AND意味論)でクリアされる） */
    uint8_t isrb;         /* $E88011: ISR-B（同上） */
    uint8_t imra;         /* $E88013: IMR-A */
    uint8_t imrb;         /* $E88015: IMR-B（bit5 = Timer C） */
    uint8_t tcdcr;        /* $E8801D: Timer C/D Control（bits[6:4] = TC prescale, bits[2:0] = TD
                              prescale） */
    uint8_t tcdr;         /* $E88023: Timer C Data（初期値） */
    uint8_t tddr;         /* $E88025: Timer D Data（初期値） */
    uint8_t tc;           /* Timer C 現在カウンタ値 */
    int tc_frac;          /* Timer C 残余 CPU サイクル */
    uint8_t td;           /* Timer D 現在カウンタ値 */
    int td_frac;          /* Timer D 残余 CPU サイクル */
    uint8_t tbcr; /* $E8801B: Timer B Control（bit3-0=モード。bit7=RST/bit4=TBOは保持のみ） */
    uint8_t tbdr; /* $E88021: Timer B Data（初期値） */
    uint8_t tb;   /* Timer B 現在カウンタ値 */
    int tb_frac;  /* Timer B 残余 CPU サイクル */
    uint8_t aer;  /* $E88003: Active Edge Register（bit0=ALARM・bit2=POW SWのエッジ選択に使用。
                     bit4 = TAI/V-DISPエッジ選択は保持のみで判定には未使用） */
    uint8_t gpip0_level; /* RTC ALARM入力。1=通常HIGH、0=ALARM LOW。 */
    uint8_t gpip2_level; /* POW SW入力。1=OFF(HIGH)、0=ON(LOW)。 */
    uint8_t tacr;        /* $E88019: Timer A Control（bit3 = イベントカウントモード） */
    uint8_t tadr;        /* $E8801F: Timer A Data（初期値） */
    uint8_t ta;          /* Timer A 現在カウンタ値（V-DISPイベントカウント用） */
    /* 16チャンネル分のpending状態（ビット位置=チャンネル番号）。実装済み
     * チャンネルはch15(H-SYNC)/ch14(ラスタ一致)/ch13(Timer A)/ch12(USART RX)/
     * ch8(Timer B)/ch6(GPIP4/V-DISP)/ch5(Timer C)/ch4(Timer D)/ch3(FMIRQ)/
     * ch2(POW SW)/ch0(RTC ALARM)の11個のみで、
     * 他ビットは発火経路が無いため常に0（mfp_ch_pending_set/clear/testヘルパ
     * 経由でのみ操作する）。 */
    uint16_t ch_pending;
    int usart_rx_armed; /* 1 = USART RX 割り込み条件成立（IERA/IMRA bit4 設定済み） */
    /* USART RX FIFO（キーボードからの押下/離上バイト列。まだ「線上」にある
     * バイト列のモデル。$0=head==tail で空。満杯時の push は黙って破棄する。 */
    uint8_t usart_rx_fifo[MFP_USART_RX_FIFO_SIZE];
    int usart_rx_head;
    int usart_rx_tail;
    uint32_t usart_rx_dropped; /* FIFO 満杯で破棄したバイト数の累計（MCP KEY PASTE で観測） */
    /* UDR（$E8802F）ラッチ。実機のMC68901はシフトレジスタ→UDRの二重バッファで、
     * UDR読み出しはBFを落とすだけでラッチの中身は破壊しない（次バイトが実際に
     * 届くまでは何度読んでも同じ値を返す）。FIFOの先頭バイトをこのラッチへ
     * 昇格させるタイミングをusart_rx_load_waitで実転送時間相当に律速することで、
     * 「読み出しごとにFIFOを1個ポップする」実装だと壊れるTSR方式の割り込み
     * ベクタチェイン（例: mint.xがROM本来のキー割り込みハンドラへチェインする
     * 際、両者がUDRを2回読む）を正しく再現する。 */
    uint8_t usart_rx_udr;   /* ラッチ値 */
    int usart_rx_bf;        /* 1 = BF（ラッチに未消費データあり） */
    int usart_rx_load_wait; /* 次にFIFO先頭をラッチへ昇格できるまでの残CPUサイクル数 */
    /* GPIP bit7=H-SYNC・bit4=V-DISP（『Inside X68000』、XEiJ MC68901.java
     * L68/L70）が crtc.raster.valid=0（起動直後・モード切替中）のときに使う
     * フォールバック位相カウンタ。ROM のラスタコピーループ（H-SYNC待ち）・
     * パレット退避復帰等（V-DISP待ち）が毎反復でこれらのビットの遷移を待つため、
     * 読み出しごとに進めて待ちループを必ず数回で抜けさせる。旧名 vdisp_phase は
     * ビット7をV-DISPと誤認したコメントとともに付けられていた名前で、一次資料・
     * XEiJの両方で再検証し是正済み。 */
    int gpip_fallback_phase; /* 0〜15の位相 */
    int gpip_raster_enabled; /* 1(既定) = GPIP bit7/bit4をCRTCラスターモデル由来で合成する
                                （crtc.raster.valid=0のときはgpip_fallback_phaseへ自動
                                フォールバック）。0 = 常にgpip_fallback_phaseのみを使う
                                （フォールバック位相のみへの逃げ道）。-mfp-gpip-raster off
                                で無効化。 */
    /* チャンク内同期。cycle_source はCPU実行中の「現在の
     * mc68k_execute()呼出しで消費済みのサイクル数」を返す。NULLなら同期しない
     * （従来のチャンク末尾一括tick）。synced_cycles はそのうちtick済みの分。 */
    int (*cycle_source)(void *ctx);
    void *cycle_ctx;
    int synced_cycles;
    uint32_t usart_tx_count; /* UDR書き込み（USART TX）の累計バイト数。観測用 */
    int chunk_active;        /* chunk_begin〜chunk_tick
                                の間だけ1。チャンク外（ホスト側アクセス）では同期しない */
} mfp_t;

/* ---- USART TX（UDR書き込み）で受けるホスト→キーボード方向コマンドの保持状態 ----
 * キーボード本体（外部デバイス）の状態であり、MFPレジスタではないため mfp_t には
 * 置かず x68k_bus_t::kbd に持つ。RESET命令（mfp_reset）では消さない。
 * 保持値は生のコマンド値で、ms換算はしない（delay=200+100n ms、rate=30+5n^2 ms）。 */
typedef struct {
    uint8_t led; /* bit=1で点灯。bit0かな 1ローマ字 2コード入力 3CAPS 4INS 5ひらがな 6全角 */
    uint8_t led_bright;   /* LED明るさ（コマンド$54-$57の下位2bit） */
    uint8_t repeat_delay; /* リピート開始時間（コマンド$60-$6Fの下位4bit） */
    uint8_t repeat_rate;  /* リピート間隔（コマンド$70-$7Fの下位4bit） */
} kbd_state_t;

/* USART TX で送られたバイトの累計数（観測用）。 */
uint32_t x68k_bus_mfp_usart_tx_count(const x68k_bus_t *b);
/* ホスト→キーボード方向コマンドで保持している状態（読み出し専用）。 */
const kbd_state_t *x68k_bus_kbd_state(const x68k_bus_t *b);

/* ---- MFP (MC68901) レジスタ I/O（実体は mfp.c）。read8/write8 から呼ぶ ---- */
uint8_t mfp_reg_read8(x68k_bus_t *b, uint32_t addr);
void mfp_reg_write8(x68k_bus_t *b, uint32_t addr, uint8_t val);

/* ---- RESET命令相当のMFP初期化（実体は mfp.c、実験的追加）。実機のRESET命令は
 *      MC68901をリセットするが、bus_reset_peripheral()はこれまでb->mfpに一切
 *      触れておらず、Timer C等がソフトリセットをまたいで自走し続けていた
 *      （Timer Cの残留位相がIPLROM30のCPU速度較正結果の
 *      ばらつきの原因という仮説）。CLI起動オプション由来のホスト側設定
 *      （soft_eoi_enabled/gpip_raster_enabled）とGPIP位相カウンタ（実ピン相当、
 *      レジスタリセットの対象外）は保持し、それ以外のレジスタ・タイマ・
 *      pending状態を電源投入相当のゼロへ戻す。 ---- */
void mfp_reset(x68k_bus_t *b);

/* ---- IACKでチャンネル ch のベクタを引き渡すときのMFP内部状態更新（実体は
 *      mfp.c）。bus_iack()のMFP 8分岐から呼ぶ。IPRの該当ビットを"0"に戻し、
 *      S=1（ソフトウェアEOI）のときだけISRの該当ビットを"1"にする。 ---- */
void mfp_iack_accept(x68k_bus_t *b, int ch);

/* ---- 現在in-serviceのチャンネル最上位番号（-1=なし）。実体は mfp.c。
 *      x68k_bus_mfp_pending_irq()とbus_iack()の両方で同じ判定を使うため共有する。 ---- */
int mfp_isr_top(const x68k_bus_t *b);

/* ---- mfp_ch_pending のビット単位ヘルパ（複数の.cから使うため
 *      staticにせずヘッダでinline定義する）。x68k_bus_t はここではまだ
 *      不完全型のため、struct x68k_bus本体ではなく自己完結したmfp_tを直接
 *      受け取る（呼び出し側は &b->mfp を渡す）。 ---- */
static inline void mfp_ch_pending_set(mfp_t *m, int ch) {
    m->ch_pending |= (uint16_t)(1u << ch);
}
static inline void mfp_ch_pending_clear(mfp_t *m, int ch) {
    m->ch_pending &= (uint16_t)~(1u << ch);
}
static inline int mfp_ch_pending_test(const mfp_t *m, int ch) {
    return (m->ch_pending & (uint16_t)(1u << ch)) != 0;
}

/* ---- 走査線1本分のMFP割り込み判定（実体は mfp.c）。
 *      x68k_bus_mfp_tick()内の擬似走査線ループから切り出した。
 *      x68k_bus_crtc_raster_advance()(crtc.c)がラスター前進で新しい絶対ラスタ番号
 *      lineに到達するたびに呼ぶ。H-SYNC(ch15)を毎ライン、ラスタ一致(ch14)を
 *      R09一致時に判定する。 ---- */
void mfp_raster_line_event(x68k_bus_t *b, int line);

/* ---- OPM(GPIP3/FMIRQ)割り込みのMFPへの反映（実体は mfp.c）。opm.cから
 *      fmirq_line変化時に呼ぶ。Timer C(x68k_bus_mfp_tick)と同型のパターン ---- */
void x68k_bus_mfp_opmirq(x68k_bus_t *b, int asserted);

/* RP5C15 ALARM端子の変化をGPIP0へ反映する。low=1はRTC端子LOW。 */
void x68k_bus_mfp_rtc_alarm(x68k_bus_t *b, int low);

/* 前面電源スイッチ状態をGPIP2へ反映する。on=1はON(LOW)、0はOFF(HIGH)。 */
void x68k_bus_mfp_power_switch(x68k_bus_t *b, int on);

#endif /* X68K_MFP_H */
