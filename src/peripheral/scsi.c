#include "peripheral/scsi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t lba;
    long offset;
} scsi_overlay_entry_t;
typedef struct {
    uint32_t lba;
    uint32_t size;
} scsi_overlay_record_t;

static const uint8_t s_overlay_magic[8] = {'I', 'X', '6', '8', 'C', 'O', 'W', '1'};
struct scsi_overlay {
    FILE *fp;
    char *path;
    scsi_overlay_entry_t *entries;
    size_t count, capacity;
};

static int overlay_find(const scsi_overlay_t *overlay, uint32_t lba) {
    for (size_t i = 0; i < overlay->count; i++)
        if (overlay->entries[i].lba == lba)
            return (int)i;
    return -1;
}

static int overlay_reserve(scsi_overlay_t *overlay) {
    if (overlay->count < overlay->capacity)
        return 1;
    size_t capacity = overlay->capacity ? overlay->capacity * 2 : 64;
    scsi_overlay_entry_t *entries = realloc(overlay->entries, capacity * sizeof(*entries));
    if (!entries)
        return 0;
    overlay->entries = entries;
    overlay->capacity = capacity;
    return 1;
}

static int overlay_read(const scsi_overlay_t *overlay, uint32_t lba, uint8_t *dst,
                        uint32_t size) {
    int i = overlay_find(overlay, lba);
    if (i < 0)
        return 1;
    return fseek(overlay->fp, overlay->entries[i].offset, SEEK_SET) == 0 &&
           fread(dst, 1, size, overlay->fp) == size;
}
static int overlay_write(scsi_overlay_t *overlay, uint32_t lba, const uint8_t *src,
                         uint32_t size) {
    int i = overlay_find(overlay, lba);
    if (i < 0) {
        if (!overlay_reserve(overlay))
            return 0;
        i = (int)overlay->count++;
        overlay->entries[i].lba = lba;
    }
    if (fseek(overlay->fp, 0, SEEK_END) != 0)
        return 0;
    scsi_overlay_record_t record = {.lba = lba, .size = size};
    if (fwrite(&record, 1, sizeof(record), overlay->fp) != sizeof(record))
        return 0;
    long off = ftell(overlay->fp);
    if (off < 0 || fwrite(src, 1, size, overlay->fp) != size || fflush(overlay->fp) != 0)
        return 0;
    overlay->entries[i].offset = off;
    return 1;
}

static scsi_overlay_t *overlay_open(const char *path) {
    scsi_overlay_t *overlay = calloc(1, sizeof(*overlay));
    if (!overlay)
        return NULL;
    overlay->path = strdup(path);
    overlay->fp = overlay->path ? fopen(overlay->path, "r+b") : NULL;
    if (!overlay->fp)
        overlay->fp = overlay->path ? fopen(overlay->path, "w+b") : NULL;
    if (!overlay->fp)
        goto fail;
    if (fseek(overlay->fp, 0, SEEK_END) != 0)
        goto fail;
    long length = ftell(overlay->fp);
    if (length < 0)
        goto fail;
    if (length == 0) {
        if (fwrite(s_overlay_magic, 1, sizeof(s_overlay_magic), overlay->fp) !=
                sizeof(s_overlay_magic) ||
            fflush(overlay->fp) != 0)
            goto fail;
        return overlay;
    }
    uint8_t magic[sizeof(s_overlay_magic)];
    if (fseek(overlay->fp, 0, SEEK_SET) != 0 ||
        fread(magic, 1, sizeof(magic), overlay->fp) != sizeof(magic) ||
        memcmp(magic, s_overlay_magic, sizeof(magic)) != 0)
        goto fail;
    for (;;) {
        scsi_overlay_record_t record;
        size_t read = fread(&record, 1, sizeof(record), overlay->fp);
        if (read == 0 && feof(overlay->fp))
            break;
        if (read != sizeof(record) || record.size == 0)
            goto fail;
        long offset = ftell(overlay->fp);
        if (offset < 0 || fseek(overlay->fp, (long)record.size, SEEK_CUR) != 0)
            goto fail;
        int entry = overlay_find(overlay, record.lba);
        if (entry < 0) {
            if (!overlay_reserve(overlay))
                goto fail;
            entry = (int)overlay->count++;
            overlay->entries[entry].lba = record.lba;
        }
        overlay->entries[entry].offset = offset;
    }
    clearerr(overlay->fp);
    return overlay;
fail:
    scsi_overlay_destroy(overlay);
    return NULL;
}

void scsi_overlay_destroy(scsi_overlay_t *overlay) {
    if (!overlay)
        return;
    if (overlay->fp)
        fclose(overlay->fp);
    free(overlay->path);
    free(overlay->entries);
    free(overlay);
}
scsi_overlay_t *scsi_overlay_clone(const scsi_overlay_t *overlay) {
    if (!overlay)
        return NULL;
    scsi_overlay_t *copy = calloc(1, sizeof(*copy));
    if (!copy)
        return NULL;
    copy->path = strdup(overlay->path);
    copy->fp = copy->path ? fopen(copy->path, "r+b") : NULL;
    if (!copy->fp) {
        scsi_overlay_destroy(copy);
        return NULL;
    }
    if (overlay->count) {
        copy->entries = malloc(overlay->count * sizeof(*copy->entries));
        if (!copy->entries) {
            scsi_overlay_destroy(copy);
            return NULL;
        }
        memcpy(copy->entries, overlay->entries, overlay->count * sizeof(*copy->entries));
    }
    copy->count = copy->capacity = overlay->count;
    return copy;
}
int scsi_enable_overlay(scsi_t *spc, int id, const char *path) {
    if (!spc || id < 0 || id >= SCSI_NUM_UNITS || !path)
        return -1;
    scsi_unit_t *unit = &spc->units[id];
    if (!unit->fp || unit->kind != SCSI_UNIT_DISK || unit->removable)
        return -1;
    scsi_overlay_t *overlay = overlay_open(path);
    if (!overlay)
        return -1;
    scsi_overlay_destroy(unit->overlay);
    unit->overlay = overlay;
    unit->write_protected = 0;
    return 0;
}

/* レジスタオフセット（addr&31）。XEiJ SPC.java 準拠。 */
enum {
    SPC_OFF_BDID = 0x01,
    SPC_OFF_SCTL = 0x03,
    SPC_OFF_SCMD = 0x05,
    SPC_OFF_INTS = 0x09,
    SPC_OFF_PSNS = 0x0B, /* Read */
    SPC_OFF_SDGC = 0x0B, /* Write (同一オフセット) */
    SPC_OFF_SSTS = 0x0D,
    SPC_OFF_SERR = 0x0F,
    SPC_OFF_PCTL = 0x11,
    SPC_OFF_MBC = 0x13,
    SPC_OFF_DREG = 0x15,
    SPC_OFF_TEMP = 0x17,
    SPC_OFF_TCH = 0x19,
    SPC_OFF_TCM = 0x1B,
    SPC_OFF_TCL = 0x1D,
};

#define SPC_BDID_I7 0x80u
#define SPC_SCTL_RD 0x80u /* Reset & Disable: リセット直後は常にセットされている */
#define SPC_SCTL_IE 0x01u

#define SPC_SCMD_CC_MASK 0xE0u
#define SPC_SCMD_CC_BR   0x00u /* Bus Release */
#define SPC_SCMD_CC_SL   0x20u /* Select */
#define SPC_SCMD_CC_RA   0x40u /* Reset ATN */
#define SPC_SCMD_CC_SA   0x60u /* Set ATN */
#define SPC_SCMD_CC_TR   0x80u /* Transfer */
#define SPC_SCMD_CC_TP   0xA0u /* Transfer Pause */
#define SPC_SCMD_CC_RR   0xC0u /* Reset ACK/REQ */
#define SPC_SCMD_CC_SR   0xE0u /* Set ACK/REQ */

