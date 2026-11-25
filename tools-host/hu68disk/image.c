/* image.c - image loading, SCSI partition selection and checked addressing */
/* fseeko/ftello（64bit オフセット）は C99 だけでは宣言されない環境があるため明示する。
 * 2GB 超のイメージで区画オフセットを seek するのに必須。 */
#define _POSIX_C_SOURCE   200809L
#define _FILE_OFFSET_BITS 64
#include "hu68disk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"

/* SCSI 判定とパーティションテーブル読み取りに必要なファイル先頭の長さ */
#define SCSI_HEADER_BYTES \
    (SCSI_PART_TABLE_OFF + 0x10 + SCSI_PART_TABLE_MAX * SCSI_PART_ENTRY_SIZE)
/* Human68k BPB が収まる最小バイト数（parse_bpb_human_at が読む範囲） */
#define BPB_MIN_BYTES      0x26
/* image_backup() のコピー単位 */
#define BACKUP_CHUNK_BYTES (256 * 1024)

/* ファイルの [off, off+len) を im->data へ読み直す。以前のマップは解放する。
 * 0=OK, -1=範囲外・I/O エラー・メモリ不足。 */
static int map_range(image_t *im, uint64_t off, uint64_t len) {
    FILE *f;
    uint8_t *buf;
    if (len == 0 || len != (size_t)len || off > im->size || len > im->size - off)
        return -1;
    buf = malloc((size_t)len);
    if (!buf)
        return -1;
    f = hu_fopen(im->path, "rb");
    if (!f) {
        free(buf);
        return -1;
    }
    if (hu_fseek(f, (hu_off_t)off, SEEK_SET) || fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);
    free(im->data);
    im->data = buf;
    im->map_offset = off;
    im->map_size = (size_t)len;
    return 0;
}

static int fd_ext_ok(const char *path) {
    static const char *const allowed[] = {".xdf", ".hdm", ".2hd"};
    const char *dot = strrchr(path, '.');
    if (!dot)
        return 0;
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++)
        if (strcasecmp(dot, allowed[i]) == 0)
            return 1;
    return 0;
}

static int derive_geometry(image_t *im) {
    uint64_t cluster = (uint64_t)im->bytes_per_sector * im->sectors_per_cluster;
    uint64_t root = ((uint64_t)im->root_entries * DE_SIZE_B + im->bytes_per_sector - 1) /
                    im->bytes_per_sector;
    uint64_t root_start =
        (uint64_t)im->reserved_sectors + (uint64_t)im->num_fats * im->sectors_per_fat;
    uint64_t data_start = root_start + root;
    if (cluster > UINT32_MAX || root > UINT32_MAX || root_start > UINT32_MAX ||
        data_start > UINT32_MAX || data_start > im->total_sectors)
        return -1;
    im->cluster_bytes = (uint32_t)cluster;
    im->fat_start_sec = im->reserved_sectors;
    im->root_start_sec = (uint32_t)root_start;
    im->root_sectors = (uint32_t)root;
    im->data_start_sec = (uint32_t)data_start;
    im->total_clusters = (im->total_sectors - im->data_start_sec) / im->sectors_per_cluster;
    im->fat_bits = im->total_clusters < 4085 ? 12 : 16;
    return 0;
}

/* Exact equality is intentionally not required: FORMAT.X may leave a tail unused. */
static int geometry_ok(const image_t *im, uint64_t available) {
    uint64_t total_bytes, fat_bytes, root_bytes, data_bytes;
    if (im->bytes_per_sector < 128 || im->bytes_per_sector > 8192 ||
        (im->bytes_per_sector & (im->bytes_per_sector - 1)) || im->sectors_per_cluster == 0 ||
        im->sectors_per_cluster > 128 ||
        (im->sectors_per_cluster & (im->sectors_per_cluster - 1)) || im->num_fats == 0 ||
        im->num_fats > 2 || im->sectors_per_fat == 0 || im->root_entries == 0 ||
        im->total_sectors == 0 || im->data_start_sec >= im->total_sectors)
        return 0;
    total_bytes = (uint64_t)im->total_sectors * im->bytes_per_sector;
    fat_bytes = (uint64_t)im->num_fats * im->sectors_per_fat * im->bytes_per_sector;
    root_bytes = (uint64_t)im->root_entries * DE_SIZE_B;
    data_bytes = (uint64_t)(im->total_sectors - im->data_start_sec) * im->bytes_per_sector;
    return total_bytes <= available && fat_bytes <= available && root_bytes <= available &&
           data_bytes <= available;
}

