#include "peripheral/media_fp.h"
#include "peripheral/sha256.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include "platform/win_compat.h"
#include <io.h>
#else
#include <unistd.h>
#endif

#define MEDIA_FP_IO_SIZE (1024u * 1024u)

static int64_t stat_mtime_ns(const struct stat *st) {
#ifdef __APPLE__
    return (int64_t)st->st_mtimespec.tv_sec * 1000000000LL + st->st_mtimespec.tv_nsec;
#elif defined(_WIN32)
    return (int64_t)st->st_mtime * 1000000000LL;
#else
    return (int64_t)st->st_mtim.tv_sec * 1000000000LL + st->st_mtim.tv_nsec;
#endif
}

static char *normalized_path(const char *path) {
#ifdef _WIN32
    char buf[_MAX_PATH];
    return _fullpath(buf, path, sizeof(buf)) ? strdup(buf) : NULL;
#else
    return realpath(path, NULL);
#endif
}

static bool fill_stat(media_file_fp_t *file, const struct stat *st) {
#ifdef _WIN32
    /* st_dev/st_ino は信用できない: _stat(path) の st_dev はドライブ番号、_fstat(fd) は 0 で、
     * 同じファイルでも取得経路により値が変わる。 */
    file->dev = 0;
    file->ino = 0;
#else
    file->dev = (uint64_t)st->st_dev;
    file->ino = (uint64_t)st->st_ino;
#endif
    file->size = (uint64_t)st->st_size;
    file->mtime_ns = stat_mtime_ns(st);
    return true;
}

static bool fingerprint_file(media_file_fp_t *out, const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return false;
    struct stat st;
    if (fstat(fileno(fp), &st) != 0) {
        fclose(fp);
        return false;
    }
    uint8_t *buf = malloc(MEDIA_FP_IO_SIZE);
    if (!buf) {
        fclose(fp);
        return false;
    }
    sha256_ctx_t hash;
    sha256_init(&hash);
    size_t n;
    while ((n = fread(buf, 1, MEDIA_FP_IO_SIZE, fp)) != 0)
        sha256_update(&hash, buf, n);
    bool ok = !ferror(fp);
    free(buf);
    fclose(fp);
    if (!ok)
        return false;
    char *normalized = normalized_path(path);
    if (!normalized)
        return false;
    out->path = normalized;
    fill_stat(out, &st);
    sha256_final(&hash, out->sha256);
    return true;
}

void media_fp_free(media_fp_t *fp) {
    for (int i = 0; i < MEDIA_FP_MAX_FILES; i++) {
        free(fp->files[i].path);
        fp->files[i].path = NULL;
    }
    memset(fp, 0, sizeof(*fp));
}

bool media_fp_compute(media_fp_t *out, const char *const *paths, int npaths) {
    media_fp_t tmp = {0};
    if (!out || !paths || npaths < 1 || npaths > MEDIA_FP_MAX_FILES)
        return false;
    for (int i = 0; i < npaths; i++) {
        if (!fingerprint_file(&tmp.files[i], paths[i])) {
            media_fp_free(&tmp);
            media_fp_free(out);
            return false;
        }
    }
    tmp.valid = true;
    tmp.nfiles = npaths;
    media_fp_free(out);
    *out = tmp;
    return true;
}

bool media_fp_compute_memory(media_fp_t *out, const void *data, size_t size, const char *path,
                             int fd) {
    if (!out || !data || !path || fd < 0)
        return false;
    struct stat st;
    if (fstat(fd, &st) != 0)
        return false;
    char *normalized = normalized_path(path);
    if (!normalized)
        return false;
    media_fp_t tmp = {0};
    tmp.files[0].path = normalized;
    fill_stat(&tmp.files[0], &st);
    sha256_ctx_t hash;
    sha256_init(&hash);
    sha256_update(&hash, data, size);
    sha256_final(&hash, tmp.files[0].sha256);
    tmp.valid = true;
    tmp.nfiles = 1;
    media_fp_free(out);
    *out = tmp;
    return true;
}

bool media_fp_clone(media_fp_t *dst, const media_fp_t *src) {
    if (!dst || !src)
        return false;
    media_fp_t tmp = {0};
    if (!src->valid) {
        media_fp_free(dst);
        return true;
    }
    tmp = *src;
    for (int i = 0; i < MEDIA_FP_MAX_FILES; i++)
        tmp.files[i].path = NULL;
    for (int i = 0; i < src->nfiles; i++) {
        tmp.files[i].path = strdup(src->files[i].path);
        if (!tmp.files[i].path) {
            media_fp_free(&tmp);
            return false;
        }
    }
    media_fp_free(dst);
    *dst = tmp;
    return true;
}

bool media_fp_equal(const media_fp_t *a, const media_fp_t *b) {
    if (!a || !b || !a->valid || !b->valid || a->nfiles != b->nfiles)
        return false;
    for (int i = 0; i < a->nfiles; i++) {
        const media_file_fp_t *af = &a->files[i], *bf = &b->files[i];
        if (strcmp(af->path, bf->path) != 0 || af->dev != bf->dev || af->ino != bf->ino ||
            af->size != bf->size || af->mtime_ns != bf->mtime_ns ||
            memcmp(af->sha256, bf->sha256, sizeof(af->sha256)) != 0)
            return false;
    }
    return true;
}

static bool matches_stat(const media_file_fp_t *file, const struct stat *st) {
    if ((uint64_t)st->st_size != file->size || stat_mtime_ns(st) != file->mtime_ns)
        return false;
#ifndef _WIN32
    if ((uint64_t)st->st_dev != file->dev || (uint64_t)st->st_ino != file->ino)
        return false;
#endif
    return true;
}

bool media_fp_matches_path(const media_fp_t *fp, int file_index) {
    if (!fp || !fp->valid || file_index < 0 || file_index >= fp->nfiles)
        return false;
    struct stat st;
    return stat(fp->files[file_index].path, &st) == 0 &&
           matches_stat(&fp->files[file_index], &st);
}

bool media_fp_matches_open_file(const media_fp_t *fp, int file_index, FILE *open_fp) {
    if (!fp || !fp->valid || !open_fp || file_index < 0 || file_index >= fp->nfiles)
        return false;
    struct stat st;
    return fstat(fileno(open_fp), &st) == 0 && matches_stat(&fp->files[file_index], &st);
}
