#include "peripheral/scsi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include "platform/win_compat.h" /* strcasecmp */
#else
#include <strings.h>
#endif

void scsi_media_changed(scsi_t *spc) {
    if (spc->media_gen == UINT64_MAX) {
        spc->media_gen_overflow = true;
        return;
    }
    spc->media_gen++;
}

/* ---- HDD/MO (X68SCSI1) ---- */

static int attach_hdd(scsi_unit_t *unit, const char *path) {
    /* r+bで開けない(読み取り専用ボリューム・chmodされたMO等)場合はrbへ
     * フォールバックする。
     * removableかどうかはヘッダ内hdr[0x0F]を読むまで分からず、ヘッダを
     * 読むにはまずファイルを開く必要があるため、この判定はHDD/MO問わず
     * ここで共通に行う。 */
    FILE *fp = fopen(path, "r+b");
    int writable = 1;
    if (!fp) {
        fp = fopen(path, "rb");
        writable = 0;
    }
    if (!fp)
        return -1;
    uint8_t hdr[16];
    if (fread(hdr, 1, sizeof(hdr), fp) != sizeof(hdr) || memcmp(hdr, "X68SCSI1", 8) != 0) {
        fclose(fp);
        return -1;
    }
    uint32_t blen = ((uint32_t)hdr[8] << 8) | hdr[9];
    uint32_t blocks = ((uint32_t)hdr[10] << 24) | ((uint32_t)hdr[11] << 16) |
                      ((uint32_t)hdr[12] << 8) | hdr[13];
    unit->fp = fp;
    unit->bytes_per_record = blen;
    unit->disk_end_record = blocks;
    /* デバイスパラメータ$0F(EJ)はリムーバブル属性を表す
     * （『Inside X68000』図9）。 */
    unit->removable = (hdr[0x0F] != 0);
    unit->kind = SCSI_UNIT_DISK;
    /* r+bに失敗した(=ホストが物理的に書けない)場合は種別を問わずWPを強制する。
     * write_protected=0のまま放置すると、Write(6)/(10)がDATA PROTECTで
     * 拒否されずspc_transfer_complete()のfwrite()まで進み、戻り値未チェックの
     * まま書き込みが静かに失われる。removable(MO)側の
     * ホスト権限からの自動判定はここでは行わず、writableなら0で仮置きし
     * x68k_bus_scsi_insert()側で確定させる（リンケージの都合）。 */
    unit->write_protected = writable ? 0 : 1;
    unit->present = 1;
    unit->attention_pending = 1;
    return 0;
}

/* ---- CD-ROM (CloneCD: .ccd + .img) ---- */

static int path_has_ccd_extension(const char *path) {
    size_t len = strlen(path);
    return len >= 4 && strcasecmp(path + len - 4, ".ccd") == 0;
}

static int file_starts_with_clonecd(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return 0;
    char buf[9] = {0};
    size_t n = fread(buf, 1, sizeof(buf), fp);
    fclose(fp);
    return n == sizeof(buf) && memcmp(buf, "[CloneCD]", sizeof(buf)) == 0;
}

/* 拡張子が.ccd(大小無視)、または内容が"[CloneCD]"で始まればCD-ROMと判定する。
 * 拡張子だけに頼らないのは.imgを直接指定された場合にも(HDD経路のX68SCSI1署名
 * チェックで)分かりやすく失敗させるため（docs/ix68k-internals/design/scsi_design.md
 * §4）。 */
static int scsi_path_is_cdrom(const char *path) {
    return path_has_ccd_extension(path) || file_starts_with_clonecd(path);
}

/* "foo.ccd" -> "foo.img" のように拡張子を置き換えたパスを返す(呼び出し元がfree())。
 * 拡張子が無いパスにも対応するため、最後の'.'以降を切り落として付け直す。 */
static char *derive_img_path(const char *ccd_path) {
    const char *dot = strrchr(ccd_path, '.');
    size_t base_len = dot ? (size_t)(dot - ccd_path) : strlen(ccd_path);
    char *img_path = malloc(base_len + 5); /* ".img" + '\0' */
    if (!img_path)
        return NULL;
    memcpy(img_path, ccd_path, base_len);
    memcpy(img_path + base_len, ".img", 5);
    return img_path;
}

