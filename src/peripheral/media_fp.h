#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define MEDIA_FP_MAX_FILES 2

typedef struct {
    char *path;
    uint64_t dev, ino;
    uint64_t size;
    int64_t mtime_ns;
    uint8_t sha256[32];
} media_file_fp_t;

typedef struct {
    bool valid;
    int nfiles;
    media_file_fp_t files[MEDIA_FP_MAX_FILES];
} media_fp_t;

bool media_fp_compute(media_fp_t *out, const char *const *paths, int npaths);
bool media_fp_compute_memory(media_fp_t *out, const void *data, size_t size, const char *path,
                             int fd);
void media_fp_free(media_fp_t *fp);
bool media_fp_clone(media_fp_t *dst, const media_fp_t *src);
bool media_fp_equal(const media_fp_t *a, const media_fp_t *b);
bool media_fp_matches_path(const media_fp_t *fp, int file_index);
bool media_fp_matches_open_file(const media_fp_t *fp, int file_index, FILE *open_fp);
