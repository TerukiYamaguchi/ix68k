/*
 * HostDrv comcodハンドラの単体テスト。bus/CPU非依存のhostdrv_rd_mem_t
 * アクセサをuint8_t buf[]直参照のスタブで実装し、リクエストヘッダ・_NAMESTS・
 * FCBを直接組んでhostdrv_remote_reg_write8()経由でハンドラを呼ぶ。
 */
#include "peripheral/hostdrv_remote.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h> /* mkdir() */
#ifdef _WIN32
#include "platform/win_compat.h" /* unlink()/access()/rmdir() 相当 */
#else
#include <unistd.h>
#endif

#define REQ_BASE     0u   /* リクエストヘッダ(26B) */
#define BUF_BASE     64u  /* GETDPB/DSKFRE出力バッファ */
#define NAMESTS_BASE 100u /* _NAMESTS(88B) */
#define FCB_BASE     200u /* FCB(96B) */
#define FILES_BASE   300u /* _FILESバッファ(53B) */
#define FILES2_BASE  600u /* ネストした_FILES用の2つ目のバッファ(53B) */
#define DATA_BASE    400u /* READ/WRITE転送用スクラッチ */

#define README_CONTENT "HOSTDRV PHASE2 OK\n"

/* comcod $47 の検索属性(req+13)。hostdrv_design.md §5・実機トレースで確認したDIRの
 * 一般検索値(READONLY|SYSTEM|DIRECTORY|ARCHIVE=$35)をhostdrv_remote.cの
 * private HOSTDRV_ATTR_*ミラーとして再定義する。 */
#define ATTR_READONLY       0x01u
#define ATTR_SYSTEM         0x04u
#define ATTR_DIRECTORY      0x10u
#define ATTR_ARCHIVE        0x20u
#define ATTR_GENERAL_SEARCH (ATTR_READONLY | ATTR_SYSTEM | ATTR_DIRECTORY | ATTR_ARCHIVE)

static uint8_t test_read8(void *ctx, uint32_t addr) {
    return ((uint8_t *)ctx)[addr];
}
static void test_write8(void *ctx, uint32_t addr, uint8_t val) {
    ((uint8_t *)ctx)[addr] = val;
}

static void set_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static uint16_t get_be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/* リクエストヘッダのcomcodを撃ってEXECを走らせ、+18(リザルトステータス)を返す。
 * +13/+14/+18/+22等、comcod依存の入力フィールドは呼び出し側が事前にbufへ
 * 書いておく（+0/+1/+2以外はクリアしない）。 */
static int32_t exec_comcod(uint8_t *buf, uint8_t comcod) {
    hostdrv_remote_t st = {0};
    hostdrv_rd_mem_t mem = {.ctx = buf, .read8 = test_read8, .write8 = test_write8};

    buf[REQ_BASE + 0] = 26;
    buf[REQ_BASE + 2] = comcod;

    hostdrv_remote_reg_write8(&st, &mem, 0x04, (uint8_t)(REQ_BASE >> 24));
    hostdrv_remote_reg_write8(&st, &mem, 0x05, (uint8_t)(REQ_BASE >> 16));
    hostdrv_remote_reg_write8(&st, &mem, 0x06, (uint8_t)(REQ_BASE >> 8));
    hostdrv_remote_reg_write8(&st, &mem, 0x07, (uint8_t)REQ_BASE);
    hostdrv_remote_reg_write8(&st, &mem, 0x02, HOSTDRV_RD_CMD_EXEC);

    return (int32_t)get_be32(&buf[REQ_BASE + 18]);
}

/* _NAMESTS(88B)を組む。pathは常にルート（NAMPTH区切りは0x09、hostdrv_design.md §4）
 * 固定。nameはSJISの"NAME.EXT"（最大主名18+拡張子3文字）または拡張子無し
 * "NAME"、'?'ワイルドカードも可。主名が8文字を超える場合は+78(10B)にも書く
 * （18+3の対応）。+78は実機と同じく未使用時0x00で埋める。 */
static void set_namests(uint8_t *buf, uint32_t addr, const char *name) {
    memset(&buf[addr], 0, 88);
    buf[addr + 2] = 0x09;
    buf[addr + 3] = 0;
    memset(&buf[addr + 67], ' ', 8);
    memset(&buf[addr + 75], ' ', 3);
    memset(&buf[addr + 78], 0, 10);
    const char *dot = strchr(name, '.');
    size_t namelen = dot ? (size_t)(dot - name) : strlen(name);
    if (namelen > 18)
        namelen = 18;
    size_t n1 = namelen > 8 ? 8 : namelen;
    memcpy(&buf[addr + 67], name, n1);
    if (namelen > 8)
        memcpy(&buf[addr + 78], name + 8, namelen - 8);
    if (dot != NULL) {
        size_t extlen = strlen(dot + 1);
        if (extlen > 3)
            extlen = 3;
        memcpy(&buf[addr + 75], dot + 1, extlen);
    }
}

