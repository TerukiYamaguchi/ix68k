#include "hostdrv_remote.h"
#include "hostdrv_remote_fd.h"
#include "sjis.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include "platform/win_compat.h" /* dirent/statvfs/strcasecmp/POSIX I/O 相当 */
#else
#include <dirent.h>
#include <strings.h>
#include <sys/statvfs.h>
#include <unistd.h>
#ifndef O_BINARY
#define O_BINARY 0 /* POSIXにテキスト/バイナリの区別は無い（Windows専用フラグの素通し用） */
#endif
/* win_compat_stat: WindowsのUTF-8→UTF-16変換ラッパーと呼び分けを
 * 揃えるための別名。POSIX側はネイティブstat()で問題ないためそのまま使う。 */
#define win_compat_stat stat
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0 /* 未定義の環境（Windows）では素通し */
#endif

/* Human68k属性ワード下位バイト（X68k_OSWORK_Data_Crystal.html FCB表・
 * X68k_DOSCALL_Data_Crystal.html準拠）。 */
#define HOSTDRV_ATTR_READONLY  0x01u
#define HOSTDRV_ATTR_HIDDEN    0x02u
#define HOSTDRV_ATTR_SYSTEM    0x04u
#define HOSTDRV_ATTR_VOLUME    0x08u
#define HOSTDRV_ATTR_DIRECTORY 0x10u
#define HOSTDRV_ATTR_ARCHIVE   0x20u

/* Human68k 3.02 素の状態の主ファイル名・拡張子の最大長。
 * _NAMESTSの+67(8B)+78(10B)を連結して主ファイル名18バイトまで表現できる。 */
#define HOSTDRV_NAME_LEN    18
#define HOSTDRV_EXT_LEN     3
#define HOSTDRV_PATTERN_LEN (HOSTDRV_NAME_LEN + HOSTDRV_EXT_LEN)

static char s_host_dir[512];
static int s_configured = 0;

void hostdrv_remote_configure(const char *host_dir) {
    strncpy(s_host_dir, host_dir, sizeof(s_host_dir) - 1);
    s_host_dir[sizeof(s_host_dir) - 1] = '\0';
    s_configured = 1;
}

int hostdrv_remote_is_configured(void) {
    return s_configured;
}

static void mem_write32(const hostdrv_rd_mem_t *mem, uint32_t addr, uint32_t val) {
    mem->write8(mem->ctx, addr, (uint8_t)(val >> 24));
    mem->write8(mem->ctx, addr + 1, (uint8_t)(val >> 16));
    mem->write8(mem->ctx, addr + 2, (uint8_t)(val >> 8));
    mem->write8(mem->ctx, addr + 3, (uint8_t)val);
}

static void mem_write16(const hostdrv_rd_mem_t *mem, uint32_t addr, uint16_t val) {
    mem->write8(mem->ctx, addr, (uint8_t)(val >> 8));
    mem->write8(mem->ctx, addr + 1, (uint8_t)val);
}

static uint32_t mem_read32(const hostdrv_rd_mem_t *mem, uint32_t addr) {
    return ((uint32_t)mem->read8(mem->ctx, addr) << 24) |
           ((uint32_t)mem->read8(mem->ctx, addr + 1) << 16) |
           ((uint32_t)mem->read8(mem->ctx, addr + 2) << 8) |
           (uint32_t)mem->read8(mem->ctx, addr + 3);
}

/* 32bit値の先頭0ビット数を返す（value==0のときのみ32を返す。__builtin_clzは
 * value==0が未定義動作のため使わない）。$50 _DSKFRE のクラスタサイズ計算専用。 */
static uint8_t count_leading_zeros(uint32_t value) {
    if (value == 0)
        return 32;
    uint8_t count = 0;
    for (; (int32_t)value >= 0; count++)
        value <<= 1;
    return count;
}

/* $50 _DSKFRE: ホストディレクトリの空き容量を統計し、8バイトバッファへ
 * 空きクラスタ数/総クラスタ数/1クラスタのセクタ数/1セクタのバイト数を書く。
 * クラスタ数が16bitに収まるようクラスタサイズを動的に決める（XEiJ HFS.java
 * hfuCallDskfreX68k() と同一アルゴリズム）。 */
static void hostdrv_dskfre(const hostdrv_rd_mem_t *mem, uint32_t buf,
                           uint32_t *out_free_bytes) {
    uint64_t free_bytes64 = 0, total_bytes64 = 0;
#ifdef _WIN32
    win_get_disk_free(s_host_dir, &free_bytes64, &total_bytes64);
#else
    struct statvfs sv;
    if (statvfs(s_host_dir, &sv) == 0) {
        free_bytes64 = (uint64_t)sv.f_bavail * (uint64_t)sv.f_frsize;
        total_bytes64 = (uint64_t)sv.f_blocks * (uint64_t)sv.f_frsize;
    }
#endif
    uint32_t total_space = total_bytes64 > 0x7fffffffu ? 0x7fffffffu : (uint32_t)total_bytes64;
    uint32_t free_space = free_bytes64 > 0x7fffffffu ? 0x7fffffffu : (uint32_t)free_bytes64;
    int cluster_bit = total_space == 0 ? 0 : 7 - (int)count_leading_zeros(total_space);
    if (cluster_bit < 0)
        cluster_bit = 0;
    mem_write16(mem, buf + 0, (uint16_t)(free_space >> (cluster_bit + 10)));
    mem_write16(mem, buf + 2, (uint16_t)(total_space >> (cluster_bit + 10)));
    mem_write16(mem, buf + 4, (uint16_t)(1u << cluster_bit));
    mem_write16(mem, buf + 6, 1u << 10);
    *out_free_bytes = free_space;
}

/* $51 _DRVCTRL: モード0（センス）のみ実装。それ以外のモード（イジェクト等）は
 * ホストディレクトリに対応する概念が無いため無処理で成功を返す。 */
static uint8_t hostdrv_drvctrl_sense(void) {
    uint8_t sense = 0x02u; /* ABU_INSERTED: ホストディレクトリは常に「挿入済み」扱い */
    if (access(s_host_dir, W_OK) != 0)
        sense |= 0x08u; /* ABU_WRITE_PROTECTED */
    return sense;
}

/* _NAMESTS の主ファイル名(+67 8B、' 'パディング)+続き(+78 10B、0x00パディング)+
 * 拡張子(+75,3B、' 'パディング)を、検索パターンとしてそのまま使える形
 * （末尾は必ず' '、ワイルドカードは'?'のまま）で取り出す。
 *
 * +67が8文字ちょうどの場合も+78の有無を判別できないため、常に連結して
 * 末尾の0x00を空白へ正規化する(XEiJ HFS.java hfuNamestsToPath()準拠)。 */
static void namests_read_pattern(const hostdrv_rd_mem_t *mem, uint32_t namests,
                                 uint8_t out[HOSTDRV_PATTERN_LEN]) {
    for (int i = 0; i < 8; i++)
        out[i] = mem->read8(mem->ctx, namests + 67 + i);
    for (int i = 0; i < 3; i++)
        out[HOSTDRV_NAME_LEN + i] = mem->read8(mem->ctx, namests + 75 + i);
    for (int i = 0; i < 10; i++)
        out[8 + i] = mem->read8(mem->ctx, namests + 78 + i);
    for (int i = 17; i >= 8 && out[i] == 0; i--)
        out[i] = ' ';
}