static void parse_bpb_dos(image_t *im) {
    const uint8_t *b = im->data;
    im->fat_be = 0;
    im->bytes_per_sector = rd16(b + 0x0b);
    im->sectors_per_cluster = b[0x0d];
    im->reserved_sectors = rd16(b + 0x0e);
    im->num_fats = b[0x10];
    im->root_entries = rd16(b + 0x11);
    im->total_sectors = rd16(b + 0x13);
    if (!im->total_sectors)
        im->total_sectors = rd32(b + 0x20);
    im->sectors_per_fat = rd16(b + 0x16);
    (void)derive_geometry(im);
}
static void parse_bpb_human_at(image_t *im, const uint8_t *b) {
    im->fat_be = 1;
    im->bytes_per_sector = rd16be(b + 0x12);
    im->sectors_per_cluster = b[0x14];
    im->num_fats = b[0x15];
    im->reserved_sectors = rd16be(b + 0x16);
    im->root_entries = rd16be(b + 0x18);
    im->sectors_per_fat = b[0x1d];
    im->total_sectors = rd16be(b + 0x1a);
    if (!im->total_sectors)
        im->total_sectors = rd32be(b + 0x1e);
    (void)derive_geometry(im);
}
static int allocate_error_buffer(image_t *im) {
    uint64_t fat = (uint64_t)im->sectors_per_fat * im->bytes_per_sector;
    uint64_t root = (uint64_t)im->root_entries * DE_SIZE_B;
    uint64_t max = im->cluster_bytes;
    if (fat > max)
        max = fat;
    if (root > max)
        max = root;
    if (im->bytes_per_sector > max)
        max = im->bytes_per_sector;
    if (max == 0 || max > SIZE_MAX)
        return -1;
    im->error_buffer = calloc(1, (size_t)max);
    if (!im->error_buffer)
        return -1;
    im->error_buffer_size = (size_t)max;
    return 0;
}
static int overlaps_other(const image_t *im, int selected) {
    const partition_t *a = &im->partitions[selected];
    uint64_t aend = a->byte_offset + a->byte_size;
    for (int i = 0; i < SCSI_PART_TABLE_MAX; i++) {
        const partition_t *b = &im->partitions[i];
        if (i == selected || !b->occupied || b->byte_size == 0)
            continue;
        if (b->byte_offset > UINT64_MAX - b->byte_size)
            return 1;
        if (a->byte_offset < b->byte_offset + b->byte_size && b->byte_offset < aend)
            return 1;
    }
    return 0;
}

int image_open(image_t *im, const char *path) {
    FILE *f;
    hu_off_t sz;
    memset(im, 0, sizeof(*im));
    im->selected_part = -1;
    snprintf(im->path, sizeof(im->path), "%s", path);
    f = hu_fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open image '%s'\n", path);
        return -1;
    }
    if (hu_fseek(f, 0, SEEK_END) || (sz = hu_ftell(f)) < 512) {
        fprintf(stderr, "error: image too small or unreadable\n");
        fclose(f);
        return -1;
    }
    fclose(f);
    im->size = (uint64_t)sz;
    /* まず SCSI 判定とパーティションテーブルに必要な先頭だけを読む。
     * 全体を読むのは（最大 1.44MB の）FD イメージだけ。 */
    if (map_range(im, 0, im->size < SCSI_HEADER_BYTES ? im->size : SCSI_HEADER_BYTES)) {
        fprintf(stderr, "error: short read or out of memory\n");
        image_free(im);
        return -1;
    }
    if (im->size >= SCSI_HEADER_BYTES && !memcmp(im->data, "X68SCSI1", 8)) {
        const uint8_t *pt = im->data + SCSI_PART_TABLE_OFF;
        im->kind = IMG_SCSI_HDD;
        if (memcmp(pt, "X68K", 4)) {
            fprintf(
                stderr,
                "error: 未初期化の SCSI ディスクです（パーティションテーブルがありません）\n");
            image_free(im);
            return -1;
        }
        for (int i = 0; i < SCSI_PART_TABLE_MAX; i++) {
            const uint8_t *e = pt + 0x10 + (size_t)i * SCSI_PART_ENTRY_SIZE;
            partition_t *p = &im->partitions[i];
            uint32_t sf = rd32be(e + 8);
            p->slot = i + 1;
            memcpy(p->type, e, 8);
            p->flag = sf >> 24;
            p->start_sector = sf & 0x00ffffffu;
            p->sector_count = rd32be(e + 12);
            p->occupied = memcmp(e, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16) != 0;
            p->is_human68k = !memcmp(p->type, "Human68k", 8);
            p->byte_offset = (uint64_t)p->start_sector * 1024;
            p->byte_size = (uint64_t)p->sector_count * 1024;
        }
        return 0;
    }
    im->kind = IMG_FLOPPY;
    if (!fd_ext_ok(path)) {
        fprintf(stderr, "error: 未対応の拡張子です（対応: .xdf/.hdm/.2hd）: %s\n", path);
        image_free(im);
        return -1;
    }
    if (map_range(im, 0, im->size)) {
        fprintf(stderr, "error: short read or out of memory\n");
        image_free(im);
        return -1;
    }
    parse_bpb_dos(im);
    if (!geometry_ok(im, im->size)) {
        parse_bpb_human_at(im, im->data);
        im->fat_be = 0;
        if (!geometry_ok(im, im->size)) {
            fprintf(stderr,
                    "error: BPB を認識できません（DOS/Human 両レイアウトとも不一致）\n");
            image_free(im);
            return -1;
        }
    }
    return image_select_partition(im, -1);
}

