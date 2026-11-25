/*
 * fat.c - FAT テーブルアクセス・クラスタ割り当て・ファイルデータ read/write
 *
 * FAT12（2HD/4MB まで）と FAT16（5MB 以上 HD）の両対応。書き込みは全 FAT に反映。
 */
#include "hu68disk.h"
#include <stdlib.h>
#include <string.h>

uint32_t fat_get(image_t *im, uint32_t cl) {
    uint64_t fat_bytes = (uint64_t)im->sectors_per_fat * im->bytes_per_sector;
    uint64_t entry_end =
        im->fat_bits == 12 ? (uint64_t)cl + (cl >> 1) + 2 : ((uint64_t)cl + 1) * 2;
    if (entry_end > fat_bytes) {
        im->range_error = 1;
        return 0;
    }
    uint8_t *fat = image_ptr(im, (uint64_t)im->fat_start_sec * im->bytes_per_sector,
                             (size_t)im->sectors_per_fat * im->bytes_per_sector);
    if (im->fat_bits == 12) {
        uint32_t off = cl + (cl >> 1); /* cl * 1.5 */
        uint16_t v = (uint16_t)(fat[off] | (fat[off + 1] << 8));
        return (cl & 1) ? (v >> 4) : (v & 0x0FFF);
    }
    /* FAT16: HD は BE、FD は LE（実データ確認済み、design_hu68disk.md §2） */
    return im->fat_be ? rd16be(fat + cl * 2) : rd16(fat + cl * 2);
}

void fat_set(image_t *im, uint32_t cl, uint32_t val) {
    uint64_t fat_bytes = (uint64_t)im->sectors_per_fat * im->bytes_per_sector;
    uint64_t entry_end =
        im->fat_bits == 12 ? (uint64_t)cl + (cl >> 1) + 2 : ((uint64_t)cl + 1) * 2;
    if (entry_end > fat_bytes) {
        im->range_error = 1;
        return;
    }
    for (uint32_t n = 0; n < im->num_fats; n++) {
        uint8_t *fat = image_ptr(
            im, (uint64_t)(im->fat_start_sec + n * im->sectors_per_fat) * im->bytes_per_sector,
            (size_t)im->sectors_per_fat * im->bytes_per_sector);
        if (im->fat_bits == 12) {
            uint32_t off = cl + (cl >> 1);
            uint16_t cur = (uint16_t)(fat[off] | (fat[off + 1] << 8));
            if (cl & 1)
                cur = (uint16_t)((cur & 0x000F) | ((val & 0x0FFF) << 4));
            else
                cur = (uint16_t)((cur & 0xF000) | (val & 0x0FFF));
            fat[off] = cur & 0xFF;
            fat[off + 1] = (cur >> 8) & 0xFF;
        } else if (im->fat_be) {
            wr16be(fat + cl * 2, (uint16_t)val);
        } else {
            wr16(fat + cl * 2, (uint16_t)val);
        }
    }
}

uint32_t fat_eoc(image_t *im) {
    return im->fat_bits == 12 ? 0x0FFF : 0xFFFF;
}
int fat_is_eoc(image_t *im, uint32_t v) {
    return im->fat_bits == 12 ? (v >= 0x0FF8) : (v >= 0xFFF8);
}
int fat_is_free(uint32_t v) {
    return v == 0;
}

uint32_t fat_find_free(image_t *im, uint32_t from) {
    for (uint32_t c = (from < 2 ? 2 : from); c < im->total_clusters + 2; c++)
        if (fat_is_free(fat_get(im, c)))
            return c;
    return 0;
}

void free_chain(image_t *im, uint32_t cl) {
    while (cl >= 2 && !fat_is_eoc(im, cl) && !fat_is_free(cl)) {
        uint32_t next = fat_get(im, cl);
        fat_set(im, cl, 0);
        cl = next;
    }
}

/* data/size をクラスタ連鎖に書き、first に先頭クラスタを返す。0=OK,-1=容量不足。
 * 0 バイトでも 1 クラスタ確保する（ゼロ埋め）。FAT 標準は空ファイルの開始クラスタを 0
 * とするが、Human68k は空ファイルにもクラスタを割り当てる実装で、開始クラスタ 0 の
 * エントリへのゼロ長 _WRITE で FAT[0] を辿って白帯（ディスク管理領域破壊）になる。 */
int write_file_data(image_t *im, const uint8_t *data, size_t size, uint32_t *first) {
    uint32_t need = size ? (uint32_t)((size + im->cluster_bytes - 1) / im->cluster_bytes) : 1;
    *first = 0;
    uint32_t prev = 0, search = 2;
    size_t rem = size;
    const uint8_t *src = data;
    for (uint32_t k = 0; k < need; k++) {
        uint32_t c = fat_find_free(im, search);
        if (!c)
            return -1;
        search = c + 1;
        fat_set(im, c, fat_eoc(im));
        if (prev)
            fat_set(im, prev, c);
        else
            *first = c;
        uint8_t *dst = cluster_ptr(im, c);
        size_t n = rem < im->cluster_bytes ? rem : im->cluster_bytes;
        memcpy(dst, src, n);
        if (n < im->cluster_bytes)
            memset(dst + n, 0, im->cluster_bytes - n);
        src += n;
        rem -= n;
        prev = c;
    }
    return 0;
}

/* first クラスタから size バイトを読み出した malloc バッファを返す（呼び出し側で free） */
uint8_t *read_file_data(image_t *im, uint32_t first, uint32_t size) {
    uint8_t *buf = malloc(size ? size : 1);
    if (!buf)
        return NULL;
    uint32_t cl = first, off = 0;
    while (off < size && cl >= 2 && !fat_is_eoc(im, cl)) {
        uint32_t n = im->cluster_bytes;
        if (size - off < n)
            n = size - off;
        memcpy(buf + off, cluster_ptr(im, cl), n);
        off += n;
        cl = fat_get(im, cl);
    }
    return buf;
}