/* SJISバイト列(sjis)を18+3形式(空白パディング)へ変換する。大文字化はしない
 * （大小文字を無視したマッチはmatch_pattern()側で行い、保存時は元の表記を維持する。
 * ここで大文字化すると、readdir()由来の実名をそのままhostdrv_write_files_entry()
 * 経由で_FILES応答へ書き込む経路で、DIR表示が常に大文字になってしまう）。
 * 18+3を超える名前は非対応として0を返す
 * （XEiJ HFS.java L1730 と同じ割り切り）。 */
static int sjis_to_pattern(const char *sjis, uint8_t out[HOSTDRV_PATTERN_LEN]) {
    const char *dot = strrchr(sjis, '.');
    size_t namelen, extlen;
    const char *ext;
    if (dot == sjis)
        return 0;
    if (dot != NULL) {
        namelen = (size_t)(dot - sjis);
        ext = dot + 1;
        extlen = strlen(ext);
    } else {
        namelen = strlen(sjis);
        ext = "";
        extlen = 0;
    }
    if (namelen == 0 || namelen > HOSTDRV_NAME_LEN || extlen > HOSTDRV_EXT_LEN)
        return 0;
    memset(out, ' ', HOSTDRV_PATTERN_LEN);
    size_t i = 0;
    while (i < namelen) {
        unsigned char c = (unsigned char)sjis[i];
        if (is_sjis_lead(c) && i + 1 < namelen) {
            out[i] = c;
            out[i + 1] = (uint8_t)sjis[i + 1];
            i += 2;
        } else {
            out[i] = c;
            i++;
        }
    }
    i = 0;
    while (i < extlen) {
        unsigned char c = (unsigned char)ext[i];
        if (is_sjis_lead(c) && i + 1 < extlen) {
            out[HOSTDRV_NAME_LEN + i] = c;
            out[HOSTDRV_NAME_LEN + i + 1] = (uint8_t)ext[i + 1];
            i += 2;
        } else {
            out[HOSTDRV_NAME_LEN + i] = c;
            i++;
        }
    }
    return 1;
}

/* ホスト側の実ファイル名(UTF-8)をSJISへ変換した上で18+3パターンへ変換する。
 * 変換失敗・先頭が'.'の名前（dotfile）は非対応として0を返す。 */
static int name_to_pattern(const char *filename, uint8_t out[HOSTDRV_PATTERN_LEN]) {
    if (strcmp(filename, ".") == 0 || strcmp(filename, "..") == 0)
        return 0; /* readdir()が返す自己参照エントリを除外する */
    char sjis[128];
    if (utf8_to_sjis(filename, sjis, sizeof(sjis)) < 0)
        return 0;
    return sjis_to_pattern(sjis, out);
}

/* FCB形式のワイルドカード一致判定（名前部・拡張子部を別々に走査し、SJIS
 * 2バイト文字は大文字小文字比較を行わずそのまま一致させる）。パターン側の
 * '?' は候補側の1文字（空白パディングも含む）に無条件一致する。 */
static int match_pattern(const uint8_t pattern[HOSTDRV_PATTERN_LEN],
                         const uint8_t candidate[HOSTDRV_PATTERN_LEN]) {
    static const int seg_start[2] = {0, HOSTDRV_NAME_LEN};
    static const int seg_len[2] = {HOSTDRV_NAME_LEN, HOSTDRV_EXT_LEN};
    for (int seg = 0; seg < 2; seg++) {
        int start = seg_start[seg], len = seg_len[seg];
        int i = 0;
        while (i < len) {
            int idx = start + i;
            if (is_sjis_lead(candidate[idx]) && i + 1 < len) {
                if (pattern[idx] != '?' &&
                    (pattern[idx] != candidate[idx] || pattern[idx + 1] != candidate[idx + 1]))
                    return 0;
                i += 2;
                continue;
            }
            if (pattern[idx] != '?' && toupper(pattern[idx]) != toupper(candidate[idx]))
                return 0;
            i++;
        }
    }
    return 1;
}

static uint8_t hostdrv_attr_for_stat(const struct stat *st) {
    uint8_t attr = S_ISDIR(st->st_mode) ? HOSTDRV_ATTR_DIRECTORY : HOSTDRV_ATTR_ARCHIVE;
    if (!(st->st_mode & S_IWUSR))
        attr |= HOSTDRV_ATTR_READONLY;
    return attr;
}

/* macOS errno を DOS エラーへ変換する。 */
static int32_t hostdrv_errno_to_dos(int err) {
    switch (err) {
    case ENOENT:
        return -2; /* FILE_NOT_FOUND */
    case ENOTDIR:
        return -3; /* DIRECTORY_NOT_FOUND */
    case EISDIR:
        return -5; /* NOT_A_FILE */
    case EEXIST:
        return -80; /* FILE_EXISTS */
    case ENOTEMPTY:
        return -21; /* RM_NONEMPTY_DIRECTORY */
    case EACCES:
    case EPERM:
    case EROFS:
        return -19; /* CANNOT_WRITE */
    case EBADF:
        return -6; /* HANDLE_IS_NOT_OPENED */
    case ENOSPC:
    case EDQUOT:
        return -23; /* DISK_FULL */
    case EMFILE:
    case ENFILE:
        return -4; /* TOO_MANY_HANDLES */
    case ENAMETOOLONG:
    case EILSEQ:
        return -13; /* ILLEGAL_FILE_NAME */
    default:
        return -14; /* INVALID_PARAMETER */
    }
}

/* ホスト側で1つのパス要素として安全に使える名前か判定する。パターンはゲストメモリ由来で
 * 任意のバイトを含みうるため、区切り文字（'/'・'\\'・':'）、"."・".."、制御文字を含む名前を
 * 拒否し、s_host_dir の外を指すパスを組めないようにする。判定はSJIS→UTF-8変換後に行う
 * （SJIS 2バイト目の0x5Cを'\\'と誤認しないため）。 */
static int hostdrv_name_is_safe(const char *name) {
    if (name[0] == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if (*p < 0x20 || *p == 0x7f || *p == '/' || *p == '\\' || *p == ':')
            return 0;
    }
    return 1;
}

/* base_dir/name がシンボリックリンクなら1を返す。共有フォルダ内のリンクを辿ると
 * s_host_dir の外を読み書きできてしまうため、HOSTDRVからはリンクを見せない。
 * Windowsのリパースポイント（ジャンクション等）は未対応。 */
static int hostdrv_is_symlink(const char *base_dir, const char *name) {
#ifdef _WIN32
    (void)base_dir;
    (void)name;
    return 0;
#else
    char path[600];
    snprintf(path, sizeof(path), "%s/%s", base_dir, name);
    struct stat st;
    return lstat(path, &st) == 0 && S_ISLNK(st.st_mode);
#endif
}

/* 18+3形式(空白パディング)のSJISパターンを、UTF-8の"NAME.EXT"文字列へ変換する
 * （$42/$43/$44/$49の新規ファイル名生成用。ワイルドカードを含まない具体名を想定）。
 * SJIS→UTF-8変換に失敗した場合、または hostdrv_name_is_safe() を満たさない場合は
 * 空文字列にして0を返す。成功時は1。 */