/* ラベル表示に必要な範囲＝区画先頭からルートディレクトリ領域の末尾まで */
static uint64_t label_probe_bytes(const image_t *im) {
    uint64_t end = (uint64_t)im->root_start_sec * im->bytes_per_sector +
                   (uint64_t)im->root_entries * DE_SIZE_B;
    return end < im->part_size ? end : im->part_size;
}

/* map_full=0 のときは区画全体ではなくルートディレクトリまでしかマップしない
 * （partitions のラベル読みだけが目的の場合に GB 級の確保を避ける）。 */
static int select_partition_internal(image_t *im, int slot, int map_full) {
    int index = -1;
    free(im->error_buffer);
    im->error_buffer = NULL;
    im->error_buffer_size = 0;
    im->range_error = 0;
    if (im->kind == IMG_FLOPPY) {
        if (slot != -1) {
            fprintf(stderr, "error: --part は FD イメージには使用できません\n");
            return -1;
        }
        im->selected_part = -1;
        im->part_offset = 0;
        im->part_size = im->size;
        if (!geometry_ok(im, im->part_size) || allocate_error_buffer(im))
            goto invalid;
        return 0;
    }
    if (slot == -1) {
        for (int i = 0; i < SCSI_PART_TABLE_MAX; i++)
            if (im->partitions[i].occupied && im->partitions[i].is_human68k &&
                im->partitions[i].flag != 1) {
                index = i;
                break;
            }
    } else if (slot >= 1 && slot <= SCSI_PART_TABLE_MAX)
        index = slot - 1;
    else {
        fprintf(stderr, "error: --part は 1〜15 で指定してください\n");
        return -1;
    }
    if (index < 0 || !im->partitions[index].occupied) {
        fprintf(stderr, "error: 指定パーティションがありません\n");
        return -1;
    }
    partition_t *p = &im->partitions[index];
    if (!p->is_human68k) {
        fprintf(stderr, "error: 指定パーティションは Human68k ではありません\n");
        return -1;
    }
    if (p->flag == 1) {
        fprintf(stderr, "error: 指定パーティションは使用不可です\n");
        return -1;
    }
    if (!p->start_sector || !p->sector_count || p->byte_offset > im->size ||
        p->byte_size > im->size - p->byte_offset || overlaps_other(im, index)) {
        fprintf(stderr, "error: 指定パーティションの範囲または重複が不正です\n");
        return -1;
    }
    im->selected_part = index;
    im->part_offset = p->byte_offset;
    im->part_size = p->byte_size;
    if (p->byte_size < BPB_MIN_BYTES)
        goto invalid;
    /* BPB は区画先頭にあるので、まず先頭だけ読んでジオメトリを確定させ、
     * 必要なマップ長が決まってから本来の範囲を読み直す。 */
    if (map_range(im, im->part_offset, BPB_MIN_BYTES))
        goto read_error;
    parse_bpb_human_at(im, im->data);
    if (!geometry_ok(im, im->part_size))
        goto invalid;
    if (map_range(im, im->part_offset, map_full ? im->part_size : label_probe_bytes(im)))
        goto read_error;
    if (allocate_error_buffer(im))
        goto invalid;
    return 0;
read_error:
    fprintf(stderr, "error: パーティションの読み込みに失敗しました\n");
    goto fail;
invalid:
    fprintf(stderr, "error: 指定パーティションの BPB/ジオメトリが不正です\n");
fail:
    free(im->error_buffer);
    im->error_buffer = NULL;
    im->error_buffer_size = 0;
    im->selected_part = -1;
    return -1;
}