static void write_host_file(const char *dir, const char *name, const char *content) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(
        path, "wb"); /* バイナリ固定: "w"はWindowsで'\n'→"\r\n"変換されファイルサイズがずれる */
    assert(f != NULL);
    fputs(content, f);
    fclose(f);
}

static void test_mediacheck_returns_zero(void) {
    uint8_t buf[512] = {0};
    assert(exec_comcod(buf, 0x57) == 0);
}

static void test_verify_bit_masked(void) {
    uint8_t buf_a[512] = {0}, buf_b[512] = {0};
    assert(exec_comcod(buf_a, 0x57) == exec_comcod(buf_b, (uint8_t)(0x57 | 0x80)));
}

static void test_unknown_comcod(void) {
    uint8_t buf[512] = {0};
    /* $53 _DISKRED は設計上「未対応」のまま据え置き（hostdrv_design.md §5）。 */
    assert(exec_comcod(buf, 0x53) == -1);
    assert(buf[REQ_BASE + 3] == 0);
    assert(buf[REQ_BASE + 4] == 0);
}

static void test_fflush_noop(void) {
    uint8_t buf[512] = {0};
    assert(exec_comcod(buf, 0x56) == 0);
}

static void test_getdpb_zeroes_buffer(void) {
    uint8_t buf[512];
    memset(buf, 0xff, sizeof(buf)); /* ダミー値で埋めて書き換えを検出する */
    set_be32(&buf[REQ_BASE + 14], BUF_BASE);
    assert(exec_comcod(buf, 0x52) == 0);
    for (int i = 0; i < 16; i++)
        assert(buf[BUF_BASE + i] == 0);
}

static void test_dskfre_reports_free_bytes(void) {
    uint8_t buf[512] = {0};
    set_be32(&buf[REQ_BASE + 14], BUF_BASE);
    assert(exec_comcod(buf, 0x50) >= 0);          /* 使用可能バイト数は負値にならない */
    assert(get_be16(&buf[BUF_BASE + 4]) >= 1);    /* 1クラスタあたりのセクタ数 */
    assert(get_be16(&buf[BUF_BASE + 6]) == 1024); /* 1セクタあたりのバイト数 */
}

static void test_drvctrl_mode0_sense(void) {
    uint8_t buf[512] = {0};
    buf[REQ_BASE + 13] = 0; /* mode = センス */
    assert(exec_comcod(buf, 0x51) == 0);
    uint8_t sense = buf[REQ_BASE + 13];
    assert((sense & 0x02u) != 0); /* ABU_INSERTED */
    assert((sense & 0x08u) ==
           0); /* mkdtemp()直後は書き込み可能なのでABU_WRITE_PROTECTEDは立たない */
}

static void test_drvctrl_other_mode_is_noop(void) {
    uint8_t buf[512] = {0};
    buf[REQ_BASE + 13] =
        1; /* mode = イジェクト。host dirには概念が無いため無処理で成功を返す */
    assert(exec_comcod(buf, 0x51) == 0);
}

/* $41 _CHDIRはルートの_NAMESTSを受け入れる。 */
static void test_chdir_always_succeeds(void) {
    uint8_t buf[512] = {0};
    set_namests(buf, NAMESTS_BASE, "");
    set_be32(&buf[REQ_BASE + 14], NAMESTS_BASE);
    assert(exec_comcod(buf, 0x41) == 0);
}

/* $47/$48: 空ディレクトリでは0件、README.TXT作成後は1件ヒットして名前・属性が
 * 正しく書き込まれる。8.3を超える名前(TOOLONGNAME.TXT)は除外される。
 * 空ディレクトリでの$47(初回)はNO_MORE_FILES(-18)ではなくFILE_NOT_FOUND(-2)を
 * 返す（mint.x等がFindFirst相当の-2を「ディレクトリは存在するが空」として
 * 特別扱いするため。参照実装XEiJ HFS.javaも同じく-2を返す。$48で尽きた場合は
 * NO_MORE_FILES(-18)を返す）。 */
