#ifndef X68K_SASI_H
#define X68K_SASI_H

#include <stdint.h>
#include "peripheral/scsi.h"

/*
 * 内蔵 HDC (SASI互換インタフェース) 最小実装。$E96000-$E9601F。
 * 『Inside X68000』 「SASI」章（図5-14）準拠。
 * SPC($E96020、MB89352、scsi.c)とは別の、IPLROM自身が起動時に
 * SRAM `$ED0018`(最優先ブートデバイス)=`$8?00`のとき直接叩く簡易インタフェース
 * （IPLROMXV boot_dispatcher $FF96CE以降が$E96001/3/5/7を直接操作しており、
 * SPCとは独立した経路のため個別実装が必要）。
 *
 * ユニットのディスクデータは scsi_t.units[] （scsi_attach/detach で
 * 接続されたSCSI ID 0-6のイメージ）をそのまま共有参照する。
 *
 * レジスタ:
 *   $E96001 R/W DATA（アクセスで自動REQ-ACKハンドシェイク、全フェーズ共通）
 *   $E96003 R   ステータス: bit4=MSG bit3=C/D bit2=I/O bit1=BSY bit0=REQ
 *   $E96003 W   DATAをバスに出力しSEL='0'（デセレクト、通常$00）
 *   $E96005 W   RST信号を約300us間'1'にする（即時リセットとしてエミュレート）
 *   $E96007 W   DATAをバスに出力しSEL='1'（セレクション開始、ID bitmask）
 *
 * SASIユニット番号（IOCSのPDA $8n の下位4bit n）は、IPLROMが
 * ID = n >> 1 / LUN = n & 1 に分解し、IDを$E96007のビットマップへ、
 * LUNをCDB[1] bit5-7へ載せる（IPLROMXV $FF9A30 / $FF9B9E）。units[]は
 * scsi.cと共有する「添字=SCSI ID」の配列でLUNの次元を持たないため、
 * 本実装はLUN0のみを扱い、LUN!=0は存在しないユニットとしてCHECK CONDITION
 * （センス$20 INVALID COMMAND）を返す。
 * SELECTの存在判定はscsi.c(SPC)と同じ `fp != NULL || present` で、
 * メディア未挿入の空リムーバブルにも応答する。
 *
 * ブロックサイズは仕様上256バイト固定（SPC経由のscsi_unit_t.bytes_per_recordとは
 * 独立、SASI仕様書に明記）。
 */

#define SASI_BLOCK_SIZE 256u
#define SASI_BUF_SIZE   32768u /* 128ブロック分。IPLROMの起動時読み込み(数KB程度)に十分 */

typedef enum {
    SASI_PHASE_BUS_FREE = 0,
    SASI_PHASE_COMMAND,
    SASI_PHASE_DATA_IN,
    SASI_PHASE_DATA_OUT,
    SASI_PHASE_STATUS,
    SASI_PHASE_MESSAGE,
} sasi_phase_t;

typedef struct {
    scsi_unit_t *units; /* scsi_t.units[] を共有参照（SCSI_NUM_UNITSエントリ） */

    sasi_phase_t phase;
    int selected_id; /* -1=未選択、0-6=選択中ユニット */

    uint8_t cmd_buf[6];
    int cmd_index;

    uint8_t status_byte;
    uint8_t message_byte;
    uint8_t sense_buf[4];

    uint8_t data_buf[SASI_BUF_SIZE];
    int data_index, data_limit;
} sasi_t;

/* units: scsi_t.units[] へのポインタ（SCSI_NUM_UNITS要素、寿命はsasiより長いこと）
 */
void sasi_init(sasi_t *hdc, scsi_unit_t *units);
void sasi_reset(sasi_t *hdc);
uint8_t sasi_read8(sasi_t *hdc, uint32_t off);
void sasi_write8(sasi_t *hdc, uint32_t off, uint8_t val);

/* ioi_init（IPLROM $FF0F16）がDMAC ch1のデバイスアドレスレジスタに$E96001
 * (HDC DATAポート)を設定するため、READコマンドのデータ転送はDMAC ch1 CCR
 * STARTで駆動される（scsi_dma_active/remainingと同じ設計）。
 * 実データ移動はDMAC(dmac.c)が$E96001をx68k_bus_read8経由でバイト単位アクセス
 * することで行う（sasi_read8のDATAパスがdata_index/limitを自前で前進
 * させる）。 */
int sasi_dma_active(const sasi_t *hdc);
uint32_t sasi_dma_remaining(const sasi_t *hdc);

#endif /* X68K_SASI_H */
