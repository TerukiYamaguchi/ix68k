#include "peripheral/sasi.h"
#include <string.h>

#define HDC_STATUS_MSG 0x10u
#define HDC_STATUS_CD  0x08u
#define HDC_STATUS_IO  0x04u
#define HDC_STATUS_BSY 0x02u
#define HDC_STATUS_REQ 0x01u

/* SASIのステータスバイト（『Inside X68000』図6-14 / XEiJ HDC.java hdcStatusPhase）。
 * 0x02 は「センスデータあり」= CHECK CONDITION。 */
#define HDC_ST_GOOD            0x00u
#define HDC_ST_CHECK_CONDITION 0x02u

/* SASIの1バイトセンスコード（XEiJ HDC.java 準拠）。SPC(scsi.c)が使う
 * 拡張センス（[0]=0x70, [2]=センスキー）とは別形式であることに注意。 */
#define HDC_SENSE_INVALID_COMMAND        0x20u
#define HDC_SENSE_INVALID_SECTOR_ADDRESS 0x21u

#define HDC_CDB_LUN_SHIFT 5u /* CDB[1] bit5-7 = LUN（IPLROM $FF9B9E が書き込む） */

void sasi_init(sasi_t *hdc, scsi_unit_t *units) {
    memset(hdc, 0, sizeof(*hdc));
    hdc->units = units;
    sasi_reset(hdc);
}

void sasi_reset(sasi_t *hdc) {
    hdc->phase = SASI_PHASE_BUS_FREE;
    hdc->selected_id = -1;
    hdc->cmd_index = 0;
    hdc->data_index = 0;
    hdc->data_limit = 0;
}

/* STATUSフェーズへ遷移する。MESSAGEバイトは常に0
 * （XEiJ HDC.java hdcStatusPhase も message は常に0）。 */
static void hdc_status_phase(sasi_t *hdc, uint8_t status) {
    hdc->status_byte = status;
    hdc->message_byte = 0x00;
    hdc->phase = SASI_PHASE_STATUS;
}

/* CHECK CONDITIONで終了する。センスコードは直後にROMが発行する
 * REQUEST SENSE STATUS(0x03)で読み出される（IPLROMXV $FF9AD8-$FF9B0E）。 */
static void hdc_check_condition(sasi_t *hdc, uint8_t sense_code) {
    hdc->sense_buf[0] = sense_code;
    hdc_status_phase(hdc, HDC_ST_CHECK_CONDITION);
}

/* コマンドフェーズで6バイト揃った時点で呼ばれる（『Inside X68000』 図6-14準拠）。
 * READ(0x08)/TEST DRIVE READY(0x00)/RECALIBRATE(0x01)/REQUEST SENSE STATUS(0x03)/
 * FORMAT DRIVE(0x04、no-op成功扱い)/SEEK(0x0B、no-op+範囲チェック)を実装。
 * WRITE(0x0A)等は未実装でCHECK CONDITION。
 * CDB[1] bit5-7のLUNはLUN0のみ実装し、LUN!=0はINVALID COMMANDで弾く
 * （IPLROMはPDA $8nをID=n>>1/LUN=n&1に分解する）。 */
