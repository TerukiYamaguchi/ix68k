#ifndef X68K_DMAC_H
#define X68K_DMAC_H

#include <stdbool.h>
#include <stdint.h>
#include "bus/x68k_bus.h"

/* DMAC ch3(ADPCM)のフレームペースド転送状態。ch0-2の
 * dmac_transfer_block()は「CCR START時に全バイト瞬時転送」する同期モデルだが、
 * ADPCM再生・録音はMSM6258のサンプリング周波数(3.9k-15.6kHz)に同期した実時間
 * 転送が前提のため、CCR STARTでは以下へラッチするのみで転送しない。実転送は
 * x68k_bus_adpcm_tick()（dmac.c）がCPUチャンク単位で漸進的に行う。
 * chain(OCRビット3-2)!=0（_ADPCMAOT/AIN/LOT/LINが使うアレイ/リンクアレイ
 * チェーン）はbar/btc/chain_doneでエントリをまたいだ状態をtick()間で保持し、
 * ch2のdmac2_start()と同じテーブル形式（+0=long転送元アドレス, +4=word
 * 転送バイト数、リンクのみ+6=long次テーブルアドレス・0で終端）をフレーム
 * またぎで漸進的に走査する。 */
typedef struct {
    int active;         /* 1=転送進行中 */
    int paused;         /* 1=一時停止中(_ADPCMMOD HLT)。activeは維持したまま転送のみ止める */
    int dir;            /* OCR.DIR: 0=再生(mem→dev) 1=録音(dev→mem) */
    int mac, dac;       /* SCR.MAC/DAC: MAR/DARそれぞれの1バイトごとの増減 */
    uint32_t mar;       /* 現在のメモリアドレス(進行中の値) */
    uint32_t dar;       /* 現在のデバイスアドレス(進行中の値、実際はDAC=0固定で不変) */
    uint32_t remaining; /* 現在のブロック(chain!=0時は現在のエントリ)の残バイト数 */

    int chain;      /* OCR.CHAIN: 0=単純 2=アレイ 3=リンクアレイ */
    uint32_t bar;   /* 次にロードすべきチェーンテーブルエントリのアドレス */
    uint32_t btc;   /* アレイチェーン(chain==2)の残エントリ数。0で終端 */
    int chain_done; /* リンクアレイ(chain==3)で次テーブルアドレスが0だった
                     * (現在のエントリ処理完了後に終了)ことを示すフラグ */

    int cnt; /* CCR.CNT(bit6)コンティニュー動作の武装フラグ。単純転送(chain==0)で
              * MTCが0になったとき、1ならBAR→MAR / BTC→MTCを取り込んで転送を
              * 継続し、0なら転送を終了する。継続のたびにハードウェアが自動
              * クリアするため、ソフトウェアが毎ブロック再セットする。ゲストが
              * 読み返すCCRレジスタのbit6とは常に一致させる
              * （adpcm_dma_set_cnt()/x68k_bus_adpcm_tick()参照）。 */

    uint32_t budget_acc; /* 経過CPUサイクルあたりの転送バイト数
                          * (fs * cycles / (2 * cpu_hz)、割り切れない)を誤差蓄積
                          * (Bresenham型)で正確に配分するための状態。単位は
                          * 「fs * CPUサイクル」で、剰余のため常に2*cpu_hz未満。
                          * CCR STARTで0にリセットする（x68k_bus_adpcm_tick()参照）。 */
} adpcm_dma_t;

