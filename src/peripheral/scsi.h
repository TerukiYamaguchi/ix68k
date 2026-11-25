#ifndef X68K_SCSI_H
#define X68K_SCSI_H

#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include "bus/x68k_bus.h"
#include "peripheral/media_fp.h"

/*
 * 内蔵SPC (MB89352)のレジスタ、SCSIフェーズ、コマンド実行をモデル化する。
 * オフセットはbase=$E96020、addr&31で以下にディスパッチする。
 *   0x01 BDID  0x03 SCTL  0x05 SCMD  0x09 INTS  0x0b PSNS(R)/SDGC(W)
 *   0x0d SSTS  0x0f SERR  0x11 PCTL  0x13 MBC  0x15 DREG  0x17 TEMP
 *   0x19 TCH  0x1b TCM  0x1d TCL（TCH:TCM:TCLで24bit転送カウンタ）
 */

#define SCSI_NUM_UNITS 7 /* SCSI ID 0-6（自ID7を除く内蔵ユニット、拡張ボード非対応） */
#define SCSI_DATA_BUF_SIZE                                              \
    1024 /* Inquiry(36B)/Read Capacity(8B)/READ TOC(理論最大804B)等の \
          * 固定長応答格納用（docs/ix68k-internals/design/scsi_design.md §6）。 */
/* Human68kのCOPYで観測した最大1,374,208バイトのWrite(10)に余裕を持たせる。 */
#define SCSI_XFER_BUF_SIZE 2097152
/* Request Senseの固定長センスデータ長。ASC(byte 12)/ASCQ(byte 13)まで含む
 * SCSI-2の標準サイズで、byte 7の追加センス長は SCSI_SENSE_LENGTH - 8 になる。 */
#define SCSI_SENSE_LENGTH  18

/* CD-ROM(CloneCD)関連定数。docs/ix68k-internals/design/scsi_design.md §4準拠。 */
#define SCSI_CD_MAX_TRACKS    99
#define SCSI_CD_RAW_SECTOR    2352 /* CloneCD .imgの1セクタ */
#define SCSI_CD_MODE1_OFFSET  16   /* 同期12B+ヘッダ4B。ユーザデータの先頭 */
#define SCSI_CD_BLOCK_SIZE    2048 /* CD-ROMの論理ブロック長 */
#define SCSI_CD_LEADOUT_TRACK 0xAA

typedef enum {
    SCSI_UNIT_DISK = 0,  /* HDD/MO。X68SCSI1ヘッダ付きrawイメージ */
    SCSI_UNIT_CDROM = 1, /* CD-ROM。CloneCDイメージ(.ccd+.img) */
} scsi_unit_kind_t;

typedef struct {
    uint8_t track_no;    /* 1-99。リードアウトは0xAA */
    uint8_t adr_control; /* READ TOCディスクリプタの[+1]: ADR<<4 | Control */
    uint32_t start_lba;  /* トラック開始LBA */
} scsi_cd_track_t;

typedef struct scsi_overlay scsi_overlay_t;

typedef struct {
    FILE *fp;                  /* raw HDD/CD-ROMイメージ("r+b"/"rb")、NULL=メディア未挿入 */
    uint32_t bytes_per_record; /* BLEN: デバイスパラメータのブロック長(256/512/1024)。
                                * CD-ROMでは常に2048(SCSI_CD_BLOCK_SIZE)。 */
    uint32_t disk_end_record;  /* 総ブロック数。CD-ROMではリードアウトLBA。 */
    int removable;             /* デバイスパラメータ$0F(EJ)!=0: リムーバブルメディア(MO等)。
                                * Inquiry応答のRMBビットに反映する。CD-ROMでは常に1。 */
    char *path;                /* 表示用イメージファイルパス。NULL=メディア無し。 */
    char *img_path;            /* CloneCDの.img実パス。通常のHDD/MOはNULL。 */
    media_fp_t media_fp;
    int present;           /* 1=このIDにドライブが存在する(SELECTに応答する)。
                            * リムーバブルはfp==NULLでもドライブが存在し得る。 */
    int attention_pending; /* メディア無し応答でUNIT ATTENTIONを一度返すための印。 */

    scsi_unit_kind_t kind;   /* このユニットの種別(HDD/MO or CD-ROM) */
    int write_protected;     /* 1=書き込み系コマンドをDATA PROTECTで拒否。CD-ROMは常に1。 */
    scsi_overlay_t *overlay; /* 明示指定時の固定HDD用copy-on-write差分。 */

    /* CD-ROM専用フィールド(kind==SCSI_UNIT_CDROMのときのみ有効) */
    uint32_t cd_raw_sector_size;  /* .imgの1セクタの生バイト数(通常2352) */
    uint32_t cd_user_data_offset; /* セクタ先頭からユーザデータまでのオフセット(Mode1=16) */
    uint8_t cd_first_track, cd_last_track;
    scsi_cd_track_t cd_tracks[SCSI_CD_MAX_TRACKS];
    int cd_track_count;
    uint8_t cd_leadout_adr_control; /* .ccd Point=0xA2エントリのADR<<4|Control。
                                     * READ TOCのリードアウトディスクリプタに使う。 */
} scsi_unit_t;