static void hdc_execute_command(sasi_t *hdc) {
    int oc = hdc->cmd_buf[0];
    int lun = (hdc->cmd_buf[1] >> HDC_CDB_LUN_SHIFT) & 0x07;
    scsi_unit_t *unit = (hdc->selected_id >= 0) ? &hdc->units[hdc->selected_id] : NULL;

    /* REQUEST SENSE STATUS: LUN・メディアの状態に関わらず必ず応答する。
     * ROMはCHECK CONDITIONを受けると同じLUNのままこのコマンドを再発行し
     * （IPLROMXV $FF9ADE）、DATA INフェーズを1バイトあたり最大250,000回
     * ポーリングする（$FF9B16）。ここで弾くとエラー情報が失われるうえ、
     * そのタイムアウトを待つ空回りが発生する。XEiJのHDC.javaはメディア無しで
     * REQUEST SENSEも弾くが、XEiJの内蔵HDCはリムーバブルを扱わない前提のため
     * 到達しない隅であり、うちは空MO/空CD-ROMを同じunits[]に載せるので通す
     * （scsi.cのmedia guardが0x03/0x12を除外しているのと同じ考え方）。 */
    if (oc == 0x03) {
        memcpy(hdc->data_buf, hdc->sense_buf, sizeof(hdc->sense_buf));
        memset(hdc->sense_buf, 0, sizeof(hdc->sense_buf)); /* 読み出しでクリア */
        hdc->data_index = 0;
        hdc->data_limit = (int)sizeof(hdc->sense_buf);
        hdc->phase = SASI_PHASE_DATA_IN;
        return;
    }

    /* LUN: IPLROMはSASIユニット番号n(PDA $8n)をID=n>>1/LUN=n&1に分解し、
     * IDをSELECTビットマップへ、LUNをCDB[1] bit5-7へ載せる（$FF9A30/$FF9B9E）。
     * units[]はscsi.cと共有する「添字=SCSI ID」の配列でLUNの次元を持たないため、
     * LUN0のみを実装し、それ以外は存在しないユニットとして弾く
     * （XEiJがLUN>1をINVALID COMMANDで弾くのと同じ扱い）。 */
    if (lun != 0) {
        hdc_check_condition(hdc, HDC_SENSE_INVALID_COMMAND);
        return;
    }

    if (!unit || !unit->fp) {
        hdc_check_condition(hdc, HDC_SENSE_INVALID_COMMAND);
        return;
    }

    switch (oc) {
    case 0x00: /* TEST DRIVE READY */
    case 0x01: /* RECALIBRATE */
    case 0x04: /* FORMAT DRIVE: 実フォーマットはせず成功のみ返す */
        hdc_status_phase(hdc, HDC_ST_GOOD);
        break;
    case 0x0B: { /* SEEK: LBA=CDB[1]下位5bit:CDB[2]:CDB[3]（256Bブロック単位、
                  * sasi_read8/writeのSASI_BLOCK_SIZEと同じ単位系）。実データ
                  * 移動を伴わないno-opで、範囲内はGOOD、範囲外はCHECK
                  * CONDITION（0x08 READのdisk_bytes境界判定と同じ基準、
                  * scsi.cのSCSI Seek(6)実装とも同じ方針。IOCS
                  * `_B_SEEK`(0x40)のPDA $80-$8F側はSPC($E96020,scsi.c)では
                  * なくこの内蔵HDC($E96000)経由でROMが発行するCDBのため
                  * こちらに実装が必要と判明した）。 */
        uint32_t lba = ((uint32_t)(hdc->cmd_buf[1] & 0x1Fu) << 16) |
                       ((uint32_t)hdc->cmd_buf[2] << 8) | hdc->cmd_buf[3];
        uint64_t offset = (uint64_t)lba * SASI_BLOCK_SIZE;
        uint64_t disk_bytes = (uint64_t)unit->disk_end_record * unit->bytes_per_record;
        if (offset > disk_bytes) {
            hdc_check_condition(hdc, HDC_SENSE_INVALID_SECTOR_ADDRESS);
            break;
        }
        hdc_status_phase(hdc, HDC_ST_GOOD);
        break;
    }
    case 0x08: { /* READ: LBA=CDB[1]下位5bit:CDB[2]:CDB[3]、count=CDB[4] (0=256慣習) */
        uint32_t lba = ((uint32_t)(hdc->cmd_buf[1] & 0x1Fu) << 16) |
                       ((uint32_t)hdc->cmd_buf[2] << 8) | hdc->cmd_buf[3];
        uint32_t count = hdc->cmd_buf[4];
        if (count == 0)
            count = 256;
        uint64_t offset = (uint64_t)lba * SASI_BLOCK_SIZE;
        uint64_t total = (uint64_t)count * SASI_BLOCK_SIZE;
        uint64_t disk_bytes = (uint64_t)unit->disk_end_record * unit->bytes_per_record;
        if (total > sizeof(hdc->data_buf) || offset + total > disk_bytes ||
            fseek(unit->fp, (long)offset, SEEK_SET) != 0 ||
            fread(hdc->data_buf, 1, (size_t)total, unit->fp) != (size_t)total) {
            hdc_check_condition(hdc, HDC_SENSE_INVALID_SECTOR_ADDRESS);
            break;
        }
        hdc->data_index = 0;
        hdc->data_limit = (int)total;
        hdc->phase = SASI_PHASE_DATA_IN;
        break;
    }
    default: /* WRITE(0x0A)等、未実装コマンド */
        hdc_check_condition(hdc, HDC_SENSE_INVALID_COMMAND);
        break;
    }
}