#define SPC_INTS_SL 0x80u
#define SPC_INTS_RS 0x40u
#define SPC_INTS_DC 0x20u
#define SPC_INTS_CC 0x10u
#define SPC_INTS_SR 0x08u
#define SPC_INTS_TO 0x04u
#define SPC_INTS_HE 0x02u
#define SPC_INTS_RC 0x01u

#define SPC_PSNS_REQ        0x80u
#define SPC_PSNS_ACK        0x40u
#define SPC_PSNS_ATN        0x20u
#define SPC_PSNS_SEL        0x10u
#define SPC_PSNS_BSY        0x08u
#define SPC_PSNS_PHASE_MASK 0x07u
#define SPC_PSNS_IO         0x01u /* フェーズコードbit0と共用 */

#define SPC_PHASE_DATA_OUT 0x00u
#define SPC_PHASE_DATA_IN  0x01u
#define SPC_PHASE_COMMAND  0x02u
#define SPC_PHASE_STATUS   0x03u
#define SPC_PHASE_MSG_OUT  0x06u
#define SPC_PHASE_MSG_IN   0x07u

#define SPC_SSTS_INIT 0x80u
#define SPC_SSTS_TARG 0x40u
#define SPC_SSTS_BUSY 0x20u
#define SPC_SSTS_TRIP 0x10u
#define SPC_SSTS_TC0  0x04u
#define SPC_SSTS_DF   0x02u
#define SPC_SSTS_DE   0x01u

#define SPC_PCTL_IE 0x80u /* Busfree INT Enable */
#define SPC_PCTL_IO 0x01u /* フェーズコードbit0と共用 */

#define SPC_STATUS_GOOD            0x00u
#define SPC_STATUS_CHECK_CONDITION 0x02u
#define SPC_MSG_COMMAND_COMPLETE   0x00u

/* センスデータ（SCSI-2固定長）。[0]=レスポンスコード、[2]=センスキー、
 * [7]=追加センス長、[12]=ASC、[13]=ASCQ。 */
#define SPC_SENSE_RESPONSE_CODE     0x70u /* 現在のエラー、拡張センスフォーマット */
#define SPC_SENSE_OFF_RESPONSE      0
#define SPC_SENSE_OFF_KEY           2
#define SPC_SENSE_OFF_ADD_LENGTH    7
#define SPC_SENSE_OFF_ASC           12
#define SPC_SENSE_OFF_ASCQ          13
#define SPC_SENSE_ADDITIONAL_LENGTH (SCSI_SENSE_LENGTH - 8)

#define SPC_SENSE_KEY_NOT_READY       0x02u
#define SPC_SENSE_KEY_MEDIUM_ERROR    0x03u
#define SPC_SENSE_KEY_ILLEGAL_REQUEST 0x05u
#define SPC_SENSE_KEY_UNIT_ATTENTION  0x06u
#define SPC_SENSE_KEY_DATA_PROTECT    0x07u

#define SPC_ASC_NONE                 0x00u
#define SPC_ASC_WRITE_ERROR          0x0Cu
#define SPC_ASC_INVALID_COMMAND      0x20u /* Invalid Command Operation Code */
#define SPC_ASC_INVALID_FIELD_IN_CDB 0x24u
#define SPC_ASC_LUN_NOT_SUPPORTED    0x25u
#define SPC_ASC_WRITE_PROTECTED      0x27u
#define SPC_ASC_MEDIUM_CHANGED       0x28u /* Not Ready To Ready Transition */
#define SPC_ASC_MEDIUM_NOT_PRESENT   0x3Au
#define SPC_ASCQ_NONE                0x00u

/* 6-byte CDBのbyte 1 bit 7-5に置かれる論理ユニット番号。NetBSDは
 * SCSI-2互換ターゲットに対してこのフィールドにもLUNを設定する。 */
#define SCSI_CDB_LUN_SHIFT 5u

/* CDB長テーブル（opcode上位4bit=グループ番号でインデックス、XEiJ SPC_CDB_LENGTH準拠） */
static const int SPC_CDB_LENGTH[16] = {6, 6, 10, 10, 10, 10, 1, 1, 16, 16, 12, 12, 6, 12, 1, 1};

static void spc_set_sense(scsi_t *spc, uint8_t key, uint8_t asc, uint8_t ascq);

static void spc_update_ssts(scsi_t *spc) {
    int remaining = spc->buf_limit - spc->buf_index;

    spc->ssts &= (uint8_t)~(SPC_SSTS_TC0 | SPC_SSTS_DF | SPC_SSTS_DE);
    if ((spc->psns & SPC_PSNS_IO) == 0) { /* Out: 出力はFIFO常にEmpty */
        if (spc->tc == 0)
            spc->ssts |= SPC_SSTS_TC0;
        spc->ssts |= SPC_SSTS_DE;
    } else { /* In */
        /* DF/DEはTCではなく、実際にDREGへ供給できる残量を表す。短転送で
         * DATA INを終えた後にDFを残すと、イニシエータが古いDREG値をさらに
         * 読み、STATUS/MSG INまで消費してしまう。転送停止中もDREGはEmpty。 */
        if (spc->tc == 0)
            spc->ssts |= (uint8_t)(SPC_SSTS_TC0 | SPC_SSTS_DE);
        else if ((spc->ssts & SPC_SSTS_TRIP) == 0 || remaining == 0)
            spc->ssts |= SPC_SSTS_DE;
        else if (remaining >= 8)
            spc->ssts |= SPC_SSTS_DF;
    }
}

/* イニシエータ→ターゲット方向(Out)の1バイトを受け取る共通処理。
 * COMMANDフェーズはCDB長が先頭バイトのopcodeで決まるため、buf_limitは1として
 * 開始し、先頭バイト受領時にSPC_CDB_LENGTHで確定させる。CPU 1バイト転送
 * (SCMD.CC_SR)とプログラム転送/DMAC(DREG書き込み)の双方から呼ぶ。片方だけに
 * 長さ確定処理を置くと、もう一方の経路でCDBの2バイト目以降がbuf_limitガードに
 * 弾かれ、コマンドが完成せず完了通知(INTS.CC)に到達しない。 */
static void spc_store_out_byte(scsi_t *spc, uint8_t val) {
    spc->write_handle[spc->buf_index++] = val;
    if ((spc->psns & SPC_PSNS_PHASE_MASK) == SPC_PHASE_COMMAND && spc->buf_index == 1) {
        int oc = spc->cmd_buf[0];
        spc->buf_limit = SPC_CDB_LENGTH[(oc >> 4) & 0xF];
    }
}

static void spc_set_interrupt(scsi_t *spc, uint8_t bits) {
    if (bits & (uint8_t)~SPC_INTS_TO) /* TO以外をセットするときはTOをクリア(XEiJ準拠) */
        spc->ints &= (uint8_t)~SPC_INTS_TO;
    spc->ints |= bits;
    if ((spc->sctl & SPC_SCTL_IE) && spc->bus)
        x68k_bus_scsi_irq_raise(spc->bus);
}

static void spc_command_phase(scsi_t *spc) {
    spc->write_handle = spc->cmd_buf;
    spc->read_handle = NULL;
    spc->buf_index = 0;
    spc->buf_limit = 1;
    spc->psns = (uint8_t)(SPC_PSNS_REQ | SPC_PHASE_COMMAND);
    spc_update_ssts(spc);
}

static void spc_data_in_phase(scsi_t *spc, const uint8_t *handle, int len) {
    spc->write_handle = NULL;
    spc->read_handle = handle;
    spc->buf_index = 0;
    spc->buf_limit = len;
    spc->psns = (uint8_t)(SPC_PSNS_REQ | SPC_PHASE_DATA_IN);
    spc_update_ssts(spc);
}