typedef struct {
    /* レジスタ */
    uint8_t bdid, sctl, scmd, ints, psns, ssts, serr, pctl, mbc, dreg, temp;
    uint32_t tc; /* TCH:TCM:TCL 24bit 転送カウンタ */

    /* ユニット接続・フェーズ状態 */
    scsi_unit_t units[SCSI_NUM_UNITS];
    int target_id; /* 現在のターゲットSCSI ID(0-6)、-1=バスフリー */
    uint64_t media_gen;
    bool media_gen_overflow;

    const uint8_t *read_handle; /* ターゲット→イニシエータ転送元 */
    uint8_t *write_handle;      /* イニシエータ→ターゲット転送先 */
    int buf_index, buf_limit;

    /* Write(6)/(10)のペンディング書き込み。DATA_OUT完了時に
     * xfer_bufをwrite_unitのイメージへfseek+fwrite+fflushで永続化する。 */
    int pending_write;
    uint32_t write_lba;
    scsi_unit_t *write_unit;
    uint8_t cmd_buf[16]; /* CDB */
    uint8_t status_buf[1];
    uint8_t msgin_buf[1];
    uint8_t msgout_buf[1]; /* IDENTIFY等、MESSAGE OUTで受ける1バイト */
    /* Request Sense応答バッファ。
     * [0]=0x70(拡張センスフォーマット), [2]=センスキー, [7]=追加センス長,
     * [12]=ASC, [13]=ASCQ。spc_set_sense()でのみ組み立てる。現実装はチップ全体で
     * 共有するためLUNごとのsense保存は未対応。 */
    uint8_t sense_buf[SCSI_SENSE_LENGTH];
    uint8_t data_buf[SCSI_DATA_BUF_SIZE];
    uint8_t xfer_buf[SCSI_XFER_BUF_SIZE]; /* Read/Writeのセクタ転送 */

    x68k_bus_t *
        bus; /* IOI割り込み発火用（x68k_bus_scsi_irq_raise呼び出し）。x68k_bus_create()で設定 */
} scsi_t;

/* SPCのレジスタと転送状態をリセットする。接続中のユニットは維持する。 */
void scsi_reset(scsi_t *spc);
uint8_t scsi_read8(scsi_t *spc, uint32_t off);
void scsi_write8(scsi_t *spc, uint32_t off, uint8_t val);

/* イメージをid(0-6)へ接続する。ファイル先頭の署名/拡張子で形式を自動判別する:
 *   "X68SCSI1" ヘッダ           → HDD/MO (SCSI_UNIT_DISK)
 *   拡張子.ccd または先頭"[CloneCD]" → CD-ROM (SCSI_UNIT_CDROM、対の.imgも開く)
 * 戻り値: 0=成功, -1=ID・ファイルI/O・署名・形式判別のエラー。 */
int scsi_attach(scsi_t *spc, int id, const char *path);
int scsi_enable_overlay(scsi_t *spc, int id, const char *path);
/* リムーバブルはメディアだけを外し、固定ディスクはユニットごと切断する。 */
void scsi_detach(scsi_t *spc, int id);

/*
 * メディア未挿入の空リムーバブルドライブ（MO等）をid(0-6)へ接続する。
 * ドライブはSELECTに応答する。Test Unit Ready等の制御コマンドはGOOD、
 * Read等のメディアアクセスはCHECK CONDITIONを返す。後からscsi_attach()で
 * メディアを挿入できる。戻り値: 0=成功, -1=失敗（idが範囲外）。
 */
int scsi_attach_empty_removable(scsi_t *spc, int id);

/*
 * メディア未挿入の空CD-ROMドライブをid(0-6)へ接続する。scsi_attach_empty_removable()と
 * 同様だがkind=SCSI_UNIT_CDROM・write_protected=1を設定する。後からscsi_attach()で
 * .ccdイメージを挿入できる。戻り値: 0=成功, -1=失敗（idが範囲外）。
 */
int scsi_attach_empty_cdrom(scsi_t *spc, int id);
void scsi_media_changed(scsi_t *spc);
scsi_overlay_t *scsi_overlay_clone(const scsi_overlay_t *overlay);
void scsi_overlay_destroy(scsi_overlay_t *overlay);

/* DMAC channel #1はDREGをバイト単位でアクセスして転送する。
 * 以下のAPIは転送可否と残りバイト数をDMACへ公開する。 */

/* Data-In転送中(read_handle有効かつSSTS.TRIP=1かつtc!=0)かどうかを返す。 */
int scsi_dma_active(const scsi_t *spc);

/* 残りバイト数(buf_limit-buf_index)を返す。DMAC側がMTCとのmin()で
 * 転送バイト数の上限を決めるために使う（CDBのAllocation Lengthが
 * 実データ長より大きい場合の転送過多を防ぐ）。 */
uint32_t scsi_dma_remaining(const scsi_t *spc);

/* Write(6)/(10)のDATA_OUT転送用API。 */

/* Data-Out転送中(write_handle有効かつSSTS.TRIP=1かつtc!=0)かどうかを返す。 */
int scsi_dma_write_active(const scsi_t *spc);

/* 残りバイト数(buf_limit-buf_index)を返す。 */
uint32_t scsi_dma_write_remaining(const scsi_t *spc);

#endif /* X68K_SCSI_H */