static int pattern_to_filename(const uint8_t p[HOSTDRV_PATTERN_LEN], char *out, size_t out_sz) {
    char sjis[HOSTDRV_PATTERN_LEN + 2];
    size_t n = 0;
    for (int i = 0; i < HOSTDRV_NAME_LEN && p[i] != ' '; i++)
        sjis[n++] = (char)p[i];
    if (p[HOSTDRV_NAME_LEN] != ' ') {
        sjis[n++] = '.';
        for (int i = HOSTDRV_NAME_LEN; i < HOSTDRV_PATTERN_LEN && p[i] != ' '; i++)
            sjis[n++] = (char)p[i];
    }
    sjis[n] = '\0';
    if (sjis_to_utf8(sjis, n, out, out_sz) < 0 || !hostdrv_name_is_safe(out)) {
        out[0] = '\0';
        return 0;
    }
    return 1;
}

/* mtimeをHuman68kの日付・時刻ワード形式（MS-DOS形式と同一）へ変換する。 */
static void hostdrv_time_to_human68k(time_t t, uint16_t *out_time, uint16_t *out_date) {
    struct tm tmv;
    localtime_r(&t, &tmv);
    int year = tmv.tm_year + 1900 - 1980;
    if (year < 0)
        year = 0;
    if (year > 127)
        year = 127;
    *out_date = (uint16_t)((year << 9) | ((tmv.tm_mon + 1) << 5) | tmv.tm_mday);
    *out_time = (uint16_t)((tmv.tm_hour << 11) | (tmv.tm_min << 5) | (tmv.tm_sec / 2));
}

/* base_dir直下でpattern(ワイルドカード無し想定)に一致する実ファイル/ディレクトリ名を
 * 大小無視で探す。見つかれば1、out_nameへ実名（macOS上の実際の大小文字）をコピーする。 */
static int hostdrv_find_name_exact(const char *base_dir,
                                   const uint8_t pattern[HOSTDRV_PATTERN_LEN], char *out_name,
                                   size_t out_sz) {
    DIR *dirp = opendir(base_dir);
    if (dirp == NULL)
        return 0;
    struct dirent *de;
    int found = 0;
    while ((de = readdir(dirp)) != NULL) {
        uint8_t cand[HOSTDRV_PATTERN_LEN];
        if (!name_to_pattern(de->d_name, cand))
            continue;
        if (match_pattern(pattern, cand)) {
            if (hostdrv_is_symlink(base_dir, de->d_name))
                continue;
            strncpy(out_name, de->d_name, out_sz - 1);
            out_name[out_sz - 1] = '\0';
            found = 1;
            break;
        }
    }
    closedir(dirp);
    return found;
}

/* _NAMESTS の NAMPTH（+2〜、$09区切り・$00終端）を解決し、
 * ホスト側の絶対ディレクトリパスを out_dir へ書く。要素ごとに
 * 大小無視でホスト側ディレクトリを探し、実際の大小文字を採用する。".."は
 * s_host_dir を越えて遡らせず、公開ルートからの脱出を防ぐ。 */
