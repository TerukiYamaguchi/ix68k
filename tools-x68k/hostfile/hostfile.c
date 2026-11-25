#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <x68k/dos.h>

/* HOSTFILE.X: 稼働中のエミュレータと macOS 間でファイルを1本単位でやり取りする
 * コマンド（Issue #39）。HOSTDRV.SYS（偽の FAT ボリューム合成）とは異なり、
 * 普通の _dos_create/_dos_write 等の DOS コールで実ドライブに読み書きするため、
 * Human68k 自身のディレクトリ/FAT キャッシュと矛盾しない。
 *
 * MMIO 裏口: 0xEA0100-0xEA0117（src/bus/x68k_bus.c 参照）。
 *
 * 使い方:
 *   HOSTFILE GET <出力ファイル名>   ホスト側が ix68k-hostfile put 済みのファイルを受け取る
 *   HOSTFILE PUT <入力ファイル名>   ファイルをホスト側へ送る(ix68k-hostfile get で回収)
 *
 * GET/PUT は send/scp 等と同じく「自分から見た自分の動作」を表す（絶対方向
 * ラベルではない）。macOS側の ix68k-hostfile put/get とは語が逆になる:
 * macOSがPUTしたものをHuman68kがGETで受け取り、Human68kがPUTしたものを
 * macOSがGETで受け取る。
 */

#define HOSTFILE_BASE 0xEA0100u

#define HOSTFILE_MAGIC0       (*(volatile uint8_t *)(HOSTFILE_BASE + 0x00))
#define HOSTFILE_MAGIC1       (*(volatile uint8_t *)(HOSTFILE_BASE + 0x01))
#define HOSTFILE_COMMAND      (*(volatile uint8_t *)(HOSTFILE_BASE + 0x02))
#define HOSTFILE_STATUS       (*(volatile uint8_t *)(HOSTFILE_BASE + 0x03))
#define HOSTFILE_DATA_LEN     (*(volatile uint32_t *)(HOSTFILE_BASE + 0x04))
#define HOSTFILE_PARAM_BUF    (*(volatile uint32_t *)(HOSTFILE_BASE + 0x08))
#define HOSTFILE_PARAM_OFFSET (*(volatile uint32_t *)(HOSTFILE_BASE + 0x0C))
#define HOSTFILE_PARAM_COUNT  (*(volatile uint32_t *)(HOSTFILE_BASE + 0x10))
#define HOSTFILE_STATE        (*(volatile uint8_t *)(HOSTFILE_BASE + 0x14))

#define HOSTFILE_CMD_READ      0x01u
#define HOSTFILE_CMD_WRITE     0x02u
#define HOSTFILE_CMD_PUT_BEGIN 0x03u
#define HOSTFILE_CMD_PUT_END   0x04u

#define CHUNK         4096
#define PROGRESS_STEP (64u * 1024u) /* 64KBごとに進捗表示（ハング調査用、2026-07-13追加） */

static uint8_t buf[CHUNK];

/* 転送進捗を同一行に上書き表示する。fflush で即座に画面へ反映させる
 * （ハング/クラッシュ時にも「どこまで進んだか」が画面に残るようにするため）。 */
static void print_progress(uint32_t off, uint32_t total) {
    unsigned int pct = total > 0 ? (unsigned int)((uint64_t)off * 100 / total) : 100;
    printf("\r%lu / %lu バイト (%u%%)  ", (unsigned long)off, (unsigned long)total, pct);
    fflush(stdout);
}

static int check_magic(void) {
    if (HOSTFILE_MAGIC0 != 0x48u || HOSTFILE_MAGIC1 != 0x46u) {
        printf("HOSTFILE: エミュレータ側の裏口が見つかりません（未対応ビルドの可能性）\r\n");
        return 0;
    }
    return 1;
}

/* dest の親ディレクトリを必要なだけ作る。区切りは '\' と '/' 両対応。
 * ドライブ指定 "A:" やルート先頭の区切りはスキップする。
 * ループ末尾の要素はファイル名本体なので mkdir しない。 */
static void ensure_parent_dirs(const char *dest) {
    char path[92]; /* Human68k フルパス上限に合わせる */
    int n = 0;
    const char *p = dest;

    if (p[0] && p[1] == ':') {
        path[n++] = p[0];
        path[n++] = ':';
        p += 2;
    }
    for (; *p; p++) {
        char c = *p;
        if (c == '/' || c == '\\') {
            if (n > 0 && path[n - 1] != ':') {
                path[n] = '\0';
                _dos_mkdir(path); /* 既存なら失敗するが無視してよい */
            }
            path[n++] = '\\';
        } else {
            path[n++] = c;
        }
        if (n >= (int)sizeof(path) - 1)
            break;
    }
}