/* Mode Select(6)等、パラメータリストを受け取るだけで内容を解釈しないno-opコマンド用。
 * data_bufへlenバイト受け取った後はDATA_OUTフェーズ完了(spc_transfer_complete)で
 * 自動的にGOODステータスへ遷移する（既存のDATA_OUT/DATA_IN共通ハンドリング）。 */
static void spc_data_out_phase(scsi_t *spc, uint8_t *handle, int len) {
    spc->write_handle = handle;
    spc->read_handle = NULL;
    spc->buf_index = 0;
    spc->buf_limit = len;
    spc->psns = (uint8_t)(SPC_PSNS_REQ | SPC_PHASE_DATA_OUT);
    spc_update_ssts(spc);
}

static void spc_status_phase(scsi_t *spc, uint8_t status, uint8_t message) {
    spc->status_buf[0] = status;
    spc->msgin_buf[0] = message;
    spc->write_handle = NULL;
    spc->read_handle = spc->status_buf;
    spc->buf_index = 0;
    spc->buf_limit = 1;
    spc->temp = spc->status_buf[0];
    spc->psns = (uint8_t)(SPC_PSNS_REQ | SPC_PHASE_STATUS);
    spc_update_ssts(spc);
}

static void spc_message_out_phase(scsi_t *spc) {
    spc->write_handle = spc->msgout_buf;
    spc->read_handle = NULL;
    spc->buf_index = 0;
    spc->buf_limit = 1;
    spc->psns = (uint8_t)(SPC_PSNS_REQ | SPC_PHASE_MSG_OUT);
    spc_update_ssts(spc);
}

static void spc_message_in_phase(scsi_t *spc) {
    spc->write_handle = NULL;
    spc->read_handle = spc->msgin_buf;
    spc->buf_index = 0;
    spc->buf_limit = 1;
    spc->temp = spc->msgin_buf[0];
    spc->psns = (uint8_t)(SPC_PSNS_REQ | SPC_PHASE_MSG_IN);
    spc_update_ssts(spc);
}

static void spc_bus_free_phase(scsi_t *spc) {
    spc->write_handle = NULL;
    spc->read_handle = NULL;
    spc->ssts &= (uint8_t)~SPC_SSTS_INIT;
    spc->psns = 0;
    spc->target_id = -1;
    if (spc->pctl & SPC_PCTL_IE)
        spc_set_interrupt(spc, SPC_INTS_DC);
}

/* lba起点でnblocks分のユーザデータをunitのイメージからdstへ読み込む単一の分岐点
 * （docs/ix68k-internals/design/scsi_design.md §4）。
 * DISK : bytes_per_record単位の連続領域を一括freadする(従来ロジック)。
 * CDROM: 1セクタごとにcd_raw_sector_size(2352B)単位でシークし、
 *        cd_user_data_offset(Mode1=16B)からbytes_per_record(2048B)だけ抜き出す。
 * 戻り値: 1=成功, 0=失敗(I/Oエラー)。呼び出し元が範囲・バッファ長を検証済みの前提。 */
static int scsi_unit_read(scsi_unit_t *unit, uint32_t lba, uint32_t nblocks, uint8_t *dst) {
    if (unit->kind == SCSI_UNIT_CDROM) {
        for (uint32_t i = 0; i < nblocks; i++) {
            long off = (long)((uint64_t)(lba + i) * unit->cd_raw_sector_size +
                              unit->cd_user_data_offset);
            if (fseek(unit->fp, off, SEEK_SET) != 0)
                return 0;
            if (fread(dst + (size_t)i * unit->bytes_per_record, 1, unit->bytes_per_record,
                      unit->fp) != unit->bytes_per_record)
                return 0;
        }
        return 1;
    }
    long offset = (long)((uint64_t)lba * unit->bytes_per_record);
    if (fseek(unit->fp, offset, SEEK_SET) != 0)
        return 0;
    uint64_t total = (uint64_t)nblocks * unit->bytes_per_record;
    if (fread(dst, 1, (size_t)total, unit->fp) != (size_t)total)
        return 0;
    if (unit->overlay)
        for (uint32_t i = 0; i < nblocks; i++)
            if (!overlay_read(unit->overlay, lba + i, dst + (size_t)i * unit->bytes_per_record,
                              unit->bytes_per_record))
                return 0;
    return 1;
}

/* Read(6)/Read(10)共通処理: lba起点でnblocks分をunitのイメージからxfer_bufへ読み込み、
 * Data-Inフェーズへ遷移する。範囲外/バッファ超過なら0を返しCHECK CONDITIONにする。 */
static int spc_read_blocks(scsi_t *spc, scsi_unit_t *unit, uint32_t lba, uint32_t nblocks) {
    if (nblocks == 0 || lba + nblocks > unit->disk_end_record)
        return 0;
    uint64_t total = (uint64_t)nblocks * unit->bytes_per_record;
    if (total > SCSI_XFER_BUF_SIZE)
        return 0;
    if (!scsi_unit_read(unit, lba, nblocks, spc->xfer_buf))
        return 0;
    spc_data_in_phase(spc, spc->xfer_buf, (int)total);
    return 1;
}

/* Write(6)/Write(10)共通処理: lba/nblocksを保留し、xfer_bufへのData-Outフェーズへ
 * 遷移する。実際のfwriteはDATA_OUT完了(spc_transfer_complete)で行う。 */
static int spc_write_blocks(scsi_t *spc, scsi_unit_t *unit, uint32_t lba, uint32_t nblocks) {
    if (nblocks == 0 || lba + nblocks > unit->disk_end_record)
        return 0;
    uint64_t total = (uint64_t)nblocks * unit->bytes_per_record;
    if (total > SCSI_XFER_BUF_SIZE)
        return 0;
    spc->pending_write = 1;
    spc->write_lba = lba;
    spc->write_unit = unit;
    spc_data_out_phase(spc, spc->xfer_buf, (int)total);
    return 1;
}

/* CHECK CONDITIONで返すセンスデータを組み立てる。イニシエータはASC/ASCQまで見て
 * エラーを分類する（NetBSDのscsipi_interpret_sense()等）ため、キーだけでなく
 * ASC/ASCQも必ず設定する。前回のセンスが残らないよう全体をクリアしてから書く。 */
static void spc_set_sense(scsi_t *spc, uint8_t key, uint8_t asc, uint8_t ascq) {
    memset(spc->sense_buf, 0, sizeof(spc->sense_buf));
    spc->sense_buf[SPC_SENSE_OFF_RESPONSE] = SPC_SENSE_RESPONSE_CODE;
    spc->sense_buf[SPC_SENSE_OFF_KEY] = key;
    spc->sense_buf[SPC_SENSE_OFF_ADD_LENGTH] = SPC_SENSE_ADDITIONAL_LENGTH;
    spc->sense_buf[SPC_SENSE_OFF_ASC] = asc;
    spc->sense_buf[SPC_SENSE_OFF_ASCQ] = ascq;
}

/* write_protectedなユニット(CD-ROM等)への書き込み系コマンドをDATA PROTECTで拒否する
 * （docs/ix68k-internals/design/scsi_design.md §7）。 */
static void spc_reject_write_protected(scsi_t *spc) {
    spc_set_sense(spc, SPC_SENSE_KEY_DATA_PROTECT, SPC_ASC_WRITE_PROTECTED, SPC_ASCQ_NONE);
    spc_status_phase(spc, SPC_STATUS_CHECK_CONDITION, SPC_MSG_COMMAND_COMPLETE);
}

/* READ TOCの1ディスクリプタ(8バイト)を組み立てる
 * （docs/ix68k-internals/design/scsi_design.md §6）。
 * msf=0: [+4..7]=開始LBA(ビッグエンディアン32bit)。
 * msf=1: [+4]=0、[+5..7]=M/S/F(バイナリ、BCDではない)。 */