static void test_files_empty_then_lists_created_file(const char *host_dir) {
    uint8_t buf[512] = {0};
    set_namests(buf, NAMESTS_BASE, "????????.???");
    set_be32(&buf[REQ_BASE + 14], NAMESTS_BASE);
    set_be32(&buf[REQ_BASE + 18], FILES_BASE);
    buf[REQ_BASE + 13] = ATTR_GENERAL_SEARCH;
    assert(exec_comcod(buf, 0x47) == -2);

    write_host_file(host_dir, "README.TXT", README_CONTENT);
    write_host_file(host_dir, "TOOLONGNAME.TXT", "8.3超のため除外される\n");

    memset(buf, 0, sizeof(buf));
    set_namests(buf, NAMESTS_BASE, "????????.???");
    set_be32(&buf[REQ_BASE + 14], NAMESTS_BASE);
    set_be32(&buf[REQ_BASE + 18], FILES_BASE);
    buf[REQ_BASE + 13] = ATTR_GENERAL_SEARCH;
    assert(exec_comcod(buf, 0x47) == 0);
    assert(strcmp((const char *)&buf[FILES_BASE + 30], "README.TXT") == 0);
    assert(buf[FILES_BASE + 21] == ATTR_ARCHIVE);
    assert(get_be32(&buf[FILES_BASE + 26]) == strlen(README_CONTENT));

    memset(&buf[REQ_BASE], 0, 26);
    set_be32(&buf[REQ_BASE + 18], FILES_BASE);
    assert(exec_comcod(buf, 0x48) == -18); /* README.TXT以外に一致は無い */
}

/* $47は日本語ファイル名をSJISの18+3形式で返す。 */
static void test_files_lists_japanese_filename(const char *host_dir) {
    write_host_file(host_dir, "日本語.txt", "test\n");

    char pattern[23];
    memset(pattern, '?', 18);
    pattern[18] = '.';
    memset(pattern + 19, '?', 3);
    pattern[22] = '\0';

    uint8_t buf[512] = {0};
    set_namests(buf, NAMESTS_BASE, pattern);
    set_be32(&buf[REQ_BASE + 14], NAMESTS_BASE);
    set_be32(&buf[REQ_BASE + 18], FILES_BASE);
    buf[REQ_BASE + 13] = ATTR_GENERAL_SEARCH;
    assert(exec_comcod(buf, 0x47) == 0);
    /* README.TXT/TOOLONGNAME.TXT(前テストで作成済み)も同じ全ワイルドカードに
     * ヒットしうるため、_FILES→_NFILESを順に辿って日本語ファイルの出現を確認する。
     * SJISで「日本語.txt」+0終端（ホスト側の実名通り、拡張子は大文字化しない）。 */
    static const uint8_t expected[] = {0x93, 0xfa, 0x96, 0x7b, 0x8c, 0xea,
                                       '.',  't',  'x',  't',  0};
    int found = 0;
    for (;;) {
        if (memcmp(&buf[FILES_BASE + 30], expected, sizeof(expected)) == 0) {
            found = 1;
            break;
        }
        memset(&buf[REQ_BASE], 0, 26);
        set_be32(&buf[REQ_BASE + 18], FILES_BASE);
        if (exec_comcod(buf, 0x48) != 0)
            break; /* -18 NO_MORE_FILES */
    }
    assert(found);

    char path[512];
    snprintf(path, sizeof(path), "%s/日本語.txt", host_dir);
    unlink(path);
}

/* mint.xの再帰コピー(cpmv.s)は _FILES → cpmv_sub内のネストした_FILES → _NFILES の
 * 順に呼ぶ。検索セッションがグローバルに1つしか無いとネストした_FILESが外側の列挙を
 * 壊し、外側は1件目で打ち切られる（HOSTDRV上のディレクトリをHDDへコピーすると
 * 最初の1ファイルしかコピーされない不具合）。セッションはFILBUFごとに持つ。 */
