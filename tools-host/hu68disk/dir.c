/*
 * dir.c - Human68k ファイル名コーデック・ディレクトリ走査/確保/検索・パス解決・日時
 *
 * Human68k ネイティブ長名: 名前 1〜8 文字目を +0x00、9〜18 文字目を予約領域
 * +0x0C〜+0x15 に地続き格納（最大 18B + 拡張子 3B = 21B）。VFAT ダミーエントリは
 * 作らない。名前は Shift-JIS バイト列。
 */
/* localtime_r は C99 だけでは宣言されない（POSIX） */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "hu68disk.h"
#include <stdio.h>
#include <string.h>

const dir_t DIR_ROOT = {1, 0};

/* ---- ファイル名 分解/エンコード/デコード（SJIS バイト列） ---- */
/* NAME_MAX_* は「バイト数」。SJIS の 2 バイト目に 0x2E('.') が来ても区切りと
 * 誤認しないよう、リードバイトを飛ばして最後の '.' を探す。 */
int split_name(const char *fname, char *base, char *ext) {
    size_t len = strlen(fname);
    long dot = -1;
    for (size_t i = 0; i < len;) {
        unsigned char c = (unsigned char)fname[i];
        if (is_sjis_lead(c) && i + 1 < len) {
            i += 2;
            continue;
        }
        if (c == '.')
            dot = (long)i;
        i++;
    }
    size_t blen, elen;
    const char *estr;
    if (dot > 0) {
        blen = (size_t)dot;
        estr = fname + dot + 1;
        elen = len - (size_t)dot - 1;
    } else {
        blen = len;
        estr = "";
        elen = 0;
    }
    if (blen == 0 || blen > NAME_MAX_BASE) {
        fprintf(stderr,
                "error: 名前部が %zu バイト。1〜%d バイト（全角 %d 文字）にしてください\n",
                blen, NAME_MAX_BASE, NAME_MAX_BASE / 2);
        return -1;
    }
    if (elen > NAME_MAX_EXT) {
        fprintf(stderr, "error: 拡張子が %zu バイト。0〜%d バイトにしてください\n", elen,
                NAME_MAX_EXT);
        return -1;
    }
    /* 制御文字・Human68k が拒否する文字（0x20 未満, : ? *）を単バイト位置のみ検査 */
    for (size_t i = 0; i < len;) {
        unsigned char c = (unsigned char)fname[i];
        if (is_sjis_lead(c) && i + 1 < len) {
            i += 2;
            continue;
        }
        if (c < 0x20 || c == ':' || c == '?' || c == '*') {
            fprintf(stderr, "error: ファイル名に使えない文字が含まれています (0x%02X)\n", c);
            return -1;
        }
        i++;
    }
    memset(base, 0, NAME_MAX_BASE + 1);
    memset(ext, 0, NAME_MAX_EXT + 1);
    memcpy(base, fname, blen);
    if (elen)
        memcpy(ext, estr, elen);
    return 0;
}

void encode_name(uint8_t *e, const char *base, const char *ext) {
    memset(e + DE_NAME, ' ', 8);
    memset(e + DE_EXT, ' ', 3);
    memset(e + DE_NAME2, 0x00, 10);
    size_t blen = strlen(base);
    size_t n1 = blen > 8 ? 8 : blen;
    memcpy(e + DE_NAME, base, n1);
    if (blen > 8)
        memcpy(e + DE_NAME2, base + 8, blen - 8); /* 9〜18 文字目 */
    memcpy(e + DE_EXT, ext, strlen(ext));
    if (e[DE_NAME] == 0xE5)
        e[DE_NAME] = 0x05; /* $E5 実名は $05 に */
}

void decode_name(const uint8_t *e, char *out) {
    char base[NAME_MAX_BASE + 1] = {0}, ext[NAME_MAX_EXT + 1] = {0};
    int bi = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t c = e[DE_NAME + i];
        if (c == 0x20)
            break;
        base[bi++] = (i == 0 && c == 0x05) ? (char)0xE5 : (char)c;
    }
    for (int i = 0; i < 10; i++) {
        uint8_t c = e[DE_NAME2 + i];
        if (c == 0x00 || c == 0x20)
            break;
        base[bi++] = (char)c;
    }
    int ei = 0;
    for (int i = 0; i < 3; i++) {
        uint8_t c = e[DE_EXT + i];
        if (c == 0x20)
            break;
        ext[ei++] = (char)c;
    }
    if (ext[0])
        snprintf(out, NAMEBUF, "%s.%s", base, ext);
    else
        snprintf(out, NAMEBUF, "%s", base);
}