static void scsi_write_toc_descriptor(uint8_t *d, uint8_t track_no, uint8_t adr_control,
                                      uint32_t lba, int msf) {
    d[0] = 0;
    d[1] = adr_control;
    d[2] = track_no;
    d[3] = 0;
    if (msf) {
        uint32_t total = lba + 150;
        d[4] = 0;
        d[5] = (uint8_t)(total / 4500);
        d[6] = (uint8_t)((total % 4500) / 75);
        d[7] = (uint8_t)(total % 75);
    } else {
        d[4] = (uint8_t)(lba >> 24);
        d[5] = (uint8_t)(lba >> 16);
        d[6] = (uint8_t)(lba >> 8);
        d[7] = (uint8_t)lba;
    }
}

/* ILLEGAL REQUESTでCHECK CONDITIONを返す。ascには不正の種別
 * （不正なopcodeなら SPC_ASC_INVALID_COMMAND、CDBのフィールド値なら
 * SPC_ASC_INVALID_FIELD_IN_CDB 等）を渡す。 */
static void spc_illegal_request(scsi_t *spc, uint8_t asc) {
    spc_set_sense(spc, SPC_SENSE_KEY_ILLEGAL_REQUEST, asc, SPC_ASCQ_NONE);
    spc_status_phase(spc, SPC_STATUS_CHECK_CONDITION, SPC_MSG_COMMAND_COMPLETE);
}

/* REQUEST SENSEは直前のCHECK CONDITIONと同じLUNで発行されるため、
 * LUN対応の可否より先に常に処理する。現在のSPCはSCSI_SENSE_LENGTHバイトの
 * 固定長senseを実装しているので、Allocation Lengthもここで同じ長さへ制限する。 */
static void spc_request_sense(scsi_t *spc) {
    int alloc = spc->cmd_buf[4] & 0xFF;
    int n = alloc < (int)sizeof(spc->sense_buf) ? alloc : (int)sizeof(spc->sense_buf);
    memcpy(spc->data_buf, spc->sense_buf, sizeof(spc->sense_buf));
    memset(spc->sense_buf, 0, sizeof(spc->sense_buf));
    spc_data_in_phase(spc, spc->data_buf, n);
}

/* 未実装LUNのINQUIRYはSCSIのperipheral qualifier/typeで「LUN未対応」を
 * 報告する。これによりNetBSDは同一SCSI ID上のLUN 1-7をディスクとして
 * attachせず、以後の通常コマンドを発行しない。 */
static void spc_unsupported_lun_inquiry(scsi_t *spc) {
    int alloc = spc->cmd_buf[4] & 0xFF;
    int len = alloc < SCSI_DATA_BUF_SIZE ? alloc : SCSI_DATA_BUF_SIZE;
    memset(spc->data_buf, 0, (size_t)len);
    if (len > 0)
        spc->data_buf[0] = 0x7Fu; /* PQ=3(未対応)、PDT=0x1f(デバイスなし) */
    spc_data_in_phase(spc, spc->data_buf, len);
}

/* Test Unit Ready(0x00)/Rezero Unit(0x01)/Request Sense(0x03)/Format Unit(0x04)/
 * Seek(6)(0x0B)/Inquiry(0x12)/Read(6)(0x08)/Write(6)(0x0A)/Mode Select(6)(0x15)/Mode
 * Sense(6)(0x1A)/ Start-Stop Unit(0x1B)/Prevent-Allow Medium Removal(0x1E)/Read Capacity(0x25)/
 * Read(10)(0x28)/Write(10)(0x2A)/READ TOC(0x43、CD-ROMのみ)を実装。
 * 未実装はCHECK CONDITIONで応答し、ROM/ドライバが継続判断できるようにする（ハング防止）。 */