uint8_t sasi_read8(sasi_t *hdc, uint32_t off) {
    switch (off) {
    case 0x01: /* DATA: フェーズに応じて自動ハンドシェイク */
        switch (hdc->phase) {
        case SASI_PHASE_DATA_IN:
            if (hdc->data_index < hdc->data_limit) {
                uint8_t v = hdc->data_buf[hdc->data_index++];
                if (hdc->data_index >= hdc->data_limit)
                    hdc_status_phase(hdc, HDC_ST_GOOD);
                return v;
            }
            return 0;
        case SASI_PHASE_STATUS:
            hdc->phase = SASI_PHASE_MESSAGE;
            return hdc->status_byte;
        case SASI_PHASE_MESSAGE:
            hdc->phase = SASI_PHASE_BUS_FREE;
            hdc->selected_id = -1;
            return hdc->message_byte;
        default:
            return 0;
        }
    case 0x03: { /* ステータス: bit4=MSG bit3=C/D bit2=I/O bit1=BSY bit0=REQ */
        uint8_t v = 0;
        if (hdc->phase != SASI_PHASE_BUS_FREE)
            v |= HDC_STATUS_BSY | HDC_STATUS_REQ; /* 実タイミング無しで常時ready扱い */
        switch (hdc->phase) {
        case SASI_PHASE_COMMAND:
            v |= HDC_STATUS_CD;
            break;
        case SASI_PHASE_DATA_IN:
            v |= HDC_STATUS_IO;
            break;
        case SASI_PHASE_DATA_OUT:
            break;
        case SASI_PHASE_STATUS:
            v |= HDC_STATUS_IO | HDC_STATUS_CD;
            break;
        case SASI_PHASE_MESSAGE:
            v |= HDC_STATUS_IO | HDC_STATUS_CD | HDC_STATUS_MSG;
            break;
        default:
            break;
        }
        return v;
    }
    default:
        return 0;
    }
}

void sasi_write8(sasi_t *hdc, uint32_t off, uint8_t val) {
    switch (off) {
    case 0x01: /* DATA */
        if (hdc->phase == SASI_PHASE_COMMAND && hdc->cmd_index < 6) {
            hdc->cmd_buf[hdc->cmd_index++] = val;
            if (hdc->cmd_index == 6)
                hdc_execute_command(hdc);
        }
        break;
    case 0x03: /* SEL='0'（デセレクト）: 明示的な状態変更は不要（$E96007側で遷移済み） */
        break;
    case 0x05: /* RST: 約300us間'1'。実タイミング無しで即時リセットとする */
        sasi_reset(hdc);
        break;
    case 0x07: { /* SEL='1'（セレクション開始）: valがIDビットマップ */
        if (hdc->phase != SASI_PHASE_BUS_FREE)
            break;
        int id = -1;
        for (int i = 0; i < SCSI_NUM_UNITS; i++) {
            if (val & (uint8_t)(1u << i)) {
                id = i;
                break;
            }
        }
        /* 空リムーバブル(fp==NULL, present=1)もドライブが存在するためSELECTに
         * 応答する。scsi.c(SPC)のSelect実装と同一条件・同一構造にしてある
         * （units[]はscsi.cと共有の配列なので、存在判定が2種類あってはならない）。
         * 未接続ID(present=0)は応答せずBUS_FREEに留まり、ROM側の
         * SELECTタイムアウト（$FF9A50、BSYを5000回ポーリング）で正しく
         * エラーになる。 */
        int connected = (id >= 0 && (hdc->units[id].fp != NULL || hdc->units[id].present));
        if (connected) {
            hdc->selected_id = id;
            hdc->cmd_index = 0;
            hdc->phase = SASI_PHASE_COMMAND;
        }
        break;
    }
    default:
        break;
    }
}

int sasi_dma_active(const sasi_t *hdc) {
    return hdc->phase == SASI_PHASE_DATA_IN && hdc->data_index < hdc->data_limit;
}

uint32_t sasi_dma_remaining(const sasi_t *hdc) {
    return (uint32_t)(hdc->data_limit - hdc->data_index);
}
