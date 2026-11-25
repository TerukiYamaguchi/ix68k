/*
 * hu68disk - Human68k (Sharp X68000) ディスクイメージ ファイル操作ツール
 *
 * 共有ヘッダ。型・定数・小さなインラインヘルパー・各モジュールの公開関数宣言。
 *
 * レイヤー:
 *   sjis.c   Shift-JIS ⇔ UTF-8 変換
 *   image.c  イメージ入出力・BPB・セクタ/クラスタ アドレス計算
 *   fat.c    FAT テーブル・クラスタ割り当て・ファイルデータ read/write
 *   dir.c    名前コーデック・ディレクトリ走査・パス解決・日時
 *   cmd.c    サブコマンド
 *   main.c   CLI ディスパッチ
 */
#ifndef HU68DISK_H
#define HU68DISK_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>

/* ---- ディレクトリエントリ オフセット（XC20 付録③） ---- */
#define DE_NAME    0x00 /* 8B  名前 1〜8 文字目（スペース $20 埋め） */
#define DE_EXT     0x08 /* 3B  拡張子（スペース $20 埋め） */
#define DE_ATTR    0x0B /* 1B  属性 */
#define DE_NAME2   0x0C /* 10B 名前 9〜18 文字目（未使用部は $00） */
#define DE_TIME    0x16 /* 2B  時刻（FAT 形式, LE） */
#define DE_DATE    0x18 /* 2B  日付（FAT 形式, LE） */
#define DE_CLUSTER 0x1A /* 2B  開始クラスタ（LE） */
#define DE_SIZE    0x1C /* 4B  ファイルサイズ（LE） */
#define DE_SIZE_B  32   /* 1 エントリ = 32 バイト */

#define ATTR_READONLY 0x01
#define ATTR_HIDDEN   0x02
#define ATTR_SYSTEM   0x04
#define ATTR_VOLUME   0x08
#define ATTR_DIR      0x10
#define ATTR_ARCHIVE  0x20
#define ATTR_LFN      0x0F /* VFAT ロングファイル名（Human68k では異物） */

#define NAME_MAX_BASE 18 /* 名前部（8 + 10）バイト */
#define NAME_MAX_EXT  3
#define NAMEBUF       64 /* 復元名の作業バッファ */

/* ---- SCSI HDD (.HDS) 関連オフセット・署名（XEiJ SCMedia.java
 * 準拠、design_hu68disk.md 参照） ---- */
#define SCSI_DEV_PARAM_OFF   0x0000 /* デバイスパラメータ（署名 "X68SCSI1"） */
#define SCSI_PART_TABLE_OFF  0x0800 /* パーティションテーブル（署名 "X68K"） */
#define SCSI_PART_TABLE_MAX  15     /* 第1〜第15パーティション */
#define SCSI_PART_ENTRY_SIZE 16     /* 1パーティション情報のバイト数 */

/* ---- イメージ種別 ---- */
typedef enum { IMG_FLOPPY, IMG_SCSI_HDD } image_kind_t;

typedef struct {
    int slot; /* table slot, 1..15 */
    uint8_t type[8];
    uint8_t flag;
    uint32_t start_sector; /* 1024-byte sectors */
    uint32_t sector_count;
    uint64_t byte_offset;
    uint64_t byte_size;
    int occupied;
    int is_human68k;
} partition_t;

typedef struct {
    uint32_t size_mb;
    const char *label;
} scsi_partition_spec_t;

/* ---- メモリ上のイメージ ---- */
/* data はファイル全体ではなくマップ済み範囲 [map_offset, map_offset + map_size) を保持する。
 * SCSI HDD は「先頭ヘッダのみ」→ 区画選択後は「その区画のみ」に絞る（GB 級イメージで
 * 全体を確保しないため）。FD は最大 1.44MB なので従来どおり全体をマップする。 */
typedef struct {
    uint8_t *data;
    uint64_t size; /* ファイル全長（マップ長ではない） */
    char path[1024];
    image_kind_t kind;
    partition_t partitions[SCSI_PART_TABLE_MAX];
    int selected_part;    /* array index, -1 until selected */
    uint64_t part_offset; /* selected partition derived cache (FD is 0) */
    uint64_t part_size;   /* selected partition derived cache */
    uint64_t map_offset;  /* data が表すファイル内オフセット */
    size_t map_size;      /* data の長さ */
    int range_error;
    uint8_t *error_buffer;
    size_t error_buffer_size;
    int fat_be; /* FAT16 エントリが BE か（HD=1 / FD=0。実データ確認済み） */
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t reserved_sectors;
    uint32_t num_fats;
    uint32_t root_entries;
    uint32_t total_sectors;
    uint32_t sectors_per_fat;
    int fat_bits; /* 12 or 16 */
    uint32_t fat_start_sec;
    uint32_t root_start_sec;
    uint32_t data_start_sec;
    uint32_t root_sectors;
    uint32_t cluster_bytes;
    uint32_t total_clusters;
} image_t;