static void spc_execute_command(scsi_t *spc) {
    int oc = spc->cmd_buf[0];
    int lun = (spc->cmd_buf[1] >> SCSI_CDB_LUN_SHIFT) & 0x07;
    scsi_unit_t *unit = (spc->target_id >= 0) ? &spc->units[spc->target_id] : NULL;

    /* REQUEST SENSEは非ゼロLUNを拒否した直後にも同じLUNで送られる。
     * ここでLUN判定より先に返さないとCHECK CONDITIONが自己再帰し、
     * イニシエータは失敗理由を取得できない。 */
    if (oc == 0x03) {
        spc_request_sense(spc);
        return;
    }

    /* SPCはtarget_idごとにLUN 0だけを実装する。従来はCDBのLUNビットを
     * 無視していたため、NetBSDがLUN 1-7へ行うINQUIRYをLUN 0のディスク
     * として成功させ、偽sdデバイスを列挙していた。 */
    if (lun != 0) {
        if (oc == 0x12)
            spc_unsupported_lun_inquiry(spc);
        else
            spc_illegal_request(spc, SPC_ASC_LUN_NOT_SUPPORTED);
        return;
    }
    /* リムーバブルメディア(MO/CD-ROM等)の状態変化(挿入・排出いずれも)を、
     * attention_pendingが立っている間はRequest Sense/Inquiry以外の最初の
     * コマンドでUNIT ATTENTIONとして一度だけ報告する。SUSIE.X等はTest Unit
     * Readyでこれを検知しメディア交換・排出を判定するため、Test Unit Ready
     * 自身も対象に含める必要がある（下のexemptロジックより優先。
     * disk windowのEJECTボタンがホストに反映されなかったバグ）。固定ディスク
     * (HDD起動経路)には影響させないためremovableのみを対象にする。 */
    if (unit && unit->removable && unit->attention_pending && oc != 0x03 && oc != 0x12) {
        spc_set_sense(spc, SPC_SENSE_KEY_UNIT_ATTENTION, SPC_ASC_MEDIUM_CHANGED, SPC_ASCQ_NONE);
        unit->attention_pending = 0;
        spc_status_phase(spc, SPC_STATUS_CHECK_CONDITION, SPC_MSG_COMMAND_COMPLETE);
        return;
    }
    /* メディアが無ければCHECK CONDITIONを返す。ただしRequest SenseとInquiryは
     * メディア無しでも必要で、Test Unit Ready、Rezero Unit、Start-Stop Unitは
     * XEiJ準拠でGOODを返す。attention_pendingは上のガードで既に処理済みなので
     * ここでは常にNOT READYを返す。 */
    if ((!unit || !unit->fp) && oc != 0x03 && oc != 0x12 && oc != 0x00 && oc != 0x01 &&
        oc != 0x1B) {
        /* NOT READY(メディア未挿入が継続中) */
        spc_set_sense(spc, SPC_SENSE_KEY_NOT_READY, SPC_ASC_MEDIUM_NOT_PRESENT, SPC_ASCQ_NONE);
        spc_status_phase(spc, SPC_STATUS_CHECK_CONDITION, SPC_MSG_COMMAND_COMPLETE);
        return;
    }
    switch (oc) {
    case 0x00: /* Test Unit Ready */
        spc_status_phase(spc, SPC_STATUS_GOOD, SPC_MSG_COMMAND_COMPLETE);
        break;
    case 0x01: /* Rezero Unit: シーク初期化、実質no-op(XEiJ準拠) */
        spc_status_phase(spc, SPC_STATUS_GOOD, SPC_MSG_COMMAND_COMPLETE);
        break;
    case 0x0B: { /* Seek(6): LBA=CDB[1]下位5bit:CDB[2]:CDB[3]。IPLROM
                  * _B_SEEK($FF8B00)のSCSI/SASI側がこのCDB(先頭バイト0x0B、
                  * ROMバイナリ$FF9D16実測)を発行する。実データ移動を
                  * 伴わないため、範囲内ならXEiJ(scuDoSeek6)準拠でGOODのみ返す
                  * no-op。範囲外はXEiJ同様ILLEGAL REQUESTとする（Rezero Unitと
                  * 異なりLBAを持つため、無条件no-opにはしない）。 */
        uint32_t lba = ((uint32_t)(spc->cmd_buf[1] & 0x1Fu) << 16) |
                       ((uint32_t)spc->cmd_buf[2] << 8) | spc->cmd_buf[3];
        if (lba > unit->disk_end_record) {
            spc_illegal_request(spc, SPC_ASC_INVALID_FIELD_IN_CDB);
        } else {
            spc_status_phase(spc, SPC_STATUS_GOOD, SPC_MSG_COMMAND_COMPLETE);
        }
        break;
    }
    case 0x04: /* Format Unit: Human68k FORMAT.Xは後続処理で管理領域を書き直すため、
                * X68SCSI1ヘッダを壊さないno-opとする。FmtData=0のみを想定。
                * write_protectedなユニット(CD-ROM)はDATA PROTECTで拒否する。 */
        if (unit->write_protected)
            spc_reject_write_protected(spc);
        else
            spc_status_phase(spc, SPC_STATUS_GOOD, SPC_MSG_COMMAND_COMPLETE);
        break;
    case 0x12: { /* Inquiry */
        uint8_t *d = spc->data_buf;
        /* 内蔵SCSI ROMの_S_INQUIRYは呼び出し側のAllocation Lengthをそのまま
         * DMAC転送カウントへ設定する。標準応答36バイトだけでDATA INを終えると、
         * CDROM.Xのalloc=92ではDMACカウントが残ってIOCSが完了待ちになるため、
         * Mode Senseと同様にAllocation Lengthまでゼロ埋めして返す。 */
        int alloc = spc->cmd_buf[4] & 0xFF;
        int len = alloc < SCSI_DATA_BUF_SIZE ? alloc : SCSI_DATA_BUF_SIZE;
        memset(d, 0, (size_t)len);
        /* peripheral device type: CD-ROM=0x05、それ以外(HDD/MO)は従来通り0x00
         * （ダイレクトアクセスデバイス）。SUSIE.Xはbuf[0]&0x1F==0x05を要求する
         * （docs/ix68k-internals/design/scsi_design.md §6）。 */
        d[0] = (unit->kind == SCSI_UNIT_CDROM) ? 0x05u : 0x00u;
        d[1] =
            unit->removable ? 0x80u : 0x00u; /* RMBビット: リムーバブル(MO/CD-ROM等)か固定か */
        d[2] = 2;                            /* SCSI-2 */
        d[3] = 2;                            /* レスポンス形式 SCSI-2 */
        d[4] = 31;                           /* 追加データ長 */
        /* ベンダIDは種別に関わらず変えない。extern/CDROM/CDROM.Xの東芝機種判定
         * ('TOSHIBA CD-ROM XM-')に引っかからないようにするため（同§9-9）。 */
        memcpy(d + 8, "X68000  ", 8);
        if (unit->kind == SCSI_UNIT_CDROM)
            memcpy(d + 16, "Emulated CD-ROM ", 16);
        else
            memcpy(d + 16, "Emulated Disk   ", 16);
        memcpy(d + 32, "1.0 ", 4);
        spc_data_in_phase(spc, spc->data_buf, len);
        break;
    }
    case 0x08: { /* Read(6): LBA=CDB[1]下位5bit:CDB[2]:CDB[3]、n=CDB[4](0=256) */
        uint32_t lba = ((uint32_t)(spc->cmd_buf[1] & 0x1Fu) << 16) |
                       ((uint32_t)spc->cmd_buf[2] << 8) | spc->cmd_buf[3];
        uint32_t n = spc->cmd_buf[4];
        if (n == 0)
            n = 256;
        if (!spc_read_blocks(spc, unit, lba, n))
            spc_status_phase(spc, SPC_STATUS_CHECK_CONDITION, SPC_MSG_COMMAND_COMPLETE);
        break;
    }
    case 0x28: { /* Read(10): LBA=CDB[2..5]、n=CDB[7]:CDB[8] */
        uint32_t lba = ((uint32_t)spc->cmd_buf[2] << 24) | ((uint32_t)spc->cmd_buf[3] << 16) |
                       ((uint32_t)spc->cmd_buf[4] << 8) | spc->cmd_buf[5];
        uint32_t n = ((uint32_t)spc->cmd_buf[7] << 8) | spc->cmd_buf[8];
        if (!spc_read_blocks(spc, unit, lba, n))
            spc_status_phase(spc, SPC_STATUS_CHECK_CONDITION, SPC_MSG_COMMAND_COMPLETE);
        break;
    }
    case 0x0A: { /* Write(6): LBA=CDB[1]下位5bit:CDB[2]:CDB[3]、n=CDB[4](0=256) */
        if (unit->write_protected) {
            spc_reject_write_protected(spc);
            break;
        }
        uint32_t lba = ((uint32_t)(spc->cmd_buf[1] & 0x1Fu) << 16) |
                       ((uint32_t)spc->cmd_buf[2] << 8) | spc->cmd_buf[3];
        uint32_t n = spc->cmd_buf[4];
        if (n == 0)
            n = 256;
        if (!spc_write_blocks(spc, unit, lba, n))
            spc_status_phase(spc, SPC_STATUS_CHECK_CONDITION, SPC_MSG_COMMAND_COMPLETE);
        break;
    }
    case 0x2A: { /* Write(10): LBA=CDB[2..5]、n=CDB[7]:CDB[8] */
        if (unit->write_protected) {
            spc_reject_write_protected(spc);
            break;
        }
        uint32_t lba = ((uint32_t)spc->cmd_buf[2] << 24) | ((uint32_t)spc->cmd_buf[3] << 16) |
                       ((uint32_t)spc->cmd_buf[4] << 8) | spc->cmd_buf[5];
        uint32_t n = ((uint32_t)spc->cmd_buf[7] << 8) | spc->cmd_buf[8];
        if (!spc_write_blocks(spc, unit, lba, n))
            spc_status_phase(spc, SPC_STATUS_CHECK_CONDITION, SPC_MSG_COMMAND_COMPLETE);
        break;
    }
    case 0x15: { /* Mode Select(6): パラメータリストを受け取るのみ(内容は解釈しない) */
        int n = spc->cmd_buf[4] & 0xFF;
        spc_data_out_phase(spc, spc->data_buf, n);
        break;
    }
    case 0x1A: { /* Mode Sense(6): ヘッダ+ブロックディスクリプタ(XEiJ scuDoModeSense6準拠) */
        int page = spc->cmd_buf[2] & 0x3F;
        if (page != 0x00 && page != 0x3F) {
            /* 未対応のページコード。センスを設定せずCHECK CONDITIONだけ返すと、
             * イニシエータの続くRequest Senseが全ゼロのセンスを受け取り「不正な
             * センスフォーマット」として扱われる（NetBSDはEINVAL）。 */
            spc_illegal_request(spc, SPC_ASC_INVALID_FIELD_IN_CDB);
            break;
        }
        /* ROMはAllocation Length分の転送を待つため、12バイト以降をゼロで埋めて
         * tcとbuf_limitを一致させる。 */
        int alloc = spc->cmd_buf[4] & 0xFF;
        int len = alloc < SCSI_DATA_BUF_SIZE ? alloc : SCSI_DATA_BUF_SIZE;
        uint8_t *d = spc->data_buf;
        memset(d, 0, (size_t)len);
        if (len > 0)
            d[0] = 12 - 1; /* センスデータ長 */
        if (len > 2 && unit->write_protected)
            d[2] |= 0x80u; /* WPビット(device-specific parameter bit7)。CD-ROMは常に1。
                            * SUSIE.Xがここだけ参照する
                            * （docs/ix68k-internals/design/scsi_design.md §7）。 */
        if (len > 3)
            d[3] = 8; /* ブロックディスクリプタ長 */
        if (len > 7) {
            d[5] = (uint8_t)(unit->disk_end_record >> 16);
            d[6] = (uint8_t)(unit->disk_end_record >> 8);
            d[7] = (uint8_t)unit->disk_end_record;
        }
        if (len > 11) {
            d[9] = (uint8_t)(unit->bytes_per_record >> 16);
            d[10] = (uint8_t)(unit->bytes_per_record >> 8);
            d[11] = (uint8_t)unit->bytes_per_record;
        }
        spc_data_in_phase(spc, spc->data_buf, len);
        break;
    }
    case 0x1B: /* Start-Stop Unit: イジェクト制御は非対応、常にGOOD */
        spc_status_phase(spc, SPC_STATUS_GOOD, SPC_MSG_COMMAND_COMPLETE);
        break;
    case 0x1E: /* Prevent-Allow Medium Removal: 何もしない */
        spc_status_phase(spc, SPC_STATUS_GOOD, SPC_MSG_COMMAND_COMPLETE);
        break;
    case 0x25: { /* Read Capacity: 最終論理ブロックアドレス + ブロック長 */
        uint32_t last_lba = unit->disk_end_record > 0 ? unit->disk_end_record - 1 : 0;
        spc->data_buf[0] = (uint8_t)(last_lba >> 24);
        spc->data_buf[1] = (uint8_t)(last_lba >> 16);
        spc->data_buf[2] = (uint8_t)(last_lba >> 8);
        spc->data_buf[3] = (uint8_t)last_lba;
        spc->data_buf[4] = (uint8_t)(unit->bytes_per_record >> 24);
        spc->data_buf[5] = (uint8_t)(unit->bytes_per_record >> 16);
        spc->data_buf[6] = (uint8_t)(unit->bytes_per_record >> 8);
        spc->data_buf[7] = (uint8_t)unit->bytes_per_record;
        spc_data_in_phase(spc, spc->data_buf, 8);
        break;
    }
    case 0x43: { /* READ TOC(CD-ROMのみ)。CDB = 43 (MSF<<1) 00 00 00 00 <開始トラック>
                  * <alloc_hi> <alloc_lo> 00（scsi_design.md §6） */
        if (unit->kind != SCSI_UNIT_CDROM) {
            spc_illegal_request(spc, SPC_ASC_INVALID_COMMAND);
            break;
        }
        int msf = (spc->cmd_buf[1] >> 1) & 1;
        int start_track = spc->cmd_buf[6];
        int alloc = ((int)spc->cmd_buf[7] << 8) | spc->cmd_buf[8];

        /* 返すディスクリプタ集合を決める: start_track==0はトラック1から、0xAAなら
         * リードアウトのみ、それ以外はstart_track以降+リードアウト。範囲外はエラー。 */
        int first_index;
        if (start_track == SCSI_CD_LEADOUT_TRACK) {
            first_index = unit->cd_track_count;
        } else {
            int effective_start = (start_track == 0) ? unit->cd_first_track : start_track;
            if (effective_start < unit->cd_first_track ||
                effective_start > unit->cd_last_track) {
                spc_illegal_request(spc, SPC_ASC_INVALID_FIELD_IN_CDB);
                break;
            }
            first_index = 0;
            while (first_index < unit->cd_track_count &&
                   unit->cd_tracks[first_index].track_no < effective_start)
                first_index++;
        }
        int descriptor_count = (unit->cd_track_count - first_index) + 1; /* +1=リードアウト */
        int response_len = 4 + descriptor_count * 8; /* 理論最大: 4+8*(99+1)=804 */
        uint8_t *d = spc->data_buf;
        memset(d, 0, (size_t)response_len);
        d[2] = unit->cd_first_track;
        d[3] = unit->cd_last_track;
        int off = 4;
        for (int i = first_index; i < unit->cd_track_count; i++) {
            scsi_write_toc_descriptor(d + off, unit->cd_tracks[i].track_no,
                                      unit->cd_tracks[i].adr_control,
                                      unit->cd_tracks[i].start_lba, msf);
            off += 8;
        }
        scsi_write_toc_descriptor(d + off, SCSI_CD_LEADOUT_TRACK, unit->cd_leadout_adr_control,
                                  unit->disk_end_record, msf);
        /* ヘッダ[0..1]には完全な応答長を記録し、実転送はalloc/response_lenの小さい方に
         * 頭打ちする（alloc長ちょうどまでのゼロ埋めはしない）。
         * response_lenは常にSCSI_DATA_BUF_SIZE(1024)以内なのでバッファは溢れない。 */
        d[0] = (uint8_t)((response_len - 2) >> 8);
        d[1] = (uint8_t)(response_len - 2);
        int transfer_len = alloc < response_len ? alloc : response_len;
        spc_data_in_phase(spc, spc->data_buf, transfer_len);
        break;
    }
    default:
        /* 未実装opcodeはRequest Senseで識別できるようILLEGAL REQUESTを記録する。 */
        spc_illegal_request(spc, SPC_ASC_INVALID_COMMAND);
        break;
    }
}