/* SJIS バイト列どうしの照合。ASCII 単バイトのみ大文字化して比較（Human68k は
 * 英字を大小同一視）。全角 2 バイトはそのまま比較。 */
int name_ieq(const char *a, const char *b) {
    size_t i = 0, j = 0;
    for (;;) {
        unsigned char ca = (unsigned char)a[i], cb = (unsigned char)b[j];
        if (ca == 0 || cb == 0)
            return ca == cb;
        if (is_sjis_lead(ca) && a[i + 1] && is_sjis_lead(cb) && b[j + 1]) {
            if (a[i] != b[j] || a[i + 1] != b[j + 1])
                return 0;
            i += 2;
            j += 2;
            continue;
        }
        if (is_sjis_lead(ca) || is_sjis_lead(cb))
            return 0; /* 片方だけ全角 */
        if (ca >= 'a' && ca <= 'z')
            ca -= 32;
        if (cb >= 'a' && cb <= 'z')
            cb -= 32;
        if (ca != cb)
            return 0;
        i++;
        j++;
    }
}

void fat_datetime(time_t t, uint16_t *fdate, uint16_t *ftime) {
    struct tm lt;
#if defined(_WIN32)
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    int year = lt.tm_year + 1900;
    if (year < 1980)
        year = 1980;
    *fdate = (uint16_t)(((year - 1980) << 9) | ((lt.tm_mon + 1) << 5) | lt.tm_mday);
    *ftime = (uint16_t)((lt.tm_hour << 11) | (lt.tm_min << 5) | (lt.tm_sec / 2));
}

/* fat_datetime() の逆変換（copyall でコピー元の日時をそのまま引き継ぐために使う）。
 * fdate==0（日時未設定）は現在時刻にフォールバックする。 */
time_t fat_datetime_to_unix(uint16_t fdate, uint16_t ftime) {
    if (!fdate)
        return time(NULL);
    struct tm lt = {0};
    lt.tm_year = ((fdate >> 9) & 0x7F) + 1980 - 1900;
    lt.tm_mon = ((fdate >> 5) & 0x0F) - 1;
    lt.tm_mday = fdate & 0x1F;
    lt.tm_hour = (ftime >> 11) & 0x1F;
    lt.tm_min = (ftime >> 5) & 0x3F;
    lt.tm_sec = (ftime & 0x1F) * 2;
    lt.tm_isdst = -1;
    return mktime(&lt);
}

/* ---- ディレクトリ抽象 ---- */
void dir_from_cluster(uint32_t cl, dir_t *out) {
    out->is_root = (cl == 0);
    out->first_cluster = cl;
}

/* dir 内の各エントリを cb に渡す。cb 非0 で停止しそのエントリを返す。0x00 終端で NULL。 */
uint8_t *dir_scan(image_t *im, const dir_t *d, int (*cb)(uint8_t *e, void *ctx), void *ctx) {
    if (d->is_root) {
        uint8_t *root = image_ptr(im, (uint64_t)im->root_start_sec * im->bytes_per_sector,
                                  (size_t)im->root_entries * DE_SIZE_B);
        for (uint32_t i = 0; i < im->root_entries; i++) {
            uint8_t *e = root + (size_t)i * DE_SIZE_B;
            if (e[DE_NAME] == 0x00)
                return NULL;
            if (cb(e, ctx))
                return e;
        }
        return NULL;
    }
    uint32_t per = im->cluster_bytes / DE_SIZE_B;
    uint32_t cl = d->first_cluster;
    while (cl >= 2 && !fat_is_eoc(im, cl)) {
        uint8_t *base = cluster_ptr(im, cl);
        for (uint32_t i = 0; i < per; i++) {
            uint8_t *e = base + (size_t)i * DE_SIZE_B;
            if (e[DE_NAME] == 0x00)
                return NULL;
            if (cb(e, ctx))
                return e;
        }
        cl = fat_get(im, cl);
    }
    return NULL;
}

