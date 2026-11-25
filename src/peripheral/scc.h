#ifndef X68K_SCC_H
#define X68K_SCC_H

#include <stdint.h>

/* ch A(RS-232C)の受信・送信リング容量。 */
#define SCC_CH_A_RING_CAP 256

/*
 * SCC(Z8530): チャンネルAのRS-232CとチャンネルBのマウスを扱う。
 *
 * アドレス割り当て（ROM初期化テーブル `IPLROMXV.S`・XEiJ `Z8530.java` で裏取り済み。
 * マニュアルは「$E98001/$E98005」としか書かずch A/B判別不能だったため確定させた）:
 *   $E98001 ch B(マウス) コントロール/ステータス   $E98003 ch B データ(3バイト受信)
 *   $E98005 ch A(RS-232C) コントロール/ステータス  $E98007 ch A データ
 * D7-D0の下位バイトレーンのみ実配線（奇数アドレスのみ有効、MFPと同様）。
 *
 * マウスプロトコル: WR5 bit1(RTS) の 0→1 立ち上がりで
 * ステータス+X相対移動量+Y相対移動量の3バイトを送出（`『Inside X68000』
 * マウス制御節、ROM/XEiJと一致確認済み）。割り込み駆動が必須（ポーリング不可、
 * ROM ISRはRR0を見ずSCC Rx割り込みの発火のみを前提にバイトを取り込む）。
 *
 * WR0=$38(Reset Highest IUS)が次バイトの割り込み再武装に機能的必須。
 * これを読み出し直後に自動で行うとキーボード受信FIFO(mfp.c)と同型の
 * 再入バグになるため、明示的な$38コマンド受信時のみ再武装する。
 */

/* SCCからbus層のメモリを読む唯一の経路（$0ACE参照用）。ctxはx68k_bus_tを想定。 */
typedef uint32_t (*scc_mem_read32_fn)(void *ctx, uint32_t addr);

typedef struct {
    /* ch B(マウス)レジスタ */
    uint8_t wr0_regsel; /* WR0で選択中のレジスタ番号(0=WR0自身/コマンド待ち) */
    uint8_t wr1;        /* Rx割り込み許可(bit3/4) */
    uint8_t wr2;        /* 割り込みベクタベース */
    uint8_t wr5;        /* bit1=RTS */
    uint8_t wr9;        /* ベクタ修飾モード(bit0/4)・チャンネル/HWリセット(bit6-7) */
    uint8_t rts_prev;   /* RTS立ち上がりエッジ検出用 */

    uint8_t rx_data[3]; /* status, dx, dy（RTS立ち上がり時にスナップショット） */
    int rx_count;       /* 未読バイト数(0-3) */
    int rx_index;       /* 次に返すrx_data[]の位置 */

    int recv_request; /* ch B Rx割り込み要求ペンディング（IACKで消費、$38で再武装） */

    /* ホストの現在のマウス位置（ネイティブ解像度換算後、platformコールバックが
     * 都度上書き）。RTS立ち上がり時にIOCS側の現在カーソル座標($0ACE)との差分を
     * その場で計算する（XEiJ Z8530.java準拠）。基準点をこちら側で推定・保持
     * しないため、ウィンドウ境界の出入り等で基準点がズレたまま蓄積することがない。 */
    int host_x, host_y;
    scc_mem_read32_fn mem_read32;
    void *mem_read32_ctx;
    uint8_t btn_left, btn_right;

    /* ch A(RS-232C)の透過データ経路。
     * WR3/4/5(キャラクタ長・パリティ・ストップビット等)は書き込みを保持
     * するのみでタイミング動作には反映しない。ホストとの実バイト経路
     * (PTY等)はplatform層の責務のため、ここではリングバッファに溜める
     * だけで直接POSIX呼び出しはしない（main.cが毎フレーム橋渡しする）。 */
    uint8_t wr0_regsel_a;
    uint8_t wr1_a;               /* Rx割り込み許可(bit3/4) */
    uint8_t wr3_a, wr4_a, wr5_a; /* フォーマット設定(保持のみ) */
    uint8_t carrier_a;           /* DCD/CTS相当。ホスト接続時1 */

    uint8_t rx_a[SCC_CH_A_RING_CAP]; /* ホスト→ゲスト */
    int rx_a_head, rx_a_count;

    uint8_t tx_a[SCC_CH_A_RING_CAP]; /* ゲスト→ホスト */
    int tx_a_head, tx_a_count;

    int recv_request_a; /* ch A Rx割り込み要求ペンディング */
} scc_t;

void scc_reset(scc_t *scc);
uint8_t scc_read8(scc_t *scc, uint32_t addr);
void scc_write8(scc_t *scc, uint32_t addr, uint8_t val);

/* bus層のメモリ読み取り関数を登録する（x68k_bus.cが初期化時に一度だけ呼ぶ）。
 * RTS立ち上がり時、IOCSのマウスカーソル座標ワーク($0ACE)をこの経路で参照する。 */
void scc_set_mem_read32(scc_t *scc, scc_mem_read32_fn fn, void *ctx);
void scc_rebase_host_pos(scc_t *scc);

/* ホストの現在のマウス位置（ネイティブ解像度換算後）を更新する
 * （platformコールバック経由、main.cが橋渡し）。 */
void scc_mouse_set_host_pos(scc_t *scc, int x, int y);
void scc_mouse_button(scc_t *scc, int left, int right);

/* IRQ5(SCC ch A/B受信)が保留中か（どちらか一方でも1） */
int scc_pending_irq(const scc_t *scc);
/* IACK時にベクタを返し、recv_requestを消費する（次バイトは$38コマンドで再武装） */
uint8_t scc_iack(scc_t *scc);
/* ch A版IACK（実チップの優先度チェーンでch Aはch Bより高優先、呼び出し側
 * (x68k_bus.c)がch A→ch Bの順でrecv_request_a/recv_requestを見て呼び分ける） */
uint8_t scc_iack_a(scc_t *scc);

/* ホストから受信したバイト列をch A受信リングへ積む（PTY等→main.cが毎
 * フレーム呼ぶ）。リングが満杯なら残りは捨てる。WR1のRx割り込み許可が
 * 立っていればIRQ5(ch A)要求を立てる。 */
void scc_recv_a(scc_t *scc, const uint8_t *buf, int n);

/* ch A送信リングからホストへ書き出すべきバイト列を取り出す（main.cが毎
 * フレーム呼び、pal_serial_write()へそのまま渡す）。outへ最大maxバイト
 * 書き込み、取り出せた個数を返す。 */
int scc_send_a_dequeue(scc_t *scc, uint8_t *out, int max);

/* ホスト側RS-232C経路の接続状態(DCD/CTS相当)を設定する。 */
void scc_serial_set_carrier(scc_t *scc, int connected);

#endif /* X68K_SCC_H */