static int cmd_get(const char *dest) {
    if (!check_magic())
        return 1;
    if (HOSTFILE_STATE != 1) {
        printf("HOSTFILE: ホスト側がファイルを送っていません"
               "（macOSで `ix68k-hostfile put` を先に実行してください）\r\n");
        return 1;
    }

    uint32_t len = HOSTFILE_DATA_LEN;
    ensure_parent_dirs(dest);
    int fd = _dos_create(dest, 0);
    if (fd < 0) {
        printf("HOSTFILE: ファイル作成に失敗しました: %s\r\n", dest);
        return 1;
    }

    uint32_t off = 0;
    uint32_t next_report = 0;
    while (off < len) {
        uint32_t n = len - off;
        if (n > CHUNK)
            n = CHUNK;
        HOSTFILE_PARAM_BUF = (uint32_t)buf;
        HOSTFILE_PARAM_OFFSET = off;
        HOSTFILE_PARAM_COUNT = n;
        HOSTFILE_COMMAND = HOSTFILE_CMD_READ;
        if (HOSTFILE_STATUS != 0) {
            printf("\r\nHOSTFILE: 転送エラー（status=%d、%lu/%lu バイトで中断）\r\n",
                   HOSTFILE_STATUS, (unsigned long)off, (unsigned long)len);
            _dos_close(fd);
            return 1;
        }
        int wrote = _dos_write(fd, (char *)buf, (int)n);
        if (wrote != (int)n) {
            printf("\r\nHOSTFILE: 書き込みに失敗しました（ディスク容量不足の可能性、"
                   "%lu/%lu バイトで中断）\r\n",
                   (unsigned long)off, (unsigned long)len);
            _dos_close(fd);
            return 1;
        }
        off += n;
        if (off >= next_report || off >= len) {
            print_progress(off, len);
            next_report = off + PROGRESS_STEP;
        }
    }
    _dos_close(fd);
    printf("\r\nHOSTFILE: %lu バイトを %s へ書き込みました\r\n", (unsigned long)len, dest);
    return 0;
}

static int cmd_put(const char *src) {
    if (!check_magic())
        return 1;

    int fd = _dos_open(src, 0);
    if (fd < 0) {
        printf("HOSTFILE: ファイルを開けません: %s\r\n", src);
        return 1;
    }
    long size = _dos_seek(fd, 0, 2);
    _dos_seek(fd, 0, 0);
    if (size < 0) {
        printf("HOSTFILE: サイズ取得に失敗しました\r\n");
        _dos_close(fd);
        return 1;
    }

    HOSTFILE_COMMAND = HOSTFILE_CMD_PUT_BEGIN;
    uint32_t off = 0;
    uint32_t next_report = 0;
    while (off < (uint32_t)size) {
        int n = _dos_read(fd, (char *)buf, CHUNK);
        if (n <= 0)
            break;
        HOSTFILE_PARAM_BUF = (uint32_t)buf;
        HOSTFILE_PARAM_OFFSET = off;
        HOSTFILE_PARAM_COUNT = (uint32_t)n;
        HOSTFILE_COMMAND = HOSTFILE_CMD_WRITE;
        if (HOSTFILE_STATUS != 0) {
            printf("\r\nHOSTFILE: 転送エラー（status=%d、%lu/%lu バイトで中断）\r\n",
                   HOSTFILE_STATUS, (unsigned long)off, (unsigned long)size);
            _dos_close(fd);
            return 1;
        }
        off += (uint32_t)n;
        if (off >= next_report || off >= (uint32_t)size) {
            print_progress(off, (uint32_t)size);
            next_report = off + PROGRESS_STEP;
        }
    }
    HOSTFILE_DATA_LEN = off;
    HOSTFILE_COMMAND = HOSTFILE_CMD_PUT_END;
    _dos_close(fd);
    printf("\r\nHOSTFILE: %lu バイトの送信準備ができました。"
           "macOS側で `ix68k-hostfile get` を実行してください\r\n",
           (unsigned long)off);
    return 0;
}

#define HOSTFILE_VERSION "1.0"

static void usage(void) {
    printf("HOSTFILE.X Ver.%s\r\n", HOSTFILE_VERSION);
    printf("使い方: HOSTFILE GET <出力ファイル名>\r\n");
    printf("        HOSTFILE PUT <入力ファイル名>\r\n");
}

int main(int argc, char **argv) {
    if (argc == 2 && (strcmp(argv[1], "-V") == 0 || strcmp(argv[1], "-v") == 0)) {
        printf("HOSTFILE.X Ver.%s\r\n", HOSTFILE_VERSION);
        return 0;
    }
    if (argc != 3) {
        usage();
        return 1;
    }
    if (strcmp(argv[1], "GET") == 0 || strcmp(argv[1], "get") == 0)
        return cmd_get(argv[2]);
    if (strcmp(argv[1], "PUT") == 0 || strcmp(argv[1], "put") == 0)
        return cmd_put(argv[2]);
    usage();
    return 1;
}