static int hostdrv_resolve_dir(const hostdrv_rd_mem_t *mem, uint32_t namests, char *out_dir,
                               size_t out_sz) {
    strncpy(out_dir, s_host_dir, out_sz - 1);
    out_dir[out_sz - 1] = '\0';
    size_t root_len = strlen(s_host_dir);

    uint32_t p = namests + 2;
    for (;;) {
        uint8_t c = mem->read8(mem->ctx, p);
        if (c != 0x09)
            break;
        p++;
        char comp[256];
        int n = 0;
        while ((c = mem->read8(mem->ctx, p)) != 0 && c != 0x09) {
            if (n + 1 < (int)sizeof(comp))
                comp[n++] = (char)c;
            p++;
        }
        comp[n] = '\0';
        if (n == 0 || strcmp(comp, ".") == 0)
            continue;
        if (strcmp(comp, "..") == 0) {
            char *slash = strrchr(out_dir, '/');
            if (slash == NULL || (size_t)(slash - out_dir) < root_len)
                return 0; /* ルート脱出禁止 */
            *slash = '\0';
            continue;
        }
        uint8_t pattern[HOSTDRV_PATTERN_LEN];
        if (!sjis_to_pattern(comp, pattern))
            return 0;
        char found[256];
        if (!hostdrv_find_name_exact(out_dir, pattern, found, sizeof(found)))
            return 0;
        char path[600];
        snprintf(path, sizeof(path), "%s/%s", out_dir, found);
        struct stat st;
        if (win_compat_stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
            return 0;
        size_t len = strlen(out_dir);
        if (len + 1 + strlen(found) + 1 > out_sz)
            return 0;
        out_dir[len] = '/';
        strcpy(out_dir + len + 1, found);
    }
    return 1;
}

/* NAMESTS全体（NAMPTHで親ディレクトリを解決し、+67/+75のパターンでファイルを探す）
 * をホスト側の実パスへ解決する。戻り値: 0=成功、-3=ディレクトリ解決失敗、
 * -2=ファイル未発見。$44/$45/$46/$4aで共用。 */
static int32_t hostdrv_resolve_full(const hostdrv_rd_mem_t *mem, uint32_t namests,
                                    char *out_path, size_t out_sz) {
    char dir[600];
    if (!hostdrv_resolve_dir(mem, namests, dir, sizeof(dir)))
        return -3;
    uint8_t pattern[HOSTDRV_PATTERN_LEN];
    namests_read_pattern(mem, namests, pattern);
    char name[256];
    if (!hostdrv_find_name_exact(dir, pattern, name, sizeof(name)))
        return -2;
    snprintf(out_path, out_sz, "%s/%s", dir, name);
    return 0;
}

/* $47/$48 の検索結果1件（ホスト実名・18+3パターン・属性・日時・サイズ）。 */
typedef struct {
    char name[256];
    uint8_t pattern[HOSTDRV_PATTERN_LEN];
    uint8_t attr;
    uint16_t time, date;
    uint32_t size;
} hostdrv_dirent_t;

/* _FILES/_NFILES の検索セッション。列挙の途中で別の_FILESが割り込んでも壊れないよう、
 * セッションはグローバルに1つ持つのではなく_FILESバッファ(FILBUF)ごとに持つ。
 * FILBUFの+2にマジック、+6にセッション番号を埋め込み、_NFILESはそこから
 * セッションを引く（参照実装XEiJ HFS.java hfuCallFilesX68k()/hfuCallNfiles()と同じ方式）。
 * +2〜+9はHuman68kのFAT検索用フィールド(dirsec/dirlft/dirpos)で、リモートドライブでは
 * Human68k自身が使わないため自由に使える。
 *
 * mint.xの再帰コピー(cpmv.s)は _FILES → cpmv_sub内のネストした_FILES → _NFILES の順に
 * 呼ぶため、セッションが1つしか無いと外側の列挙が1件目で打ち切られる。 */
#define HOSTDRV_FILES_MAGIC        0x2a484653u /* '*HFS' */
#define HOSTDRV_FILBUF_MAGIC_OFF   2u
#define HOSTDRV_FILBUF_KEY_OFF     6u
#define HOSTDRV_SEARCH_SESSION_NUM 16

typedef struct {
    uint32_t key; /* 0=未使用。1から単調増加する通し番号 */
    hostdrv_dirent_t *entries;
    int count;
    int index;
} hostdrv_search_t;

static hostdrv_search_t s_searches[HOSTDRV_SEARCH_SESSION_NUM];
static uint32_t s_search_key_seq;

static int hostdrv_dirent_cmp(const void *a, const void *b) {
    return strcmp(((const hostdrv_dirent_t *)a)->name, ((const hostdrv_dirent_t *)b)->name);
}

static void hostdrv_search_free(hostdrv_search_t *s) {
    free(s->entries);
    s->entries = NULL;
    s->count = 0;
    s->index = 0;
    s->key = 0;
}

static void hostdrv_search_reset_all(void) {
    for (int i = 0; i < HOSTDRV_SEARCH_SESSION_NUM; i++)
        hostdrv_search_free(&s_searches[i]);
}

/* 新しいセッションを確保する。空きが無ければ最も古い（通し番号が最小の）セッションを
 * 破棄して再利用する。該当するのは列挙を最後まで辿らずに放棄されたセッションで、
 * 最後まで辿られたセッションは終端時に自動的に解放される。 */
static hostdrv_search_t *hostdrv_search_alloc(void) {
    hostdrv_search_t *slot = NULL;
    for (int i = 0; i < HOSTDRV_SEARCH_SESSION_NUM; i++) {
        if (s_searches[i].key == 0) {
            slot = &s_searches[i];
            break;
        }
    }
    if (slot == NULL) {
        slot = &s_searches[0];
        for (int i = 1; i < HOSTDRV_SEARCH_SESSION_NUM; i++) {
            if (s_searches[i].key < slot->key)
                slot = &s_searches[i];
        }
        hostdrv_search_free(slot);
    }
    if (++s_search_key_seq == 0)
        s_search_key_seq = 1; /* 0は「未使用」の意味なので飛ばす */
    slot->key = s_search_key_seq;
    return slot;
}

static hostdrv_search_t *hostdrv_search_find(uint32_t key) {
    if (key == 0)
        return NULL;
    for (int i = 0; i < HOSTDRV_SEARCH_SESSION_NUM; i++) {
        if (s_searches[i].key == key)
            return &s_searches[i];
    }
    return NULL;
}

/* FILBUFに埋め込んだセッション情報を消す。使い回されたスタック上のFILBUFに
 * 古いマジックが残って別セッションを拾うことを防ぐ。 */
static void hostdrv_filbuf_clear_session(const hostdrv_rd_mem_t *mem, uint32_t buf) {
    mem_write32(mem, buf + HOSTDRV_FILBUF_MAGIC_OFF, 0);
    mem_write32(mem, buf + HOSTDRV_FILBUF_KEY_OFF, 0);
}

/* ルートディレクトリ(ホストのマウント先フォルダ)の末尾フォルダ名をボリューム
 * ラベルとして合成する（参照実装XEiJ HFS.java hfuCallFilesHost()の「ボリューム名を
 * 追加する」処理と同じ発想。マウントパス全体ではなく末尾フォルダ名を使う）。
 * SJIS変換後18バイトに
 * 収まらない場合は文字境界を保ったまま切り詰める。生成できなければ0を返す。 */
static int hostdrv_make_volume_entry(hostdrv_dirent_t *out) {
    char dir[512];
    strncpy(dir, s_host_dir, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    size_t len = strlen(dir);
    while (len > 1 && dir[len - 1] == '/')
        dir[--len] = '\0';
    char *base = strrchr(dir, '/');
    base = base ? base + 1 : dir;
    if (base[0] == '\0')
        return 0;

    char sjis[128];
    if (utf8_to_sjis(base, sjis, sizeof(sjis)) < 0)
        return 0;
    size_t sjis_len = strlen(sjis);
    if (sjis_len > HOSTDRV_NAME_LEN) {
        size_t i = 0;
        while (i < HOSTDRV_NAME_LEN) {
            unsigned char c = (unsigned char)sjis[i];
            if (is_sjis_lead(c) && i + 1 < sjis_len && i + 2 <= HOSTDRV_NAME_LEN)
                i += 2;
            else if (is_sjis_lead(c) && i + 1 < sjis_len)
                break; /* 2バイト文字がちょうど境界をまたぐ: ここで打ち切る */
            else
                i++;
        }
        sjis[i] = '\0';
        sjis_len = i;
    }
    if (sjis_len == 0)
        return 0;

    memset(out->pattern, ' ', HOSTDRV_PATTERN_LEN);
    memcpy(out->pattern, sjis, sjis_len);
    out->name[0] = '\0';
    out->attr = HOSTDRV_ATTR_VOLUME;
    out->size = 0;
    struct stat st;
    if (win_compat_stat(s_host_dir, &st) == 0)
        hostdrv_time_to_human68k(st.st_mtime, &out->time, &out->date);
    else {
        out->time = 0;
        out->date = 0;
    }
    return 1;
}

/* 検索属性(search_attr)は「1を指定したビットが1個でも一致すればそのファイルを返す」
 * というORマッチ（X68k DOSCALL資料の_FILES記述。参照実装
 * XEiJ HFS.java L2074も同じ判定）。MS-DOSのFindFirstのように「READONLY/ARCHIVEは
 * 無条件で対象」と解釈してはいけない。mmdsp.rはディレクトリ属性($10)とファイル属性
 * ($20)の2パスで同じディレクトリを走査して結果を1つのリストへ追記する
 * （mmd_files.s FNAME_SET）ため、通常ファイルが両方のパスにヒットすると
 * 一覧が二重になる。VOLUME単独指定はボリュームラベル専用検索
 * （DIRコマンドがルートディレクトリに対しこの属性で_FILESを呼ぶことを
 * COMMAND.S 5248-5251行目のROM逆アセンブルで確認済み）として扱い、ルートかつ
 * パターンが"*.*"のときだけホストディレクトリ名から合成した1件を返す。 */
static void hostdrv_search_collect(hostdrv_search_t *s, const char *base_dir,
                                   const uint8_t pattern[HOSTDRV_PATTERN_LEN],
                                   uint8_t search_attr) {
    if (search_attr == HOSTDRV_ATTR_VOLUME) {
        if (strcmp(base_dir, s_host_dir) == 0 && pattern[0] == '?' &&
            pattern[HOSTDRV_NAME_LEN] == '?') {
            hostdrv_dirent_t vol;
            if (hostdrv_make_volume_entry(&vol)) {
                s->entries = malloc(sizeof(hostdrv_dirent_t));
                s->entries[0] = vol;
                s->count = 1;
            }
        }
        return;
    }

    DIR *dirp = opendir(base_dir);
    if (dirp == NULL)
        return;

    size_t cap = 16;
    s->entries = malloc(cap * sizeof(hostdrv_dirent_t));

    struct dirent *de;
    while ((de = readdir(dirp)) != NULL) {
        uint8_t cand[HOSTDRV_PATTERN_LEN];
        if (!name_to_pattern(de->d_name, cand))
            continue;
        if (!match_pattern(pattern, cand))
            continue;
        if (hostdrv_is_symlink(base_dir, de->d_name))
            continue;

        char path[600];
        snprintf(path, sizeof(path), "%s/%s", base_dir, de->d_name);
        struct stat st;
        if (win_compat_stat(path, &st) != 0)
            continue;
        uint8_t attr = hostdrv_attr_for_stat(&st);
        if ((attr & search_attr) == 0)
            continue;

        if ((size_t)s->count >= cap) {
            cap *= 2;
            s->entries = realloc(s->entries, cap * sizeof(hostdrv_dirent_t));
        }
        hostdrv_dirent_t *e = &s->entries[s->count++];
        strncpy(e->name, de->d_name, sizeof(e->name) - 1);
        e->name[sizeof(e->name) - 1] = '\0';
        memcpy(e->pattern, cand, HOSTDRV_PATTERN_LEN);
        e->attr = attr;
        hostdrv_time_to_human68k(st.st_mtime, &e->time, &e->date);
        e->size = (uint32_t)st.st_size;
    }
    closedir(dirp);

    qsort(s->entries, (size_t)s->count, sizeof(hostdrv_dirent_t), hostdrv_dirent_cmp);
}

/* _FILES バッファ（141バイト拡張形式のうち先頭53バイト）へ1件書き込む。
 * +30 は「主名.拡張子」+0（18+3対応で最大23バイト）。 */
static void hostdrv_write_files_entry(const hostdrv_rd_mem_t *mem, uint32_t buf,
                                      const hostdrv_dirent_t *e) {
    mem->write8(mem->ctx, buf + 21, e->attr);
    mem_write16(mem, buf + 22, e->time);
    mem_write16(mem, buf + 24, e->date);
    mem_write32(mem, buf + 26, e->size);

    int p = 0;
    for (int i = 0; i < HOSTDRV_NAME_LEN && e->pattern[i] != ' '; i++)
        mem->write8(mem->ctx, buf + 30 + (uint32_t)p++, e->pattern[i]);
    if (e->pattern[HOSTDRV_NAME_LEN] != ' ') {
        mem->write8(mem->ctx, buf + 30 + (uint32_t)p++, '.');
        for (int i = HOSTDRV_NAME_LEN; i < HOSTDRV_PATTERN_LEN && e->pattern[i] != ' '; i++)
            mem->write8(mem->ctx, buf + 30 + (uint32_t)p++, e->pattern[i]);
    }
    mem->write8(mem->ctx, buf + 30 + (uint32_t)p, 0);
}

/* $4a _OPEN・$49 _CREATE 成功時の FCB 書き戻し。+6 ファイルポインタ、
 * +32 最終アクセスポインタ、
 * +36/+44 主名前半/拡張子、+47 属性、+48 主名後半(9〜18文字目)、+58/+60 時刻/日付、
 * +62 先頭FAT番号(リモートデバイスでは無関係のため0)、+64 ファイルサイズ。 */
static void hostdrv_fcb_fill_on_open(const hostdrv_rd_mem_t *mem, uint32_t fcb,
                                     const uint8_t cand[HOSTDRV_PATTERN_LEN], uint8_t attr,
                                     time_t mtime, uint32_t size) {
    mem_write32(mem, fcb + 6, 0);
    mem_write32(mem, fcb + 32, 0);
    for (int i = 0; i < 8; i++)
        mem->write8(mem->ctx, fcb + 36 + (uint32_t)i, cand[i]);
    for (int i = 0; i < 3; i++)
        mem->write8(mem->ctx, fcb + 44 + (uint32_t)i, cand[HOSTDRV_NAME_LEN + i]);
    mem->write8(mem->ctx, fcb + 47, attr);
    for (int i = 0; i < 10; i++)
        mem->write8(mem->ctx, fcb + 48 + (uint32_t)i, cand[8 + i]);
    uint16_t htime, hdate;
    hostdrv_time_to_human68k(mtime, &htime, &hdate);
    mem_write16(mem, fcb + 58, htime);
    mem_write16(mem, fcb + 60, hdate);
    mem_write16(mem, fcb + 62, 0);
    mem_write32(mem, fcb + 64, size);
}

/* リセット時に開いているホストfdと検索セッションを破棄する。 */
void hostdrv_remote_reset(void) {
    hostdrv_fd_table_reset();
    hostdrv_search_reset_all();
}

/* リクエストヘッダ: +2 comcod（bit7=VERIFYフラグ、&$7fしてから分岐）、
 * +3 errl、+4 errh、+18 リザルトステータス。$40 初期化はドライバ側で完結するため
 * ここには来ない想定だが、来た場合も安全側で INVALID_FUNCTION にする。 */
static void hostdrv_remote_exec(hostdrv_remote_t *st, const hostdrv_rd_mem_t *mem) {
    uint32_t req = st->reg_a5;
    uint8_t comcod = mem->read8(mem->ctx, req + 2) & 0x7fu;

    switch (comcod) {
    case 0x41: { /* _CHDIR: NAMPTHで指定ディレクトリの存在を確認する */
        uint32_t namests = mem_read32(mem, req + 14);
        char dir[600];
        mem_write32(mem, req + 18,
                    hostdrv_resolve_dir(mem, namests, dir, sizeof(dir)) ? 0 : (uint32_t)-3);
        break;
    }
    case 0x42: { /* _MKDIR: NAMPTH=親ディレクトリ、+67/+75=新規ディレクトリ名 */
        uint32_t namests = mem_read32(mem, req + 14);
        char parent[600];
        if (!hostdrv_resolve_dir(mem, namests, parent, sizeof(parent))) {
            mem_write32(mem, req + 18, (uint32_t)-3); /* DIRECTORY_NOT_FOUND */
            break;
        }
        uint8_t pattern[HOSTDRV_PATTERN_LEN];
        namests_read_pattern(mem, namests, pattern);
        char name[128];
        if (!pattern_to_filename(pattern, name, sizeof(name))) {
            mem_write32(mem, req + 18, (uint32_t)-13); /* ILLEGAL_FILE_NAME */
            break;
        }
        char path[600];
        snprintf(path, sizeof(path), "%s/%s", parent, name);
        int32_t result = 0;
        if (mkdir(path, 0777) != 0)
            result = errno == EEXIST ? -20 /* DIRECTORY_EXISTS */ : hostdrv_errno_to_dos(errno);
        mem_write32(mem, req + 18, (uint32_t)result);
        break;
    }
    case 0x43: { /* _RMDIR */
        uint32_t namests = mem_read32(mem, req + 14);
        char parent[600];
        if (!hostdrv_resolve_dir(mem, namests, parent, sizeof(parent))) {
            mem_write32(mem, req + 18, (uint32_t)-3);
            break;
        }
        uint8_t pattern[HOSTDRV_PATTERN_LEN];
        namests_read_pattern(mem, namests, pattern);
        char name[128];
        if (!pattern_to_filename(pattern, name, sizeof(name))) {
            mem_write32(mem, req + 18, (uint32_t)-13); /* ILLEGAL_FILE_NAME */
            break;
        }
        char path[600];
        snprintf(path, sizeof(path), "%s/%s", parent, name);
        int32_t result = 0;
        if (rmdir(path) != 0)
            result = errno == ENOTEMPTY ? -21 /* RM_NONEMPTY_DIRECTORY */ : -3;
        mem_write32(mem, req + 18, (uint32_t)result);
        break;
    }
    case 0x44: { /* _RENAME: +14=旧名NAMESTS、+18=入力(新名NAMESTS)→出力(結果) */
        uint32_t old_namests = mem_read32(mem, req + 14);
        uint32_t new_namests = mem_read32(mem, req + 18);
        char old_path[600];
        int32_t rc = hostdrv_resolve_full(mem, old_namests, old_path, sizeof(old_path));
        if (rc != 0) {
            mem_write32(mem, req + 18, (uint32_t)rc);
            break;
        }
        char new_dir[600];
        if (!hostdrv_resolve_dir(mem, new_namests, new_dir, sizeof(new_dir))) {
            mem_write32(mem, req + 18, (uint32_t)-3);
            break;
        }
        uint8_t new_pattern[HOSTDRV_PATTERN_LEN];
        namests_read_pattern(mem, new_namests, new_pattern);
        char new_name[128];
        if (!pattern_to_filename(new_pattern, new_name, sizeof(new_name))) {
            mem_write32(mem, req + 18, (uint32_t)-13); /* ILLEGAL_FILE_NAME */
            break;
        }
        char new_path[600];
        snprintf(new_path, sizeof(new_path), "%s/%s", new_dir, new_name);

        struct stat st;
        if (strcasecmp(old_path, new_path) != 0 && win_compat_stat(new_path, &st) == 0) {
            mem_write32(mem, req + 18, (uint32_t)-80); /* FILE_EXISTS */
            break;
        }
        mem_write32(
            mem, req + 18,
            (uint32_t)(rename(old_path, new_path) == 0 ? 0 : hostdrv_errno_to_dos(errno)));
        break;
    }
    case 0x45: { /* _DELETE */
        uint32_t namests = mem_read32(mem, req + 14);
        char path[600];
        int32_t rc = hostdrv_resolve_full(mem, namests, path, sizeof(path));
        if (rc != 0) {
            mem_write32(mem, req + 18, (uint32_t)rc);
            break;
        }
        mem_write32(mem, req + 18,
                    (uint32_t)(unlink(path) == 0 ? 0 : hostdrv_errno_to_dos(errno)));
        break;
    }
    case 0x46: { /* _CHMOD: +13=$FFなら属性照会、それ以外はREADONLYビットのみ反映 */
        uint32_t namests = mem_read32(mem, req + 14);
        uint8_t new_attr = mem->read8(mem->ctx, req + 13);
        char path[600];
        int32_t rc = hostdrv_resolve_full(mem, namests, path, sizeof(path));
        if (rc != 0) {
            mem_write32(mem, req + 18, (uint32_t)rc);
            break;
        }
        struct stat st;
        if (win_compat_stat(path, &st) != 0) {
            mem_write32(mem, req + 18, (uint32_t)-2);
            break;
        }
        if (new_attr != 0xFFu) {
            /* ホストファイルシステムに対応概念があるのはREADONLYのみ。
             * 他の属性ビット(SYSTEM/HIDDEN等)は無視する。ディレクトリに実行(x)
             * ビット無しのモードを適用するとトラバース不能になる（LZH展開時の
             * 属性復元でディレクトリにも_CHMODが飛んで再現）ため、ファイルと
             * ディレクトリでモードを分ける。 */
            mode_t mode;
            if (S_ISDIR(st.st_mode))
                mode = (new_attr & HOSTDRV_ATTR_READONLY) ? 0555 : 0755;
            else
                mode = (new_attr & HOSTDRV_ATTR_READONLY) ? 0444 : 0644;
            if (chmod(path, mode) != 0) {
                mem_write32(mem, req + 18, (uint32_t)hostdrv_errno_to_dos(errno));
                break;
            }
            if (win_compat_stat(path, &st) != 0) {
                mem_write32(mem, req + 18, (uint32_t)-2);
                break;
            }
        }
        mem_write32(mem, req + 18, hostdrv_attr_for_stat(&st));
        break;
    }
    case 0x47:   /* _FILES */
    case 0x48: { /* _NFILES */
        uint32_t buf =
            mem_read32(mem, req + 18); /* +18は入力(バッファ)→出力(結果)を兼ねるため先に退避 */
        hostdrv_search_t *s;
        if (comcod == 0x47) {
            uint32_t namests = mem_read32(mem, req + 14);
            uint8_t search_attr = mem->read8(mem->ctx, req + 13);
            char dir[600];
            if (!hostdrv_resolve_dir(mem, namests, dir, sizeof(dir))) {
                hostdrv_filbuf_clear_session(mem, buf);
                mem_write32(mem, req + 18, (uint32_t)-3); /* DIRECTORY_NOT_FOUND */
                break;
            }
            uint8_t pattern[HOSTDRV_PATTERN_LEN];
            namests_read_pattern(mem, namests, pattern);
            s = hostdrv_search_alloc();
            hostdrv_search_collect(s, dir, pattern, search_attr);
            if (s->count == 0) {
                /* XEiJ準拠: _FILESで0件ならFILE_NOT_FOUND、_NFILESで検索終了なら
                 * NO_MORE_FILESを返す。mint.xはこの差で空ディレクトリを識別する。 */
                hostdrv_search_free(s);
                hostdrv_filbuf_clear_session(mem, buf);
                mem_write32(mem, req + 18, (uint32_t)-2); /* FILE_NOT_FOUND */
                break;
            }
            mem_write32(mem, buf + HOSTDRV_FILBUF_MAGIC_OFF, HOSTDRV_FILES_MAGIC);
            mem_write32(mem, buf + HOSTDRV_FILBUF_KEY_OFF, s->key);
        } else {
            if (mem_read32(mem, buf + HOSTDRV_FILBUF_MAGIC_OFF) != HOSTDRV_FILES_MAGIC)
                s = NULL; /* _FILESを経ていないFILBUF */
            else
                s = hostdrv_search_find(mem_read32(mem, buf + HOSTDRV_FILBUF_KEY_OFF));
            if (s == NULL) {
                mem_write32(mem, req + 18, (uint32_t)-18); /* NO_MORE_FILES */
                break;
            }
        }
        hostdrv_write_files_entry(mem, buf, &s->entries[s->index]);
        s->index++;
        if (s->index >= s->count) {
            /* 最後の1件を返し終えたらセッションを畳む。次の_NFILESはマジックが
             * 消えているためNO_MORE_FILESになる（XEiJ HFS.javaと同じ）。 */
            hostdrv_search_free(s);
            hostdrv_filbuf_clear_session(mem, buf);
        }
        mem_write32(mem, req + 18, 0);
        break;
    }
    case 0x49: { /* _CREATE:
                    +13=作成属性(未使用)、+18=入力(0=_NEWFILE/1=_CREATE)→出力(結果)、+22=FCB */
        uint32_t namests = mem_read32(mem, req + 14);
        uint32_t newfile_flag = mem_read32(mem, req + 18);
        uint32_t fcb = mem_read32(mem, req + 22);

        char dir[600];
        if (!hostdrv_resolve_dir(mem, namests, dir, sizeof(dir))) {
            mem_write32(mem, req + 18, (uint32_t)-3);
            break;
        }
        uint8_t pattern[HOSTDRV_PATTERN_LEN];
        namests_read_pattern(mem, namests, pattern);
        char found[256];
        int exists = hostdrv_find_name_exact(dir, pattern, found, sizeof(found));
        char generated[128];
        const char *use_name = found;
        if (!exists) {
            if (!pattern_to_filename(pattern, generated, sizeof(generated))) {
                mem_write32(mem, req + 18, (uint32_t)-13); /* ILLEGAL_FILE_NAME */
                break;
            }
            use_name = generated;
        }

        char path[600];
        snprintf(path, sizeof(path), "%s/%s", dir, use_name);
        /* hostdrv_find_name_exact() はリンクを返さないため、同名のリンクがあると
         * exists=0 のまま O_TRUNC で開くことになる。O_NOFOLLOW でリンク先を切り詰めない。 */
        int flags =
            O_CREAT | O_RDWR | O_BINARY | O_NOFOLLOW | (newfile_flag == 0 ? O_EXCL : O_TRUNC);
        int fd = open(path, flags, 0666);
        if (fd < 0) {
            mem_write32(mem, req + 18, (uint32_t)hostdrv_errno_to_dos(errno));
            break;
        }
        hostdrv_fd_table_put(fcb, fd);

        struct stat st;
        fstat(fd, &st);
        uint8_t cand[HOSTDRV_PATTERN_LEN];
        name_to_pattern(use_name, cand);
        hostdrv_fcb_fill_on_open(mem, fcb, cand, hostdrv_attr_for_stat(&st), st.st_mtime,
                                 (uint32_t)st.st_size);

        mem_write32(mem, req + 18, 0);
        break;
    }
    case 0x4a: { /* _OPEN */
        uint32_t namests = mem_read32(mem, req + 14);
        uint32_t fcb = mem_read32(mem, req + 22);
        char path[600];
        int32_t rc = hostdrv_resolve_full(mem, namests, path, sizeof(path));
        if (rc != 0) {
            mem_write32(mem, req + 18, (uint32_t)rc);
            break;
        }
        struct stat st;
        if (win_compat_stat(path, &st) != 0) {
            mem_write32(mem, req + 18, (uint32_t)-2);
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            mem_write32(mem, req + 18, (uint32_t)-5); /* NOT_A_FILE */
            break;
        }
        uint8_t mode = mem->read8(mem->ctx, fcb + 14); /* FCB+14=アクセスモード(OSWORK参照) */
        int flags = (mode == 1 ? O_WRONLY : mode == 2 ? O_RDWR : O_RDONLY) | O_BINARY;
#ifdef _WIN32
        /* 通常のopen()（_wopen経由）が作るHANDLEはO_RDONLYの場合
         * FILE_WRITE_ATTRIBUTES権限を持たず、後続の$4F _FILEDATE（日時設定）が
         * SetFileTime()失敗で常にエラーになる。HOSTDRV _OPEN専用の経路だけ
         * FILE_WRITE_ATTRIBUTESを常時付与する。 */
        int fd = win_compat_open_hostdrv(path, flags);
#else
        int fd = open(path, flags | O_NOFOLLOW);
#endif
        if (fd < 0) {
            mem_write32(mem, req + 18, (uint32_t)-2);
            break;
        }
        hostdrv_fd_table_put(fcb, fd);

        uint8_t cand[HOSTDRV_PATTERN_LEN];
        char *slash = strrchr(path, '/');
        name_to_pattern(slash ? slash + 1 : path, cand);
        hostdrv_fcb_fill_on_open(mem, fcb, cand, hostdrv_attr_for_stat(&st), st.st_mtime,
                                 (uint32_t)st.st_size);

        mem_write32(mem, req + 18, 0);
        break;
    }
    case 0x4b: { /* _CLOSE: 未オープンFCBのcloseもエラーにせず0を返す（二重close対策） */
        uint32_t fcb = mem_read32(mem, req + 22);
        int fd = hostdrv_fd_table_get(fcb);
        if (fd >= 0) {
            close(fd);
            hostdrv_fd_table_remove(fcb);
        }
        mem_write32(mem, req + 18, 0);
        break;
    }
    case 0x4c: { /* _READ: シーク位置はFCB+6（ドライバ管理、Human68k側は追跡しない） */
        uint32_t buf = mem_read32(mem, req + 14);
        uint32_t want = mem_read32(mem, req + 18);
        uint32_t fcb = mem_read32(mem, req + 22);
        int fd = hostdrv_fd_table_get(fcb);
        if (fd < 0) {
            mem_write32(mem, req + 18, (uint32_t)-6); /* HANDLE_IS_NOT_OPENED */
            break;
        }
        uint32_t pos = mem_read32(mem, fcb + 6);
        uint32_t filesize = mem_read32(mem, fcb + 64);
        uint32_t remain = pos < filesize ? filesize - pos : 0;
        uint32_t n = want < remain ? want : remain;
        uint32_t done = 0;
        uint8_t chunk[4096];
        while (done < n) {
            uint32_t chunklen = n - done < sizeof(chunk) ? n - done : (uint32_t)sizeof(chunk);
            ssize_t r = pread(fd, chunk, chunklen, (off_t)(pos + done));
            if (r <= 0)
                break;
            for (ssize_t i = 0; i < r; i++)
                mem->write8(mem->ctx, buf + done + (uint32_t)i, chunk[i]);
            done += (uint32_t)r;
            if ((size_t)r < chunklen)
                break;
        }
        mem_write32(mem, fcb + 6, pos + done);
        mem_write32(mem, fcb + 32, pos + done);
        mem_write32(mem, req + 18, done);
        break;
    }
    case 0x4d: { /* _WRITE: サイズ0は現在位置でftruncate()に落とす（切り詰め） */
        uint32_t buf = mem_read32(mem, req + 14);
        uint32_t want = mem_read32(mem, req + 18);
        uint32_t fcb = mem_read32(mem, req + 22);
        int fd = hostdrv_fd_table_get(fcb);
        if (fd < 0) {
            mem_write32(mem, req + 18, (uint32_t)-6); /* HANDLE_IS_NOT_OPENED */
            break;
        }
        uint32_t pos = mem_read32(mem, fcb + 6);
        if (want == 0) {
            if (ftruncate(fd, (off_t)pos) != 0) {
                mem_write32(mem, req + 18, (uint32_t)hostdrv_errno_to_dos(errno));
                break;
            }
            mem_write32(mem, fcb + 64, pos);
            mem_write32(mem, req + 18, 0);
            break;
        }
        uint32_t done = 0;
        uint8_t chunk[4096];
        while (done < want) {
            uint32_t chunklen =
                want - done < sizeof(chunk) ? want - done : (uint32_t)sizeof(chunk);
            for (uint32_t i = 0; i < chunklen; i++)
                chunk[i] = mem->read8(mem->ctx, buf + done + i);
            ssize_t w = pwrite(fd, chunk, chunklen, (off_t)(pos + done));
            if (w <= 0)
                break;
            done += (uint32_t)w;
            if ((size_t)w < chunklen)
                break;
        }
        uint32_t newpos = pos + done;
        mem_write32(mem, fcb + 6, newpos);
        mem_write32(mem, fcb + 32, newpos);
        if (newpos > mem_read32(mem, fcb + 64))
            mem_write32(mem, fcb + 64, newpos); /* ファイル拡張 */
        mem_write32(mem, req + 18, done == 0 ? (uint32_t)-23 /* DISK_FULL */ : done);
        break;
    }
    case 0x4e: { /* _SEEK: ホスト側lseek不要。FCB+6(現在位置)/+64(サイズ)だけで完結 */
        uint8_t mode = mem->read8(mem->ctx, req + 13);
        int32_t offset = (int32_t)mem_read32(mem, req + 18);
        uint32_t fcb = mem_read32(mem, req + 22);
        uint32_t pos = mem_read32(mem, fcb + 6);
        uint32_t filesize = mem_read32(mem, fcb + 64);
        int64_t newpos = 0;
        if (mode == 0)
            newpos = offset;
        else if (mode == 1)
            newpos = (int64_t)pos + offset;
        else if (mode == 2)
            newpos = (int64_t)filesize + offset;

        int32_t result;
        if (mode > 2) {
            result = -14; /* INVALID_PARAMETER */
        } else if (newpos < 0 || newpos > (int64_t)filesize) {
            result = -25; /* SEEK_OVER_EOF */
        } else {
            mem_write32(mem, fcb + 6, (uint32_t)newpos);
            mem_write32(mem, fcb + 32, (uint32_t)newpos);
            result = (int32_t)newpos;
        }
        mem_write32(mem, req + 18, (uint32_t)result);
        break;
    }
    case 0x4f: { /* _FILEDATE:
                    +18=入力(0=読み出し/非0=日付<<16|時刻を設定)→出力(日付<<16|時刻)、+22=FCB */
        uint32_t fcb = mem_read32(mem, req + 22);
        uint32_t param = mem_read32(mem, req + 18);
        int fd = hostdrv_fd_table_get(fcb);
        if (fd < 0) {
            mem_write32(mem, req + 18, (uint32_t)-6); /* HANDLE_IS_NOT_OPENED */
            break;
        }
        if (param == 0) {
            struct stat st;
            if (fstat(fd, &st) != 0) {
                mem_write32(mem, req + 18, (uint32_t)hostdrv_errno_to_dos(errno));
                break;
            }
            uint16_t htime, hdate;
            hostdrv_time_to_human68k(st.st_mtime, &htime, &hdate);
            mem_write32(mem, req + 18, ((uint32_t)hdate << 16) | htime);
            break;
        }
        uint16_t hdate = (uint16_t)(param >> 16);
        uint16_t htime = (uint16_t)param;
        struct tm tmv;
        memset(&tmv, 0, sizeof(tmv));
        tmv.tm_year = ((hdate >> 9) & 0x7f) + 1980 - 1900;
        tmv.tm_mon = ((hdate >> 5) & 0x0f) - 1;
        tmv.tm_mday = hdate & 0x1f;
        tmv.tm_hour = (htime >> 11) & 0x1f;
        tmv.tm_min = (htime >> 5) & 0x3f;
        tmv.tm_sec = (htime & 0x1f) * 2;
        tmv.tm_isdst = -1;
        time_t t = mktime(&tmv);
        struct timespec times[2];
        times[0].tv_sec = t;
        times[0].tv_nsec = 0;
        times[1].tv_sec = t;
        times[1].tv_nsec = 0;
        if (futimens(fd, times) != 0) {
            mem_write32(mem, req + 18, (uint32_t)hostdrv_errno_to_dos(errno));
            break;
        }
        mem_write32(mem, req + 18, param);
        break;
    }
    case 0x50: { /* _DSKFRE */
        uint32_t buf = mem_read32(mem, req + 14);
        uint32_t free_bytes;
        hostdrv_dskfre(mem, buf, &free_bytes);
        mem_write32(mem, req + 18, free_bytes);
        break;
    }
    case 0x51: { /* _DRVCTRL */
        uint8_t mode = mem->read8(mem->ctx, req + 13);
        if (mode == 0)
            mem->write8(mem->ctx, req + 13, hostdrv_drvctrl_sense());
        mem_write32(mem, req + 18, 0);
        break;
    }
    case 0x52: { /* _GETDPB: 16バイトのDPB相当を全て0で埋める（1セクタバイト数0=特殊デバイス）
                  */
        uint32_t buf = mem_read32(mem, req + 14);
        mem_write32(mem, buf, 0);
        mem_write32(mem, buf + 4, 0);
        mem_write32(mem, buf + 8, 0);
        mem_write32(mem, buf + 12, 0);
        mem_write32(mem, req + 18, 0);
        break;
    }
    case 0x56: /* _FFLUSH: 明示的なfsyncは行わない。 */
        mem_write32(mem, req + 18, 0);
        break;
    case 0x57: /* メディアチェック: 常に交換なし */
        mem_write32(mem, req + 18, 0);
        break;
    default:
        /* 未実装 comcod は +18 = -1 (INVALID_FUNCTION)。通常のファイル操作エラーは
         * +18 の負値で返し、中断ダイアログを出す +3/+4 は使わない。 */
        mem_write32(mem, req + 18, (uint32_t)-1);
        break;
    }

    mem->write8(mem->ctx, req + 3, 0);
    mem->write8(mem->ctx, req + 4, 0);

    st->status = 0;
    st->reg_d0 = 0;
}

uint8_t hostdrv_remote_reg_read8(const hostdrv_remote_t *st, uint32_t off) {
    switch (off) {
    case 0x00:
        return hostdrv_remote_is_configured() ? 0x48u : 0x00u; /* MAGIC 上位 'H' */
    case 0x01:
        return hostdrv_remote_is_configured() ? 0x44u : 0x00u; /* MAGIC 下位 'D' */
    case 0x03:
        return st->status;
    case 0x08:
        return (uint8_t)(st->reg_d0 >> 8);
    case 0x09:
        return (uint8_t)st->reg_d0;
    default:
        return 0x00u;
    }
}

void hostdrv_remote_reg_write8(hostdrv_remote_t *st, const hostdrv_rd_mem_t *mem, uint32_t off,
                               uint8_t val) {
    switch (off) {
    case 0x02: /* COMMAND */
        if (val != HOSTDRV_RD_CMD_EXEC)
            return;
        /* ウィンドウは hostdrv_dir の設定に関係なくバスに出ている。未設定のときは
         * s_host_dir が空文字列で、ルート "/" 起点のパスを組んでしまうため処理しない
         * （MAGIC も 0 を返すので、正規の HOSTDRV.SYS はここに来ない）。 */
        if (!s_configured) {
            st->status = 0x01u;
            return;
        }
        hostdrv_remote_exec(st, mem);
        return;
    case 0x04:
        st->reg_a5 = (st->reg_a5 & 0x00FFFFFFu) | ((uint32_t)val << 24);
        return;
    case 0x05:
        st->reg_a5 = (st->reg_a5 & 0xFF00FFFFu) | ((uint32_t)val << 16);
        return;
    case 0x06:
        st->reg_a5 = (st->reg_a5 & 0xFFFF00FFu) | ((uint32_t)val << 8);
        return;
    case 0x07:
        st->reg_a5 = (st->reg_a5 & 0xFFFFFF00u) | (uint32_t)val;
        return;
    default:
        return;
    }
}