/* 空きスロット（0x00 か 0xE5）を確保。サブディレクトリが満杯ならクラスタを足す。 */
uint8_t *dir_alloc_slot(image_t *im, const dir_t *d) {
    if (d->is_root) {
        uint8_t *root = image_ptr(im, (uint64_t)im->root_start_sec * im->bytes_per_sector,
                                  (size_t)im->root_entries * DE_SIZE_B);
        for (uint32_t i = 0; i < im->root_entries; i++) {
            uint8_t *e = root + (size_t)i * DE_SIZE_B;
            if (e[DE_NAME] == 0x00 || e[DE_NAME] == 0xE5)
                return e;
        }
        return NULL;
    }
    uint32_t per = im->cluster_bytes / DE_SIZE_B;
    uint32_t cl = d->first_cluster, last = cl;
    while (cl >= 2 && !fat_is_eoc(im, cl)) {
        uint8_t *base = cluster_ptr(im, cl);
        for (uint32_t i = 0; i < per; i++) {
            uint8_t *e = base + (size_t)i * DE_SIZE_B;
            if (e[DE_NAME] == 0x00 || e[DE_NAME] == 0xE5)
                return e;
        }
        last = cl;
        cl = fat_get(im, cl);
    }
    uint32_t nc = fat_find_free(im, 2);
    if (!nc)
        return NULL;
    fat_set(im, nc, fat_eoc(im));
    fat_set(im, last, nc);
    uint8_t *base = cluster_ptr(im, nc);
    memset(base, 0, im->cluster_bytes);
    return base;
}

/* 名前一致検索。want_dir: 1=DIR のみ, 0=非DIR のみ, -1=どちらでも。 */
struct findctx {
    const char *name;
    int want_dir;
};
static int cb_find(uint8_t *e, void *ctx) {
    struct findctx *f = ctx;
    if (e[DE_NAME] == 0xE5)
        return 0;
    if (e[DE_ATTR] == ATTR_LFN)
        return 0;
    if (e[DE_ATTR] & ATTR_VOLUME)
        return 0;
    char nm[NAMEBUF];
    decode_name(e, nm);
    if (!name_ieq(nm, f->name))
        return 0;
    if (f->want_dir == 1 && !(e[DE_ATTR] & ATTR_DIR))
        return 0;
    if (f->want_dir == 0 && (e[DE_ATTR] & ATTR_DIR))
        return 0;
    return 1;
}
uint8_t *dir_find(image_t *im, const dir_t *d, const char *name, int want_dir) {
    struct findctx f = {name, want_dir};
    return dir_scan(im, d, cb_find, &f);
}

/* ---- パス解決（SJIS バイト列、'/' 区切り） ---- */
int path_split(const char *path, char comps[16][32]) {
    int n = 0;
    const char *p = path;
    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        const char *s = p;
        while (*p && *p != '/')
            p++;
        size_t len = (size_t)(p - s);
        if (len >= 32)
            len = 31;
        if (n >= 16)
            return -1;
        memcpy(comps[n], s, len);
        comps[n][len] = 0;
        n++;
    }
    return n;
}
int descend(image_t *im, const dir_t *in, const char *name, dir_t *out) {
    if (strcmp(name, ".") == 0) {
        *out = *in;
        return 0;
    }
    uint8_t *e = dir_find(im, in, name, 1);
    if (!e)
        return -1;
    dir_from_cluster(rd16(e + DE_CLUSTER), out);
    return 0;
}
/* path 全体をディレクトリとして解決（"/" はルート）。0=OK,-1=無い */
int resolve_dir(image_t *im, const char *path, dir_t *out) {
    char comps[16][32];
    int n = path_split(path, comps);
    if (n < 0)
        return -1;
    dir_t d = DIR_ROOT;
    for (int i = 0; i < n; i++)
        if (descend(im, &d, comps[i], &d))
            return -1;
    *out = d;
    return 0;
}
/* path を「親ディレクトリ + 末尾要素(leaf)」に分解 */
int resolve_parent(image_t *im, const char *path, dir_t *parent, char *leaf, size_t leafsz) {
    char comps[16][32];
    int n = path_split(path, comps);
    if (n < 0) {
        fprintf(stderr, "error: パスが深すぎます\n");
        return -1;
    }
    if (n == 0) {
        fprintf(stderr, "error: パスが空です\n");
        return -1;
    }
    dir_t d = DIR_ROOT;
    for (int i = 0; i < n - 1; i++) {
        if (descend(im, &d, comps[i], &d)) {
            fprintf(stderr, "error: ディレクトリ '%s' が見つかりません\n", comps[i]);
            return -1;
        }
    }
    *parent = d;
    snprintf(leaf, leafsz, "%s", comps[n - 1]);
    return 0;
}