static void spc_transfer_complete(scsi_t *spc) {
    /* CHECK CONDITION等、データ転送(SCMD.CC_TR/DMAC)を経ずに完了する経路でも
     * コマンド完了通知(INTS.CC)は必ず発火させる。ROM側はコマンド全体の完了を
     * INTS.CCの共通ポーリングループで待つため、TRIP依存でCCを間引くと
     * データ転送を伴わない完了（CHECK CONDITION等）で永久ハングする。
     * BUSY/TRIPのクリアはTRIPが実際に立っていた場合のみ行う。 */
    spc_set_interrupt(spc, SPC_INTS_CC);
    if (spc->ssts & SPC_SSTS_TRIP)
        spc->ssts &= (uint8_t)~(SPC_SSTS_BUSY | SPC_SSTS_TRIP);
    switch (spc->psns & SPC_PSNS_PHASE_MASK) {
    case SPC_PHASE_DATA_OUT: {
        /* Write(6)/(10)経由のDATA_OUT完了時のみxfer_bufをホストイメージへ永続化する。
         * pending_writeでゲートするため、Mode Select(6)等
         * 解釈しないDATA_OUT(spc_data_out_phase直接呼び出し)では書き込まれない。 */
        int write_ok = 1;
        if (spc->pending_write && spc->write_unit && spc->write_unit->fp) {
            long off = (long)((uint64_t)spc->write_lba * spc->write_unit->bytes_per_record);
            size_t actual = (size_t)spc->buf_index;
            if (spc->write_unit->overlay) {
                for (size_t i = 0; i < actual / spc->write_unit->bytes_per_record; i++)
                    if (!overlay_write(spc->write_unit->overlay, spc->write_lba + (uint32_t)i,
                                       spc->xfer_buf + i * spc->write_unit->bytes_per_record,
                                       spc->write_unit->bytes_per_record))
                        write_ok = 0;
            } else if (fseek(spc->write_unit->fp, off, SEEK_SET) != 0 ||
                       fwrite(spc->xfer_buf, 1, actual, spc->write_unit->fp) != actual ||
                       fflush(spc->write_unit->fp) != 0)
                write_ok = 0;
            else
                scsi_media_changed(spc);
        }
        spc->pending_write = 0;
        spc->write_unit = NULL;
        if (write_ok)
            spc_status_phase(spc, SPC_STATUS_GOOD, SPC_MSG_COMMAND_COMPLETE);
        else {
            spc_set_sense(spc, SPC_SENSE_KEY_MEDIUM_ERROR, SPC_ASC_WRITE_ERROR, SPC_ASCQ_NONE);
            spc_status_phase(spc, SPC_STATUS_CHECK_CONDITION, SPC_MSG_COMMAND_COMPLETE);
        }
        break;
    }
    case SPC_PHASE_DATA_IN:
        spc_status_phase(spc, SPC_STATUS_GOOD, SPC_MSG_COMMAND_COMPLETE);
        break;
    case SPC_PHASE_COMMAND:
        if (spc->buf_index == 1) {
            int oc = spc->cmd_buf[0];
            spc->buf_limit = SPC_CDB_LENGTH[(oc >> 4) & 0xF];
            if (spc->buf_index < spc->buf_limit) {
                spc->psns |= SPC_PSNS_REQ;
                break;
            }
        }
        spc_execute_command(spc);
        break;
    case SPC_PHASE_STATUS:
        spc_message_in_phase(spc);
        break;
    case SPC_PHASE_MSG_OUT:
        spc_command_phase(spc);
        break;
    case SPC_PHASE_MSG_IN:
        spc_bus_free_phase(spc);
        break;
    default:
        break;
    }
}