static FILE *open_img_for_ccd(const char *ccd_path, char **out_path) {
    char *img_path = derive_img_path(ccd_path);
    if (!img_path)
        return NULL;
    FILE *fp = fopen(img_path, "rb");
    if (!fp) {
        /* 大文字拡張子のフォールバック。 */
        size_t len = strlen(img_path);
        if (len >= 4) {
            img_path[len - 3] = 'I';
            img_path[len - 2] = 'M';
            img_path[len - 1] = 'G';
            fp = fopen(img_path, "rb");
        }
    }
    if (!fp) {
        free(img_path);
        return NULL;
    }
    *out_path = img_path;
    return fp;
}

/* .ccdの[Entry n]から拾った値をscsi_unit_tへ反映する。走査中のエントリはPoint未設定
 * (point<0)で「無効」を表し、次のセクション開始時とEOF時に呼ぶ。 */
static void ccd_apply_entry(scsi_unit_t *unit, long point, long adr, long control, long pmin,
                            long psec, long pframe, long plba, int has_plba) {
    if (point < 0)
        return;
    uint32_t lba;
    if (has_plba) {
        lba = (uint32_t)plba;
    } else {
        /* 古い.ccdでPLBAが無い場合の保険（scsi_design.md §4）。 */
        long total = (pmin * 60 + psec) * 75 + pframe - 150;
        lba = (uint32_t)(total < 0 ? 0 : total);
    }
    uint8_t adr_control = (uint8_t)(((adr & 0x0F) << 4) | (control & 0x0F));
    if (point == 0xA0) {
        unit->cd_first_track = (uint8_t)pmin;
    } else if (point == 0xA1) {
        unit->cd_last_track = (uint8_t)pmin;
    } else if (point == 0xA2) {
        unit->disk_end_record = lba; /* リードアウトLBA */
        unit->cd_leadout_adr_control = adr_control;
    } else if (point >= 1 && point <= 0x63 && unit->cd_track_count < SCSI_CD_MAX_TRACKS) {
        scsi_cd_track_t *t = &unit->cd_tracks[unit->cd_track_count++];
        t->track_no = (uint8_t)point;
        t->adr_control = adr_control;
        t->start_lba = lba;
    }
}

/* "key=value" / "key = value" 行をkey/valueに分割する(前後の空白を落とす)。
 * '='を含まない行(セクション見出し等)は呼び出し元で弾く。 */
static int ccd_parse_kv(char *line, char **key, char **value) {
    char *eq = strchr(line, '=');
    if (!eq)
        return 0;
    *eq = '\0';
    char *k = line;
    while (*k == ' ' || *k == '\t')
        k++;
    char *kend = eq - 1;
    while (kend >= k && (*kend == ' ' || *kend == '\t'))
        *kend-- = '\0';
    char *v = eq + 1;
    while (*v == ' ' || *v == '\t')
        v++;
    *key = k;
    *value = v;
    return 1;
}

/* .ccd(INI形式テキスト)をパースし、unitのCD-ROM関連フィールドを埋める。
 * *out_scrambled = [Disc]のDataTracksScrambled、*out_mode = 最初に見つかった
 * [TRACK n]のMODE(未検出なら-1)。戻り値: 0=成功(トラック1本以上かつリードアウト
 * LBA確定), -1=失敗。 */