static void test_nested_files_does_not_break_outer_search(const char *host_dir) {
    static const char *const names[] = {"CPMV_A.TXT", "CPMV_B.TXT", "CPMV_C.TXT"};
    enum { NAME_NUM = sizeof(names) / sizeof(names[0]) };
    for (int i = 0; i < NAME_NUM; i++)
        write_host_file(host_dir, names[i], "x\n");

    uint8_t buf[1024] = {0};
    set_namests(buf, NAMESTS_BASE, "????????.???");
    set_be32(&buf[REQ_BASE + 14], NAMESTS_BASE);
    set_be32(&buf[REQ_BASE + 18], FILES_BASE);
    buf[REQ_BASE + 13] = ATTR_GENERAL_SEARCH;
    assert(exec_comcod(buf, 0x47) == 0);

    int found[NAME_NUM] = {0};
    for (int guard = 0; guard < 100; guard++) {
        for (int i = 0; i < NAME_NUM; i++) {
            if (strcmp((const char *)&buf[FILES_BASE + 30], names[i]) == 0)
                found[i] = 1;
        }

        /* 外側の列挙の途中で、別のFILBUFに対して_FILESを撃つ(cpmv_sub相当) */
        memset(&buf[REQ_BASE], 0, 26);
        set_namests(buf, NAMESTS_BASE, names[0]);
        set_be32(&buf[REQ_BASE + 14], NAMESTS_BASE);
        set_be32(&buf[REQ_BASE + 18], FILES2_BASE);
        buf[REQ_BASE + 13] = ATTR_GENERAL_SEARCH;
        assert(exec_comcod(buf, 0x47) == 0);
        assert(strcmp((const char *)&buf[FILES2_BASE + 30], names[0]) == 0);

        memset(&buf[REQ_BASE], 0, 26);
        set_be32(&buf[REQ_BASE + 18], FILES_BASE);
        if (exec_comcod(buf, 0x48) != 0)
            break; /* -18 NO_MORE_FILES */
    }
    for (int i = 0; i < NAME_NUM; i++)
        assert(found[i]);

    char path[512];
    for (int i = 0; i < NAME_NUM; i++) {
        snprintf(path, sizeof(path), "%s/%s", host_dir, names[i]);
        unlink(path);
    }
}

/* 検索属性は「1を指定したビットが1個でも一致すれば返す」ORマッチ
 * （X68k DOSCALL資料の_FILES記述）。mmdsp.rは同じ
 * ディレクトリをディレクトリ属性($10)とファイル属性($20)の2パスで走査して結果を
 * 1つのリストへ追記する（mmd_files.s FNAME_SET）ため、通常ファイルが$10のパスにも
 * ヒットすると一覧が二重になる。 */