/* ---- ディレクトリ（ルート=固定領域、サブ=クラスタチェーン） ---- */
typedef struct {
    int is_root;
    uint32_t first_cluster;
} dir_t;
extern const dir_t DIR_ROOT;

/* ---- リトルエンディアン read/write（ホスト非依存の小ヘルパー） ---- */
static inline uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static inline uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static inline void wr16(uint8_t *p, uint16_t v) {
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
}
static inline void wr32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
}

/* ---- ビッグエンディアン read/write（SCSI HDD のデバイスパラメータ・パーティション
 * テーブル・BPB・FAT16 は 68k ネイティブの BE。design_hu68disk.md 参照） ---- */
static inline uint16_t rd16be(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}
static inline uint32_t rd32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline void wr16be(uint8_t *p, uint16_t v) {
    p[0] = (v >> 8) & 0xFF;
    p[1] = v & 0xFF;
}
static inline void wr32be(uint8_t *p, uint32_t v) {
    p[0] = (v >> 24) & 0xFF;
    p[1] = (v >> 16) & 0xFF;
    p[2] = (v >> 8) & 0xFF;
    p[3] = v & 0xFF;
}

/* ---- sjis.c (src/peripheral/sjis.h と共有) ---- */
#include "../../src/peripheral/sjis.h"

/* ---- image.c ---- */
int image_open(image_t *im, const char *path);
int image_select_partition(image_t *im, int slot); /* -1: first usable */
/* ラベル表示専用の軽量選択。区画全体ではなくルートディレクトリまでしかマップしない */
int image_probe_partition(image_t *im, int slot);
int image_load(image_t *im, const char *path); /* open + default selection */
int image_writeback(image_t *im);
int image_backup(const image_t *im, const char *dest); /* ファイル→ファイルの複製 */
void image_free(image_t *im);
uint8_t *image_ptr(image_t *im, uint64_t relative_offset, size_t length);
uint8_t *sector_ptr(image_t *im, uint32_t sec);
uint8_t *cluster_ptr(image_t *im, uint32_t cl);

/* ---- fat.c ---- */
uint32_t fat_get(image_t *im, uint32_t cl);
void fat_set(image_t *im, uint32_t cl, uint32_t val);
uint32_t fat_eoc(image_t *im);
int fat_is_eoc(image_t *im, uint32_t v);
int fat_is_free(uint32_t v);
uint32_t fat_find_free(image_t *im, uint32_t from);
void free_chain(image_t *im, uint32_t cl);
int write_file_data(image_t *im, const uint8_t *data, size_t size, uint32_t *first);
uint8_t *read_file_data(image_t *im, uint32_t first, uint32_t size);

/* ---- dir.c ---- */
int split_name(const char *fname, char *base, char *ext);
void encode_name(uint8_t *e, const char *base, const char *ext);
void decode_name(const uint8_t *e, char *out);
int name_ieq(const char *a, const char *b);
void fat_datetime(time_t t, uint16_t *fdate, uint16_t *ftime);
time_t fat_datetime_to_unix(uint16_t fdate, uint16_t ftime);
void dir_from_cluster(uint32_t cl, dir_t *out);
uint8_t *dir_scan(image_t *im, const dir_t *d, int (*cb)(uint8_t *e, void *ctx), void *ctx);
uint8_t *dir_alloc_slot(image_t *im, const dir_t *d);
uint8_t *dir_find(image_t *im, const dir_t *d, const char *name, int want_dir);
int path_split(const char *path, char comps[16][32]);
int descend(image_t *im, const dir_t *in, const char *name, dir_t *out);
int resolve_dir(image_t *im, const char *path, dir_t *out);
int resolve_parent(image_t *im, const char *path, dir_t *parent, char *leaf, size_t leafsz);

/* ---- cmd.c ---- */
int cmd_list(image_t *im, const char *path);
int cmd_copy(image_t *im, const char *hostpath, const char *destpath);
int cmd_del(image_t *im, const char *path);
int cmd_extract(image_t *im, const char *path, const char *hostout);
int cmd_mkdir(image_t *im, const char *path);
int cmd_rmdir(image_t *im, const char *path);
int cmd_copyall(image_t *src, const char *src_path, image_t *dst, const char *dst_path);
int cmd_partitions(image_t *im, int all);
int cmd_mkfs(const char *path, const char *type, int force, int size_mb, const char *label,
             int removable, int no_partition, const scsi_partition_spec_t *volumes,
             int volume_count);

/* ---- fsck.c ---- */
int cmd_fsck(image_t *im, int fix, int verbose, int assume_yes, int no_backup);

#endif /* HU68DISK_H */