typedef struct {
    int ch2_irq_pending;    /* DMAC ch#2の正常割り込み要求pending。IACK時にch2 NIVを返す。
                               IOCS _DMAMOVE系(0x8A-0x8C)のROM委譲に必要、dmac.c参照） */
    int ch3_irq_pending;    /* DMAC ch#3(ADPCM)の正常割り込み要求pending。COCと、CCR.ITEが
                               有効なCNT継続時のBTCを含み、IACK時にch3 NIVを返す。
                               IOCS _ADPCMOUT等(0x60-0x67)のROM委譲に必要。ch2と同時
                               pending時はch2優先（dmac_iack参照） */
    int ch2_error_pending;  /* ch2エラー割り込み。IACK時にEIVを返す。 */
    adpcm_dma_t ch3;        /* DMAC ch3フレームペースド転送状態 */
    int64_t ch2_credit_num; /* ch2の未消費時間。単位はDMACクロック×CPU Hz。 */
    uint32_t ch2_mar, ch2_dar, ch2_mtc;
    uint32_t ch2_bar, ch2_btc, ch2_next_bar;
    /* active中ロック書込みを破棄した直後、同一CPUアクセスの続きバイトも破棄する */
    uint32_t ch2_discard_addr;
    int ch2_discard_left;
    uint32_t ch2_entries; /* STARTから読んだチェーンエントリ数（循環リンクの安全停止用） */
    uint8_t ch2_dir, ch2_mac, ch2_dac, ch2_chain, ch2_ite;
    uint8_t ch2_unit; /* 1転送単位のバイト数（OCR.SIZE: 00 byte=1, 01 word=2）。MTCは単位数 */
    int ch2_active;
    bool (*cycle_source)(void *ctx, uint64_t *out); /* チャンク内同期用。NULL=同期なし */
    void *cycle_ctx;
    uint64_t synced_cycles; /* 現チャンクでch2へ反映済みのCPUサイクル */
    int chunk_active;       /* 1=実行チャンク中。sync_to_now()が有効 */
    int syncing;            /* 1=DMA自身のバスアクセス中。再帰同期を抑止する */
    int ch0_armed;          /* DMAC ch0(FDC) CCR STARTによる武装フラグ。
                               ch1と異なりCCR STARTがFDCコマンド送出より先に発行される
                               （実機計装で確認、ゼロ例外）ため、CCR書き込み時点では
                               FDCがまだどのセクタを転送するか未確定で転送できない。
                               このフラグだけ立てて、実転送はfdc_exec()がREAD/WRITE
                               DATAコマンド実行完了後にdmac_ch0_fdc_transfer()を呼んで
                               行う。武装されていない場合はフォールバック(内部ドレイン)
                               に委ねる（stale register誤転送を防ぐガード）。 */
} dmac_t;

/* ---- DMAC (HD63450) レジスタ書き込み（実体は dmac.c）。write8 から呼ぶ。
 *      0xE84000/E84040/E84047 を処理したら 1、非該当なら 0 を返す ---- */
int dmac_write8(x68k_bus_t *b, uint32_t addr, uint8_t val);

/* ---- DMAC ch0(FDC)汎用エンジン呼び出し（実体は dmac.c）。fdc_exec()から呼ぶ。
 *      戻り値: 1=CCR STARTで武装済みのため汎用エンジンで消費した、
 *      0=非武装のため消費しなかった（呼び出し元はフォールバックすること）。
 *      b->fdc.dma_ptr/dma_remaining/dma_dir（FDC_EXEC状態）を消費する。 ---- */
int dmac_ch0_fdc_transfer(x68k_bus_t *b);

/* ---- DMACのレベル3 IACK。ch2/ch3のpendingを優先順に消費し、該当チャネルの
 *      NIVレジスタ値を*vectorへ返して1、要求が無ければ0を返す。実体はdmac.c。 ---- */
int dmac_iack(x68k_bus_t *b, uint8_t *vector);

/* ---- RESET命令相当のDMAC初期化（実体は dmac.c）。実機のHD63450は
 *      RESET信号で4ch全レジスタが電源投入相当へ戻る（NIV/EIVのみ未初期化割り込みを
 *      示す$0F。XEiJ HD63450.java dmaReset()準拠）。レジスタ実体は
 *      b->crtc_regs[$4000-$40FF]の生バイトなので、chごとに$40バイトを
 *      ゼロクリアしてNIV/EIVだけ$0Fへ戻す（CRTC/パレット等は範囲外で無傷）。
 *      dmac_t/adpcm_dma_tはホスト側設定も外部ピン相当の状態も持たないため
 *      構造体全体をゼロクリアしてよい（フィールド追加時はここを再確認すること）。
 *      デバイス側のDMA状態（FDCの実行データフェーズ、SPC/HDCの転送残）は各
 *      デバイスのリセット責務であり、ここでは触らない。 ---- */
void dmac_reset(x68k_bus_t *b);

#endif /* X68K_DMAC_H */