void scsi_reset(scsi_t *spc) {
    spc->bdid = SPC_BDID_I7;
    spc->sctl = SPC_SCTL_RD;
    spc->scmd = 0;
    spc->ints = 0;
    spc->psns = 0;
    spc->ssts = 0;
    spc->serr = 0;
    spc->pctl = 0;
    spc->mbc = 0;
    spc->dreg = 0;
    spc->temp = 0;
    spc->tc = 0;
    /* 実SPCのリセット直後はSSTS=TC0|DE(アイドル)。IPLROM 1.6は$E9602Dが0かFFだと
     * 内蔵SCSIなしと判断するため、0のままにしない（XEiJ spiReset準拠）。 */
    spc_update_ssts(spc);
    spc->target_id = -1;
    spc->read_handle = NULL;
    spc->write_handle = NULL;
    spc->buf_index = 0;
    spc->buf_limit = 0;
    spc->pending_write = 0;
    spc->write_unit = NULL;
    memset(spc->sense_buf, 0, sizeof(spc->sense_buf));
    /* units[] はディスク挿入状態なのでリセットでは維持する（FDCのdisk[]と同様） */
}

/* 実SCSI転送はDMAC ch1 CCR STARTが駆動する。
 * 実データ移動自体はDMAC(dmac.c)が$E96035(DREG)をバイト単位で
 * read8/write8することで行い、下のscsi_read8/write8のDREGパスがbuf_index/tc
 * を自前で前進させる。ここではDMAC側が「転送可能か」
 * 「残りバイト数」を知るための最小APIのみ提供する。 */
int scsi_dma_active(const scsi_t *spc) {
    return spc->read_handle != NULL && (spc->ssts & SPC_SSTS_TRIP) != 0 && spc->tc != 0;
}

uint32_t scsi_dma_remaining(const scsi_t *spc) {
    return (uint32_t)(spc->buf_limit - spc->buf_index);
}

int scsi_dma_write_active(const scsi_t *spc) {
    return spc->write_handle != NULL && (spc->ssts & SPC_SSTS_TRIP) != 0 && spc->tc != 0;
}

uint32_t scsi_dma_write_remaining(const scsi_t *spc) {
    return (uint32_t)(spc->buf_limit - spc->buf_index);
}

/* X68K_SCSI_TRACE=1で有効化するレジスタアクセスログ。
 * SUSIE.X等サードパーティSCSIドライバはIOCS _SCSIDRV($F5)を経由せず
 * SPCレジスタへ直接port I/Oするため、IOCSトレース(X68K_TRACE_IOCS)では見えない。 */
static int scsi_trace_enabled(void) {
    static int checked = 0, enabled = 0;
    if (!checked) {
        enabled = getenv("X68K_SCSI_TRACE") != NULL;
        checked = 1;
    }
    return enabled;
}

static const char *spc_off_name(uint32_t off) {
    switch (off & 31u) {
    case SPC_OFF_BDID:
        return "BDID";
    case SPC_OFF_SCTL:
        return "SCTL";
    case SPC_OFF_SCMD:
        return "SCMD";
    case SPC_OFF_INTS:
        return "INTS";
    case SPC_OFF_PSNS:
        return "PSNS";
    case SPC_OFF_SSTS:
        return "SSTS";
    case SPC_OFF_SERR:
        return "SERR";
    case SPC_OFF_PCTL:
        return "PCTL";
    case SPC_OFF_MBC:
        return "MBC";
    case SPC_OFF_DREG:
        return "DREG";
    case SPC_OFF_TEMP:
        return "TEMP";
    case SPC_OFF_TCH:
        return "TCH";
    case SPC_OFF_TCM:
        return "TCM";
    case SPC_OFF_TCL:
        return "TCL";
    default:
        return "?";
    }
}

static void scsi_trace_state(scsi_t *spc, const char *dir, uint32_t off, uint8_t val) {
    if (!scsi_trace_enabled())
        return;
    fprintf(stderr,
            "[SCSI] %s %-4s(+%02X)=%02X  psns=%02X ssts=%02X ints=%02X scmd=%02X tgt=%d "
            "tc=%06X idx=%d/%d\n",
            dir, spc_off_name(off), off & 31u, val, spc->psns, spc->ssts, spc->ints, spc->scmd,
            spc->target_id, spc->tc, spc->buf_index, spc->buf_limit);
}

static uint8_t scsi_read8_impl(scsi_t *spc, uint32_t off) {
    switch (off & 31u) {
    case SPC_OFF_BDID:
        return spc->bdid;
    case SPC_OFF_SCTL:
        return spc->sctl;
    case SPC_OFF_SCMD:
        return spc->scmd;
    case SPC_OFF_INTS:
        return spc->ints;
    case SPC_OFF_PSNS:
        return spc->psns;
    case SPC_OFF_SSTS:
        return spc->ssts;
    case SPC_OFF_SERR:
        return spc->serr;
    case SPC_OFF_PCTL:
        return spc->pctl;
    case SPC_OFF_MBC:
        return spc->mbc;
    case SPC_OFF_DREG: {
        /* 実SPC(MB89352)はDREG読み出し自体がCPU(PIO)転送の1バイト前進トリガになる
         * （実ROMトレースで確認: Request Sense等の小転送はSCMD=Transfer発行後、
         * DMAC ch1には一切触れずDREGをtc回読むだけで完了する）。
         * DMAC ch1もCCR START経由でこの同じDREGパスを
         * バイト単位でx68k_bus_read8越しに叩く設計のため、
         * 読み出すたびに1バイト進めTCH:TCM:TCLを1減算し、0になったら
         * transfer_completeへ進める処理はCPU直叩き・DMAC経由の両方で共通。 */
        if (spc->read_handle && (spc->ssts & SPC_SSTS_TRIP) != 0 && spc->tc != 0 &&
            spc->buf_index < spc->buf_limit) {
            spc->dreg = spc->read_handle[spc->buf_index++];
            if (spc->tc > 0)
                spc->tc--;
            spc_update_ssts(spc);
            /* tc==0だけでなくbuf_index==buf_limit(ターゲット側データ枯渇)でも
             * 完了させる。Request Sense等、CDBのAllocation Lengthがこちらの
             * 応答長より大きい場合にtcが0へ到達せず永久ハングするのを防ぐ。 */
            if (spc->tc == 0 || spc->buf_index == spc->buf_limit)
                spc_transfer_complete(spc);
        }
        return spc->dreg;
    }
    case SPC_OFF_TEMP:
        return spc->temp;
    case SPC_OFF_TCH:
        return (uint8_t)(spc->tc >> 16);
    case SPC_OFF_TCM:
        return (uint8_t)(spc->tc >> 8);
    case SPC_OFF_TCL:
        return (uint8_t)spc->tc;
    default:
        return 0;
    }
}

uint8_t scsi_read8(scsi_t *spc, uint32_t off) {
    uint8_t v = scsi_read8_impl(spc, off);
    scsi_trace_state(spc, "R", off, v);
    return v;
}