static void test_search_attr_selects_dirs_and_files_separately(const char *host_dir) {
    char path[512];
    snprintf(path, sizeof(path), "%s/ATTRDIR", host_dir);
    assert(mkdir(path, 0777) == 0 || errno == EEXIST);
    write_host_file(host_dir, "ATTRFILE.TXT", "x\n");

    /* $10(ディレクトリ)のみ: 通常ファイルは返らない */
    uint8_t buf[512] = {0};
    set_namests(buf, NAMESTS_BASE, "????????.???");
    set_be32(&buf[REQ_BASE + 14], NAMESTS_BASE);
    set_be32(&buf[REQ_BASE + 18], FILES_BASE);
    buf[REQ_BASE + 13] = ATTR_DIRECTORY;
    int32_t res = exec_comcod(buf, 0x47);
    while (res == 0) {
        assert(buf[FILES_BASE + 21] & ATTR_DIRECTORY);
        memset(&buf[REQ_BASE], 0, 26);
        set_be32(&buf[REQ_BASE + 18], FILES_BASE);
        res = exec_comcod(buf, 0x48);
    }

    /* $20(通常ファイル)のみ: ディレクトリは返らない */
    memset(buf, 0, sizeof(buf));
    set_namests(buf, NAMESTS_BASE, "????????.???");
    set_be32(&buf[REQ_BASE + 14], NAMESTS_BASE);
    set_be32(&buf[REQ_BASE + 18], FILES_BASE);
    buf[REQ_BASE + 13] = ATTR_ARCHIVE;
    res = exec_comcod(buf, 0x47);
    int found_file = 0;
    while (res == 0) {
        assert(!(buf[FILES_BASE + 21] & ATTR_DIRECTORY));
        if (strcmp((const char *)&buf[FILES_BASE + 30], "ATTRFILE.TXT") == 0)
            found_file = 1;
        memset(&buf[REQ_BASE], 0, 26);
        set_be32(&buf[REQ_BASE + 18], FILES_BASE);
        res = exec_comcod(buf, 0x48);
    }
    assert(found_file);

    snprintf(path, sizeof(path), "%s/ATTRFILE.TXT", host_dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/ATTRDIR", host_dir);
    rmdir(path);
}

static void test_chmod_reports_attr(void) {
    uint8_t buf[512] = {0};
    set_namests(buf, NAMESTS_BASE, "README.TXT");
    set_be32(&buf[REQ_BASE + 14], NAMESTS_BASE);
    buf[REQ_BASE + 13] = (uint8_t)-1; /* 属性照会 */
    assert(exec_comcod(buf, 0x46) == ATTR_ARCHIVE);
}

static void test_open_read_close_roundtrip(void) {
    uint8_t buf[512] = {0};
    set_namests(buf, NAMESTS_BASE, "README.TXT");
    set_be32(&buf[REQ_BASE + 14], NAMESTS_BASE);
    set_be32(&buf[REQ_BASE + 22], FCB_BASE);
    buf[FCB_BASE + 14] = 0; /* アクセスモード=読み出し */
    assert(exec_comcod(buf, 0x4a) == 0);
    assert(get_be32(&buf[FCB_BASE + 64]) == strlen(README_CONTENT));
    assert(memcmp(&buf[FCB_BASE + 36], "README  ", 8) == 0);
    assert(memcmp(&buf[FCB_BASE + 44], "TXT", 3) == 0);

    memset(&buf[REQ_BASE], 0, 26);
    set_be32(&buf[REQ_BASE + 14], DATA_BASE);
    set_be32(&buf[REQ_BASE + 18], 4096); /* ファイルサイズを超える要求量 */
    set_be32(&buf[REQ_BASE + 22], FCB_BASE);
    int32_t nread = exec_comcod(buf, 0x4c);
    assert((size_t)nread == strlen(README_CONTENT));
    assert(memcmp(&buf[DATA_BASE], README_CONTENT, strlen(README_CONTENT)) == 0);
    assert(get_be32(&buf[FCB_BASE + 6]) == strlen(README_CONTENT)); /* ファイルポインタが進む */

    memset(&buf[REQ_BASE], 0, 26); /* 2回目のREADはEOFで0バイト（エラーではない） */
    set_be32(&buf[REQ_BASE + 14], DATA_BASE);
    set_be32(&buf[REQ_BASE + 18], 16);
    set_be32(&buf[REQ_BASE + 22], FCB_BASE);
    assert(exec_comcod(buf, 0x4c) == 0);

    memset(&buf[REQ_BASE], 0, 26);
    set_be32(&buf[REQ_BASE + 22], FCB_BASE);
    assert(exec_comcod(buf, 0x4b) == 0);
}

static void test_open_nonexistent_returns_not_found(void) {
    uint8_t buf[512] = {0};
    set_namests(buf, NAMESTS_BASE, "NOSUCH.TXT");
    set_be32(&buf[REQ_BASE + 14], NAMESTS_BASE);
    set_be32(&buf[REQ_BASE + 22], FCB_BASE);
    assert(exec_comcod(buf, 0x4a) == -2);
}

/* 主名が8文字ちょうどでも+78の未使用領域と正しく連結して解決する。 */
static void test_open_exact_8char_name(const char *host_dir) {
    write_host_file(host_dir, "IOCSTEST.TXT", "hostdrv test data\n");

    uint8_t buf[512] = {0};
    set_namests(buf, NAMESTS_BASE, "IOCSTEST.TXT");
    set_be32(&buf[REQ_BASE + 14], NAMESTS_BASE);
    set_be32(&buf[REQ_BASE + 22], FCB_BASE);
    assert(exec_comcod(buf, 0x4a) == 0);

    memset(&buf[REQ_BASE], 0, 26);
    set_be32(&buf[REQ_BASE + 22], FCB_BASE);
    exec_comcod(buf, 0x4b);

    char path[512];
    snprintf(path, sizeof(path), "%s/IOCSTEST.TXT", host_dir);
    unlink(path);
}

static void test_seek(void) {
    uint8_t buf[512] = {0};
    set_namests(buf, NAMESTS_BASE, "README.TXT");
    set_be32(&buf[REQ_BASE + 14], NAMESTS_BASE);
    set_be32(&buf[REQ_BASE + 22], FCB_BASE);
    assert(exec_comcod(buf, 0x4a) == 0);

    memset(&buf[REQ_BASE], 0, 26);
    buf[REQ_BASE + 13] = 0; /* 先頭から */
    set_be32(&buf[REQ_BASE + 18], 3);
    set_be32(&buf[REQ_BASE + 22], FCB_BASE);
    assert(exec_comcod(buf, 0x4e) == 3);
    assert(get_be32(&buf[FCB_BASE + 6]) == 3);

    memset(&buf[REQ_BASE], 0, 26);
    buf[REQ_BASE + 13] = 2; /* 終端から */
    set_be32(&buf[REQ_BASE + 18], 100);
    set_be32(&buf[REQ_BASE + 22], FCB_BASE);
    assert(exec_comcod(buf, 0x4e) == -25); /* SEEK_OVER_EOF */

    memset(&buf[REQ_BASE], 0, 26);
    set_be32(&buf[REQ_BASE + 22], FCB_BASE);
    exec_comcod(buf, 0x4b);
}

static void test_close_unopened_fcb_is_noop(void) {
    uint8_t buf[512] = {0};
    set_be32(&buf[REQ_BASE + 22], 0xdeadu); /* 対応するfdの無いFCBアドレス */
    assert(exec_comcod(buf, 0x4b) == 0);
}

/* RESETは開いているFCBを無効化し、以後のREADをHANDLE_IS_NOT_OPENEDにする。 */
static void test_reset_invalidates_open_fcb(void) {
    uint8_t buf[512] = {0};
    set_namests(buf, NAMESTS_BASE, "README.TXT");
    set_be32(&buf[REQ_BASE + 14], NAMESTS_BASE);
    set_be32(&buf[REQ_BASE + 22], FCB_BASE);
    assert(exec_comcod(buf, 0x4a) == 0);

    hostdrv_remote_reset();
    memset(&buf[REQ_BASE], 0, 26);
    set_be32(&buf[REQ_BASE + 14], DATA_BASE);
    set_be32(&buf[REQ_BASE + 18], 1);
    set_be32(&buf[REQ_BASE + 22], FCB_BASE);
    assert(exec_comcod(buf, 0x4c) == -6);
}

/* $49 _CREATE(_NEWFILE)→$4d _WRITE→$4b _CLOSE→再度$4a _OPEN→$4c _READ で
 * 新規ファイルの内容が一致することを確認する。 */
static void test_create_write_read_roundtrip(void) {
    uint8_t buf[512] = {0};
    set_namests(buf, NAMESTS_BASE, "NEWFILE.TXT");
    set_be32(&buf[REQ_BASE + 14], NAMESTS_BASE);
    set_be32(&buf[REQ_BASE + 22], FCB_BASE);
    set_be32(&buf[REQ_BASE + 18], 0); /* _NEWFILE */
    assert(exec_comcod(buf, 0x49) == 0);
    assert(get_be32(&buf[FCB_BASE + 64]) == 0);

    memset(&buf[REQ_BASE], 0, 26);
    memcpy(&buf[DATA_BASE], "NEW CONTENT", 11);
    set_be32(&buf[REQ_BASE + 14], DATA_BASE);
    set_be32(&buf[REQ_BASE + 18], 11);
    set_be32(&buf[REQ_BASE + 22], FCB_BASE);
    assert(exec_comcod(buf, 0x4d) == 11);
    assert(get_be32(&buf[FCB_BASE + 6]) == 11);
    assert(get_be32(&buf[FCB_BASE + 64]) == 11); /* ファイルサイズが書き込み分拡張される */

    memset(&buf[REQ_BASE], 0, 26);
    set_be32(&buf[REQ_BASE + 22], FCB_BASE);
    assert(exec_comcod(buf, 0x4b) == 0);

    memset(&buf[REQ_BASE], 0, 26);
    set_namests(buf, NAMESTS_BASE, "NEWFILE.TXT");
    set_be32(&buf[REQ_BASE + 14], NAMESTS_BASE);
    set_be32(&buf[REQ_BASE + 22], FCB_BASE);
    assert(exec_comcod(buf, 0x4a) == 0);
    assert(get_be32(&buf[FCB_BASE + 64]) == 11);

    memset(&buf[REQ_BASE], 0, 26);
    set_be32(&buf[REQ_BASE + 14], DATA_BASE + 100);
    set_be32(&buf[REQ_BASE + 18], 64);
    set_be32(&buf[REQ_BASE + 22], FCB_BASE);
    assert(exec_comcod(buf, 0x4c) == 11);
    assert(memcmp(&buf[DATA_BASE + 100], "NEW CONTENT", 11) == 0);

    memset(&buf[REQ_BASE], 0, 26);
    set_be32(&buf[REQ_BASE + 22], FCB_BASE);
    exec_comcod(buf, 0x4b);
}

static void test_create_newfile_conflict(void) {
    uint8_t buf[512] = {0};
    set_namests(buf, NAMESTS_BASE, "README.TXT"); /* 既存ファイル */
    set_be32(&buf[REQ_BASE + 14], NAMESTS_BASE);
    set_be32(&buf[REQ_BASE + 22], FCB_BASE);
    set_be32(&buf[REQ_BASE + 18], 0);      /* _NEWFILE */
    assert(exec_comcod(buf, 0x49) == -80); /* FILE_EXISTS */
}

static void test_write_zero_truncates(void) {
    uint8_t buf[512] = {0};
    set_namests(buf, NAMESTS_BASE, "TRUNC.TXT");
    set_be32(&buf[REQ_BASE + 14], NAMESTS_BASE);
    set_be32(&buf[REQ_BASE + 22], FCB_BASE);
    set_be32(&buf[REQ_BASE + 18], 0); /* _NEWFILE */
    assert(exec_comcod(buf, 0x49) == 0);

    memset(&buf[REQ_BASE], 0, 26);
    memcpy(&buf[DATA_BASE], "0123456789", 10);
    set_be32(&buf[REQ_BASE + 14], DATA_BASE);
    set_be32(&buf[REQ_BASE + 18], 10);
    set_be32(&buf[REQ_BASE + 22], FCB_BASE);
    assert(exec_comcod(buf, 0x4d) == 10);

    memset(&buf[REQ_BASE], 0, 26); /* 位置3までシーク */
    buf[REQ_BASE + 13] = 0;
    set_be32(&buf[REQ_BASE + 18], 3);
    set_be32(&buf[REQ_BASE + 22], FCB_BASE);
    assert(exec_comcod(buf, 0x4e) == 3);

    memset(&buf[REQ_BASE], 0, 26); /* サイズ0の$4dは現在位置での切り詰め */
    set_be32(&buf[REQ_BASE + 18], 0);
    set_be32(&buf[REQ_BASE + 22], FCB_BASE);
    assert(exec_comcod(buf, 0x4d) == 0);
    assert(get_be32(&buf[FCB_BASE + 64]) == 3);

    memset(&buf[REQ_BASE], 0, 26);
    set_be32(&buf[REQ_BASE + 22], FCB_BASE);
    exec_comcod(buf, 0x4b);
}

static void test_filedate_read_and_write(void) {
    uint8_t buf[512] = {0};
    set_namests(buf, NAMESTS_BASE, "NEWFILE.TXT");
    set_be32(&buf[REQ_BASE + 14], NAMESTS_BASE);
    set_be32(&buf[REQ_BASE + 22], FCB_BASE);
    assert(exec_comcod(buf, 0x4a) == 0);

    memset(&buf[REQ_BASE], 0, 26);
    set_be32(&buf[REQ_BASE + 22], FCB_BASE);
    set_be32(&buf[REQ_BASE + 18], 0); /* 読み出し */
    assert(exec_comcod(buf, 0x4f) != -6);

    uint16_t hdate = (uint16_t)(((2020 - 1980) << 9) | (1 << 5) | 2); /* 2020-01-02 */
    uint16_t htime = (uint16_t)((3 << 11) | (4 << 5) | 0);            /* 03:04:00 */
    uint32_t param = ((uint32_t)hdate << 16) | htime;

    memset(&buf[REQ_BASE], 0, 26);
    set_be32(&buf[REQ_BASE + 22], FCB_BASE);
    set_be32(&buf[REQ_BASE + 18], param);
    assert(exec_comcod(buf, 0x4f) == (int32_t)param);

    memset(&buf[REQ_BASE], 0, 26);
    set_be32(&buf[REQ_BASE + 22], FCB_BASE);
    set_be32(&buf[REQ_BASE + 18], 0);
    assert(exec_comcod(buf, 0x4f) == (int32_t)param);

    memset(&buf[REQ_BASE], 0, 26);
    set_be32(&buf[REQ_BASE + 22], FCB_BASE);
    exec_comcod(buf, 0x4b);
}

static void test_chmod_write_readonly(const char *host_dir) {
    uint8_t buf[512] = {0};
    set_namests(buf, NAMESTS_BASE, "NEWFILE.TXT");
    set_be32(&buf[REQ_BASE + 14], NAMESTS_BASE);
    buf[REQ_BASE + 13] = ATTR_READONLY;
    assert(exec_comcod(buf, 0x46) ==
           (ATTR_ARCHIVE | ATTR_READONLY)); /* 通常ファイルなのでARCHIVEも立つ */

    char path[512];
    snprintf(path, sizeof(path), "%s/NEWFILE.TXT", host_dir);
    assert(access(path, W_OK) != 0);

    memset(&buf[REQ_BASE], 0, 26); /* 後続テストのため書き込み可能へ戻す */
    set_namests(buf, NAMESTS_BASE, "NEWFILE.TXT");
    set_be32(&buf[REQ_BASE + 14], NAMESTS_BASE);
    buf[REQ_BASE + 13] = 0;
    assert(exec_comcod(buf, 0x46) == ATTR_ARCHIVE);
}

static void test_rename(void) {
    uint8_t buf[900] = {0};
    const uint32_t new_namests = 700u;
    set_namests(buf, NAMESTS_BASE, "NEWFILE.TXT");
    set_be32(&buf[REQ_BASE + 14], NAMESTS_BASE);
    set_namests(buf, new_namests, "RENAMED.TXT");
    set_be32(&buf[REQ_BASE + 18], new_namests);
    assert(exec_comcod(buf, 0x44) == 0);

    memset(&buf[REQ_BASE], 0, 26); /* 旧名はもうオープンできない */
    set_namests(buf, NAMESTS_BASE, "NEWFILE.TXT");
    set_be32(&buf[REQ_BASE + 14], NAMESTS_BASE);
    set_be32(&buf[REQ_BASE + 22], FCB_BASE);
    assert(exec_comcod(buf, 0x4a) == -2);

    memset(&buf[REQ_BASE], 0, 26); /* 新名はオープンできる */
    set_namests(buf, NAMESTS_BASE, "RENAMED.TXT");
    set_be32(&buf[REQ_BASE + 14], NAMESTS_BASE);
    set_be32(&buf[REQ_BASE + 22], FCB_BASE);
    assert(exec_comcod(buf, 0x4a) == 0);
    memset(&buf[REQ_BASE], 0, 26);
    set_be32(&buf[REQ_BASE + 22], FCB_BASE);
    exec_comcod(buf, 0x4b);
}

static void test_delete(void) {
    uint8_t buf[512] = {0};
    set_namests(buf, NAMESTS_BASE, "RENAMED.TXT");
    set_be32(&buf[REQ_BASE + 14], NAMESTS_BASE);
    assert(exec_comcod(buf, 0x45) == 0);

    memset(&buf[REQ_BASE], 0, 26);
    set_namests(buf, NAMESTS_BASE, "RENAMED.TXT");
    set_be32(&buf[REQ_BASE + 14], NAMESTS_BASE);
    set_be32(&buf[REQ_BASE + 22], FCB_BASE);
    assert(exec_comcod(buf, 0x4a) == -2);
}

static void test_delete_nonexistent_fails(void) {
    uint8_t buf[512] = {0};
    set_namests(buf, NAMESTS_BASE, "NOSUCH2.TXT");
    set_be32(&buf[REQ_BASE + 14], NAMESTS_BASE);
    assert(exec_comcod(buf, 0x45) == -2);
}

int main(void) {
#ifdef _WIN32
    char host_dir[300];
    win_get_temp_template(host_dir, sizeof(host_dir), "hostdrv_remote_test.");
#else
    char host_dir[] = "/tmp/hostdrv_remote_test.XXXXXX";
#endif
    assert(mkdtemp(host_dir) != NULL);
    hostdrv_remote_configure(host_dir);

    test_mediacheck_returns_zero();
    test_verify_bit_masked();
    test_unknown_comcod();
    test_fflush_noop();
    test_getdpb_zeroes_buffer();
    test_dskfre_reports_free_bytes();
    test_drvctrl_mode0_sense();
    test_drvctrl_other_mode_is_noop();
    test_chdir_always_succeeds();
    test_files_empty_then_lists_created_file(host_dir);
    test_files_lists_japanese_filename(host_dir);
    test_nested_files_does_not_break_outer_search(host_dir);
    test_search_attr_selects_dirs_and_files_separately(host_dir);
    test_chmod_reports_attr();
    test_open_read_close_roundtrip();
    test_open_nonexistent_returns_not_found();
    test_open_exact_8char_name(host_dir);
    test_seek();
    test_close_unopened_fcb_is_noop();
    test_reset_invalidates_open_fcb();
    test_create_write_read_roundtrip();
    test_create_newfile_conflict();
    test_write_zero_truncates();
    test_filedate_read_and_write();
    test_chmod_write_readonly(host_dir);
    test_rename();
    test_delete();
    test_delete_nonexistent_fails();

    hostdrv_remote_reset();
    char path[512];
    snprintf(path, sizeof(path), "%s/README.TXT", host_dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/TOOLONGNAME.TXT", host_dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/TRUNC.TXT", host_dir);
    unlink(path);
    rmdir(host_dir);

    printf("hostdrv_remote_test: OK\n");
    return 0;
}