static int scsi_ccd_parse(FILE *fp, scsi_unit_t *unit, int *out_scrambled, int *out_mode) {
    char line[256];
    char section[32] = "";
    long point = -1, adr = 0, control = 0, pmin = 0, psec = 0, pframe = 0, plba = 0;
    int has_plba = 0;

    *out_scrambled = 0;
    *out_mode = -1;
    unit->cd_first_track = 0;
    unit->cd_last_track = 0;
    unit->cd_track_count = 0;

    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0')
            continue;
        if (p[0] == '[') {
            /* 新セクション開始: 直前の[Entry]分を確定してから切り替える。 */
            ccd_apply_entry(unit, point, adr, control, pmin, psec, pframe, plba, has_plba);
            point = -1;
            adr = control = pmin = psec = pframe = plba = 0;
            has_plba = 0;
            char *end = strchr(p, ']');
            size_t len = end ? (size_t)(end - p - 1) : strlen(p) - 1;
            if (len >= sizeof(section))
                len = sizeof(section) - 1;
            memcpy(section, p + 1, len);
            section[len] = '\0';
            continue;
        }
        char *key, *value;
        if (!ccd_parse_kv(p, &key, &value))
            continue;
        if (strncmp(section, "Entry", 5) == 0) {
            if (strcmp(key, "Point") == 0)
                point = strtol(value, NULL, 0);
            else if (strcmp(key, "ADR") == 0)
                adr = strtol(value, NULL, 0);
            else if (strcmp(key, "Control") == 0)
                control = strtol(value, NULL, 0);
            else if (strcmp(key, "PMin") == 0)
                pmin = strtol(value, NULL, 0);
            else if (strcmp(key, "PSec") == 0)
                psec = strtol(value, NULL, 0);
            else if (strcmp(key, "PFrame") == 0)
                pframe = strtol(value, NULL, 0);
            else if (strcmp(key, "PLBA") == 0) {
                plba = strtol(value, NULL, 0);
                has_plba = 1;
            }
        } else if (strcmp(section, "Disc") == 0) {
            if (strcmp(key, "DataTracksScrambled") == 0)
                *out_scrambled = (int)strtol(value, NULL, 0);
        } else if (strncmp(section, "TRACK", 5) == 0) {
            if (strcmp(key, "MODE") == 0 && *out_mode < 0)
                *out_mode = (int)strtol(value, NULL, 0);
        }
    }
    ccd_apply_entry(unit, point, adr, control, pmin, psec, pframe, plba, has_plba); /* 末尾分 */

    return (unit->cd_track_count > 0 && unit->disk_end_record > 0) ? 0 : -1;
}

static int attach_cdrom(scsi_unit_t *unit, const char *ccd_path) {
    FILE *ccd_fp = fopen(ccd_path, "rb");
    if (!ccd_fp)
        return -1;
    int scrambled = 0, mode = -1;
    int rc = scsi_ccd_parse(ccd_fp, unit, &scrambled, &mode);
    fclose(ccd_fp);
    if (rc != 0)
        return -1;
    if (scrambled != 0) /* スクランブル済みデータトラックは非スコープ(§9-8) */
        return -1;
    if (mode != 1) /* Mode1以外(Mode2 Form1等)は非スコープ */
        return -1;

    FILE *img_fp = open_img_for_ccd(ccd_path, &unit->img_path);
    if (!img_fp)
        return -1;

    if (fseek(img_fp, 0, SEEK_END) != 0) {
        fclose(img_fp);
        free(unit->img_path);
        unit->img_path = NULL;
        return -1;
    }
    long img_size = ftell(img_fp);
    if (img_size < 0 || img_size % SCSI_CD_RAW_SECTOR != 0) {
        fclose(img_fp);
        free(unit->img_path);
        unit->img_path = NULL;
        return -1;
    }
    uint32_t img_sectors = (uint32_t)(img_size / SCSI_CD_RAW_SECTOR);
    if (img_sectors < unit->disk_end_record) {
        /* 切り詰めイメージへの耐性: .ccd記載のリードアウトLBAより.imgが小さい
         * 場合、実測セクタ数を採用する。 */
        fprintf(stderr,
                "SCSI CD-ROM: %s の.imgが%uセクタしかありません(.ccd記載は%uセクタ) - "
                "切り詰め扱いにします\n",
                ccd_path, img_sectors, unit->disk_end_record);
        unit->disk_end_record = img_sectors;
    }
    if (unit->disk_end_record == 0) {
        fclose(img_fp);
        free(unit->img_path);
        unit->img_path = NULL;
        return -1;
    }

    unit->fp = img_fp;
    unit->bytes_per_record = SCSI_CD_BLOCK_SIZE;
    unit->removable = 1;
    unit->kind = SCSI_UNIT_CDROM;
    unit->write_protected = 1;
    unit->cd_raw_sector_size = SCSI_CD_RAW_SECTOR;
    unit->cd_user_data_offset = SCSI_CD_MODE1_OFFSET;
    unit->present = 1;
    unit->attention_pending = 1;
    return 0;
}

/* ---- 公開API ---- */

