#include "hostfile.h"
#include <errno.h>  /* EXDEV（rename→コピーのフォールバック判定） */
#include <stdlib.h> /* mkstemp() */
#include <string.h>
#ifdef _WIN32
#include "platform/win_compat.h" /* unlink()/mkstemp()/close()/fdopen() 相当 */
#else
#include <unistd.h> /* unlink() */
#endif

void hostfile_reset_transfer_state(hostfile_t *hf) {
    if (hf->get_src) {
        fclose(hf->get_src);
        hf->get_src = NULL;
    }
    if (hf->put_tmp_fp) {
        fclose(hf->put_tmp_fp);
        hf->put_tmp_fp = NULL;
    }
    if (hf->put_tmp_valid) {
        unlink(hf->put_tmp_path);
        hf->put_tmp_valid = 0;
    }
    hf->len = 0;
    hf->state = 0;
}

static uint32_t hostfile_stage_get32(const hostfile_t *hf, uint32_t off) {
    return ((uint32_t)hf->regs[off] << 24) | ((uint32_t)hf->regs[off + 1] << 16) |
           ((uint32_t)hf->regs[off + 2] << 8) | (uint32_t)hf->regs[off + 3];
}

static void hostfile_stage_put32(hostfile_t *hf, uint32_t off, uint32_t val) {
    hf->regs[off] = (uint8_t)(val >> 24);
    hf->regs[off + 1] = (uint8_t)(val >> 16);
    hf->regs[off + 2] = (uint8_t)(val >> 8);
    hf->regs[off + 3] = (uint8_t)val;
}

#define HOSTFILE_CMD_READ      0x01u /* ソースファイル → 68k RAM（GET用） */
#define HOSTFILE_CMD_WRITE     0x02u /* 68k RAM → 一時ファイル（PUT用） */
#define HOSTFILE_CMD_PUT_BEGIN 0x03u /* 一時ファイルを新規作成 */
#define HOSTFILE_CMD_PUT_END   0x04u /* DATA_LEN を確定し PUT 待ちにする */

uint8_t hostfile_reg_read8(const hostfile_t *hf, uint32_t off) {
    if (off == 0x00)
        return 0x48u; /* MAGIC 上位バイト 'H'（hostdrv_dir 設定に依存せず常時有効） */
    if (off == 0x01)
        return 0x46u; /* MAGIC 下位バイト 'F' */
    if (off == 0x14)
        return hf->state;
    if (off < HOSTFILE_REGS_SIZE)
        return hf->regs[off];
    return 0x00u;
}

/* COMMAND (+0x02) への書き込みで同期実行するコマンド本体。範囲外アクセスは
 * すべて STATUS にエラーコードを立てて早期 return する（HOSTDRV と同じ方針）。 */