static void scsi_write8_impl(scsi_t *spc, uint32_t off, uint8_t val) {
    switch (off & 31u) {
    case SPC_OFF_BDID:
        spc->bdid = (uint8_t)(1u << (val & 7u)); /* 下位3bitをone-hot化して書き込む */
        break;
    case SPC_OFF_SCTL:
        spc->sctl = val;
        break;
    case SPC_OFF_SCMD: {
        spc->scmd = val;
        switch (val & SPC_SCMD_CC_MASK) {
        case SPC_SCMD_CC_BR: /* Bus Release */
            if (spc->target_id >= 0)
                spc_bus_free_phase(spc);
            break;
        case SPC_SCMD_CC_SL: { /* Select */
            uint8_t candidates = (uint8_t)(spc->temp & (uint8_t)~spc->bdid);
            int id = -1;
            for (int i = 0; i < 8; i++) {
                if (candidates & (uint8_t)(1u << i)) {
                    id = i;
                    break;
                }
            }
            /* 空リムーバブルもドライブが存在するためSELECTに応答する。 */
            int connected = (id >= 0 && id < SCSI_NUM_UNITS &&
                             (spc->units[id].fp != NULL || spc->units[id].present));
            if (!connected) {
                /* ユニットが存在しないときセレクションフェーズを開始して直ちにタイムアウトにする
                 */
                spc->ssts |= (uint8_t)(SPC_SSTS_INIT | SPC_SSTS_BUSY);
                spc->psns |= SPC_PSNS_SEL;
                spc->tc = 0;
                spc_set_interrupt(spc, SPC_INTS_TO);
                spc_update_ssts(spc);
            } else {
                spc->target_id = id;
                spc->ssts |= SPC_SSTS_INIT;
                spc_set_interrupt(spc, SPC_INTS_CC);
                if (spc->psns & SPC_PSNS_ATN)
                    spc_message_out_phase(spc);
                else
                    spc_command_phase(spc);
            }
            break;
        }
        case SPC_SCMD_CC_RA: /* Reset ATN */
            spc->psns &= (uint8_t)~SPC_PSNS_ATN;
            break;
        case SPC_SCMD_CC_SA: /* Set ATN */
            spc->psns |= SPC_PSNS_ATN;
            break;
        case SPC_SCMD_CC_TR: /* Transfer: 実転送はDMAC ch1 CCR STARTが駆動する。
                              * ここではSSTS.BUSY|TRIPをセットしてROMにポーリング対象の
                              * 状態遷移を見せるだけで、実データ移動はDMAC ch1 CCR
                              * STARTハンドラ(dmac.c)がDREG($E96035)をバイト単位で
                              * read8/write8することで行う。 */
            spc->ssts |= (uint8_t)(SPC_SSTS_BUSY | SPC_SSTS_TRIP);
            spc_update_ssts(spc);
            break;
        case SPC_SCMD_CC_TP: /* Transfer Pause: 未実装(no-op) */
            break;
        case SPC_SCMD_CC_RR:                      /* Reset ACK/REQ: CPU転送 */
            if ((spc->psns & SPC_PSNS_IO) == 0) { /* Out */
                if (!spc->write_handle)
                    break;
                spc->psns &= (uint8_t)~SPC_PSNS_ACK;
                if (spc->buf_index < spc->buf_limit) {
                    spc->psns |= SPC_PSNS_REQ;
                    break;
                }
                spc_transfer_complete(spc);
            } else { /* In */
                if (!spc->read_handle)
                    break;
                spc->psns &= (uint8_t)~SPC_PSNS_REQ;
                if (spc->buf_index < spc->buf_limit) {
                    spc->psns |= SPC_PSNS_REQ;
                    break;
                }
                spc_transfer_complete(spc);
            }
            break;
        case SPC_SCMD_CC_SR:                      /* Set ACK/REQ */
            if ((spc->psns & SPC_PSNS_IO) == 0) { /* Out */
                if (!spc->write_handle)
                    break;
                spc->psns |= SPC_PSNS_ACK;
                if (spc->buf_index < spc->buf_limit) {
                    spc_store_out_byte(spc, spc->temp);
                    spc_update_ssts(spc);
                }
                spc->psns &= (uint8_t)~SPC_PSNS_REQ;
            } else { /* In */
                if (!spc->read_handle)
                    break;
                spc->psns |= SPC_PSNS_ACK;
                if (spc->buf_index < spc->buf_limit) {
                    spc->temp = spc->read_handle[spc->buf_index++];
                    spc_update_ssts(spc);
                }
                spc->psns &= (uint8_t)~SPC_PSNS_REQ;
            }
            break;
        default:
            break;
        }
        break;
    }
    case SPC_OFF_INTS:
        /* 1を書き込んだビットだけ0クリアするのが基本だが、Selectタイムアウト中
         * (PSNS.SEL=1)にTOをクリアする書込みは特別扱い（XEiJ SPC.java spiWrite
         * SPC_INTS case準拠）。tc!=0ならまだ再送出前なので再度TOにして待たせ、
         * tc==0ならセレクションフェーズ自体を終了させる（PSNS.SEL/SSTS.INIT|BUSY
         * クリア）。これが無いとPSNS.SELが立ちっぱなしになり、ROMが次のID候補で
         * Selectを再送出できずポーリングループでハングする。 */
        if ((spc->psns & SPC_PSNS_SEL) && (spc->ints & val & SPC_INTS_TO)) {
            spc->ints &= (uint8_t)~val;
            if (spc->tc != 0) {
                spc->tc = 0;
                spc_set_interrupt(spc, SPC_INTS_TO);
            } else {
                spc->psns &= (uint8_t)~SPC_PSNS_SEL;
                spc->ssts &= (uint8_t)~(SPC_SSTS_INIT | SPC_SSTS_BUSY);
                spc_update_ssts(spc);
            }
        } else {
            spc->ints &= (uint8_t)~val;
        }
        break;
    case SPC_OFF_SDGC:
        /* 自己診断用レジスタ。X68000 標準ドライバは未使用のため no-op。 */
        break;
    case SPC_OFF_SSTS: /* 読み出し専用 */
    case SPC_OFF_SERR: /* 読み出し専用 */
    case SPC_OFF_MBC:  /* 読み出し専用 */
        break;
    case SPC_OFF_PCTL:
        spc->pctl = val;
        break;
    case SPC_OFF_DREG:
        /* DREG書き込みごとにData-Out PIO転送を1バイト進める。 */
        spc->dreg = val;
        if (spc->write_handle && (spc->ssts & SPC_SSTS_TRIP) != 0 && spc->tc != 0 &&
            spc->buf_index < spc->buf_limit) {
            spc_store_out_byte(spc, val);
            if (spc->tc > 0)
                spc->tc--;
            spc_update_ssts(spc);
            /* tc==0だけでなくbuf_index==buf_limit(ターゲットが要求する分を受け切った)
             * でも完了とする。DREG読み出し側と対称。CDB長より大きいtcを設定して
             * 転送するドライバでも、CDB最終バイトで完了通知に到達させるため。 */
            if (spc->tc == 0 || spc->buf_index == spc->buf_limit)
                spc_transfer_complete(spc);
        }
        break;
    case SPC_OFF_TEMP:
        spc->temp = val;
        break;
    case SPC_OFF_TCH:
        spc->tc = (spc->tc & 0x00FFFFu) | ((uint32_t)val << 16);
        spc_update_ssts(spc);
        break;
    case SPC_OFF_TCM:
        spc->tc = (spc->tc & 0xFF00FFu) | ((uint32_t)val << 8);
        spc_update_ssts(spc);
        break;
    case SPC_OFF_TCL:
        spc->tc = (spc->tc & 0xFFFF00u) | val;
        spc_update_ssts(spc);
        break;
    default:
        break;
    }
}

void scsi_write8(scsi_t *spc, uint32_t off, uint8_t val) {
    scsi_write8_impl(spc, off, val);
    scsi_trace_state(spc, "W", off, val);
}