int scsi_attach(scsi_t *spc, int id, const char *path) {
    if (id < 0 || id >= SCSI_NUM_UNITS)
        return -1;
    scsi_unit_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    int rc = scsi_path_is_cdrom(path) ? attach_cdrom(&tmp, path) : attach_hdd(&tmp, path);
    if (rc != 0) {
        if (tmp.fp)
            fclose(tmp.fp);
        free(tmp.img_path);
        return -1;
    }
    tmp.path = strdup(path);
    if (!tmp.path) {
        fclose(tmp.fp);
        free(tmp.img_path);
        return -1;
    }
    /* ここまで来たら新メディアの検証は完了している。既存ユニットはここで初めて置き換える
     * （検証失敗時に上のreturn -1で抜けるため、既存メディアを失うことはない）。 */
    if (spc->units[id].fp)
        fclose(spc->units[id].fp);
    free(spc->units[id].path);
    free(spc->units[id].img_path);
    media_fp_free(&spc->units[id].media_fp);
    scsi_overlay_destroy(spc->units[id].overlay);
    spc->units[id] = tmp;
    scsi_media_changed(spc);
    return 0;
}

int scsi_attach_empty_removable(scsi_t *spc, int id) {
    if (id < 0 || id >= SCSI_NUM_UNITS)
        return -1;
    /* メディアを解放し、ドライブだけが存在する状態にする。 */
    if (spc->units[id].fp) {
        fclose(spc->units[id].fp);
        spc->units[id].fp = NULL;
    }
    free(spc->units[id].path);
    spc->units[id].path = NULL;
    free(spc->units[id].img_path);
    spc->units[id].img_path = NULL;
    media_fp_free(&spc->units[id].media_fp);
    scsi_overlay_destroy(spc->units[id].overlay);
    spc->units[id].bytes_per_record = 0;
    spc->units[id].disk_end_record = 0;
    spc->units[id].removable = 1;
    spc->units[id].kind = SCSI_UNIT_DISK;
    spc->units[id].write_protected = 0;
    spc->units[id].present = 1;
    spc->units[id].attention_pending = 1;
    scsi_media_changed(spc);
    return 0;
}

int scsi_attach_empty_cdrom(scsi_t *spc, int id) {
    if (id < 0 || id >= SCSI_NUM_UNITS)
        return -1;
    if (spc->units[id].fp) {
        fclose(spc->units[id].fp);
        spc->units[id].fp = NULL;
    }
    free(spc->units[id].path);
    spc->units[id].path = NULL;
    free(spc->units[id].img_path);
    spc->units[id].img_path = NULL;
    media_fp_free(&spc->units[id].media_fp);
    scsi_overlay_destroy(spc->units[id].overlay);
    spc->units[id].bytes_per_record = 0;
    spc->units[id].disk_end_record = 0;
    spc->units[id].removable = 1;
    spc->units[id].kind = SCSI_UNIT_CDROM;
    spc->units[id].write_protected = 1;
    spc->units[id].cd_first_track = 0;
    spc->units[id].cd_last_track = 0;
    spc->units[id].cd_track_count = 0;
    spc->units[id].present = 1;
    spc->units[id].attention_pending = 1;
    scsi_media_changed(spc);
    return 0;
}

void scsi_detach(scsi_t *spc, int id) {
    if (id < 0 || id >= SCSI_NUM_UNITS)
        return;
    /* リムーバブルはドライブを残し、固定ディスクはユニットごと切断する。 */
    int keep_present = spc->units[id].removable;
    free(spc->units[id].path);
    spc->units[id].path = NULL;
    free(spc->units[id].img_path);
    spc->units[id].img_path = NULL;
    media_fp_free(&spc->units[id].media_fp);
    scsi_overlay_destroy(spc->units[id].overlay);
    spc->units[id].overlay = NULL;
    if (spc->units[id].fp) {
        fclose(spc->units[id].fp);
        spc->units[id].fp = NULL;
    }
    spc->units[id].bytes_per_record = 0;
    spc->units[id].disk_end_record = 0;
    if (!keep_present) {
        spc->units[id].removable = 0;
        spc->units[id].present = 0;
    } else {
        spc->units[id].attention_pending = 1;
    }
    scsi_media_changed(spc);
}