static void hostfile_exec_command(hostfile_t *hf, uint8_t *ram, uint32_t ram_size,
                                  uint8_t cmd) {
    uint32_t buf = hostfile_stage_get32(hf, 0x08);   /* PARAM_BUF */
    uint32_t off = hostfile_stage_get32(hf, 0x0C);   /* PARAM_OFFSET */
    uint32_t count = hostfile_stage_get32(hf, 0x10); /* PARAM_COUNT */

    switch (cmd) {
    case HOSTFILE_CMD_READ: {
        if ((uint64_t)buf + count > ram_size) {
            hf->regs[0x03] = 0x03u; /* RAM範囲外 */
            return;
        }
        if (!hf->get_src || (uint64_t)off + count > hf->len) {
            hf->regs[0x03] = 0x02u; /* ソースファイル未オープン/範囲外 */
            return;
        }
        if (fseek(hf->get_src, (long)off, SEEK_SET) != 0 ||
            fread(ram + buf, 1, count, hf->get_src) != count) {
            hf->regs[0x03] = 0x05u; /* ファイルI/Oエラー */
            return;
        }
        hf->regs[0x03] = 0x00u;
        return;
    }

    case HOSTFILE_CMD_WRITE: {
        if ((uint64_t)buf + count > ram_size) {
            hf->regs[0x03] = 0x03u;
            return;
        }
        if (!hf->put_tmp_fp) {
            hf->regs[0x03] = 0x06u; /* PUT_BEGIN未実行 */
            return;
        }
        if (fseek(hf->put_tmp_fp, (long)off, SEEK_SET) != 0 ||
            fwrite(ram + buf, 1, count, hf->put_tmp_fp) != count) {
            hf->regs[0x03] = 0x05u; /* ファイルI/Oエラー */
            return;
        }
        hf->regs[0x03] = 0x00u;
        return;
    }

    case HOSTFILE_CMD_PUT_BEGIN: {
        hostfile_reset_transfer_state(hf); /* 前回分（未完了・未回収問わず）を破棄 */
#ifdef _WIN32
        /* Windowsに/tmpは無いため、%TEMP%を実行時に解決してテンプレートを組み立てる。 */
        win_get_temp_template(hf->put_tmp_path, sizeof(hf->put_tmp_path), "x68k_hostfile_");
#else
        strncpy(hf->put_tmp_path, HF_PUT_TMP_TEMPLATE, sizeof(hf->put_tmp_path) - 1);
        hf->put_tmp_path[sizeof(hf->put_tmp_path) - 1] = '\0';
#endif
        int fd = mkstemp(hf->put_tmp_path);
        if (fd < 0) {
            hf->regs[0x03] = 0x07u; /* 一時ファイル作成失敗 */
            return;
        }
        hf->put_tmp_fp = fdopen(fd, "w+b");
        if (!hf->put_tmp_fp) {
            close(fd);
            unlink(hf->put_tmp_path);
            hf->regs[0x03] = 0x07u;
            return;
        }
        hf->regs[0x03] = 0x00u;
        return;
    }

    case HOSTFILE_CMD_PUT_END:
        if (!hf->put_tmp_fp) {
            hf->regs[0x03] = 0x06u; /* PUT_BEGIN未実行 */
            return;
        }
        fflush(hf->put_tmp_fp);
        fclose(hf->put_tmp_fp);
        hf->put_tmp_fp = NULL;
        hf->put_tmp_valid = 1;
        hf->len = hostfile_stage_get32(hf, 0x04); /* DATA_LEN */
        hf->state = 2;                            /* PUT待ち */
        hf->regs[0x03] = 0x00u;
        return;

    default:
        hf->regs[0x03] = 0x01u; /* 未対応コマンド */
        return;
    }
}

void hostfile_reg_write8(hostfile_t *hf, uint8_t *ram, uint32_t ram_size, uint32_t off,
                         uint8_t val) {
    if (off < HOSTFILE_REGS_SIZE)
        hf->regs[off] = val;
    if (off == 0x02u)
        hostfile_exec_command(hf, ram, ram_size, val);
}

long hostfile_put(hostfile_t *hf, const char *path) {
    hostfile_reset_transfer_state(hf); /* 前回分（未完了・未回収問わず）を破棄 */
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    /* ファイル全体はRAMに載せず、get_srcを開いたまま保持する。68kのREAD要求
     * ごとにfseek+freadでオンデマンドに読む（100MB超でもホスト側メモリはO(1)）。 */
    hf->get_src = f;
    hf->len = (uint32_t)size;
    hostfile_stage_put32(hf, 0x04, (uint32_t)size); /* DATA_LEN: 68k から RO で読める */
    hf->state = 1;                                  /* GET待ち */
    return size;
}

long hostfile_get(hostfile_t *hf, const char *path) {
    if (hf->state != 2 || !hf->put_tmp_valid)
        return -1; /* 68k側が HOSTFILE PUT で確定させていない */
    long size = (long)hf->len;
    /* 一時ファイルを目的地へ rename するだけで済ませる（同一ファイルシステムなら
     * データコピー不要、O(1)）。クロスデバイス(EXDEV)時のみ手動コピーへフォールバック。 */
    if (rename(hf->put_tmp_path, path) != 0) {
        if (errno != EXDEV)
            return -1;
        FILE *src = fopen(hf->put_tmp_path, "rb");
        FILE *dst = src ? fopen(path, "wb") : NULL;
        if (!src || !dst) {
            if (src)
                fclose(src);
            if (dst)
                fclose(dst);
            return -1;
        }
        uint8_t copybuf[65536];
        size_t n;
        int ok = 1;
        while ((n = fread(copybuf, 1, sizeof(copybuf), src)) > 0) {
            if (fwrite(copybuf, 1, n, dst) != n) {
                ok = 0;
                break;
            }
        }
        int src_err = ferror(src);
        fclose(src);
        fclose(dst);
        if (!ok || src_err)
            return -1;
        unlink(hf->put_tmp_path);
    }
    hf->put_tmp_valid = 0;
    hf->state = 0; /* 空に戻す */
    return size;
}

void hostfile_status(const hostfile_t *hf, int *state, uint32_t *len) {
    *state = hf->state;
    *len = hf->len;
}