int image_select_partition(image_t *im, int slot) {
    return select_partition_internal(im, slot, 1);
}

int image_probe_partition(image_t *im, int slot) {
    return select_partition_internal(im, slot, 0);
}
int image_load(image_t *im, const char *path) {
    if (image_open(im, path))
        return -1;
    return im->kind == IMG_SCSI_HDD ? image_select_partition(im, -1) : 0;
}
/* off から length バイトが「選択区画の内側」かつ「マップ済み範囲の内側」か */
static int in_mapped_range(const image_t *im, uint64_t off, size_t length) {
    uint64_t rel;
    if (off > im->part_size || length > im->part_size - off)
        return 0;
    if (im->part_offset + off < im->map_offset)
        return 0;
    rel = im->part_offset + off - im->map_offset;
    return rel <= im->map_size && length <= im->map_size - rel;
}

uint8_t *image_ptr(image_t *im, uint64_t off, size_t length) {
    if (!im->error_buffer || length > im->error_buffer_size ||
        !in_mapped_range(im, off, length)) {
        im->range_error = 1;
        return im->error_buffer;
    }
    return im->data + (size_t)(im->part_offset + off - im->map_offset);
}
uint8_t *sector_ptr(image_t *im, uint32_t sec) {
    return image_ptr(im, (uint64_t)sec * im->bytes_per_sector, im->bytes_per_sector);
}
uint8_t *cluster_ptr(image_t *im, uint32_t cl) {
    uint64_t sec = (uint64_t)im->data_start_sec + (uint64_t)(cl - 2) * im->sectors_per_cluster;
    return image_ptr(im, sec * im->bytes_per_sector, im->cluster_bytes);
}
int image_writeback(image_t *im) {
    if (im->range_error) {
        fprintf(stderr, "error: パーティション境界違反を検出したため書き戻しません\n");
        return -1;
    }
    FILE *f = hu_fopen(im->path, "r+b");
    if (!f) {
        fprintf(stderr, "error: cannot reopen image for write\n");
        return -1;
    }
    if (hu_fseek(f, (hu_off_t)im->map_offset, SEEK_SET) ||
        fwrite(im->data, 1, im->map_size, f) != im->map_size) {
        fprintf(stderr, "error: writeback failed\n");
        fclose(f);
        return -1;
    }
    if (fclose(f)) { /* 書き込みエラーは close で初めて表面化することがある */
        fprintf(stderr, "error: writeback failed\n");
        return -1;
    }
    return 0;
}
/* イメージファイルをそのまま dest へ複製する（fsck の .bak 作成用）。
 * メモリ上には選択区画しか無いのでファイル→ファイルでコピーする。 */
int image_backup(const image_t *im, const char *dest) {
    uint8_t *buf;
    FILE *sf, *df;
    uint64_t left = im->size;
    int rc = 0;
    sf = hu_fopen(im->path, "rb");
    if (!sf)
        return -1;
    df = hu_fopen(dest, "wb");
    if (!df) {
        fclose(sf);
        return -1;
    }
    buf = malloc(BACKUP_CHUNK_BYTES);
    if (!buf)
        rc = -1;
    while (rc == 0 && left) {
        size_t want = left < BACKUP_CHUNK_BYTES ? (size_t)left : BACKUP_CHUNK_BYTES;
        if (fread(buf, 1, want, sf) != want || fwrite(buf, 1, want, df) != want)
            rc = -1;
        left -= want;
    }
    free(buf);
    fclose(sf);
    if (fclose(df))
        rc = -1;
    return rc;
}
void image_free(image_t *im) {
    free(im->data);
    free(im->error_buffer);
    im->data = NULL;
    im->error_buffer = NULL;
    im->error_buffer_size = 0;
    im->map_offset = 0;
    im->map_size = 0;
}
