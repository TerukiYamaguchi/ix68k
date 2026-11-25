/*
 * cmd.c - サブコマンド（list / copy / del / extract / mkdir / rmdir / mkfs）
 */
#include "hu68disk.h"
#include "scsi_ipl_data.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "compat.h"

/* SJIS 名を表示用 UTF-8 に（失敗時は SJIS のまま） */
static void name_for_display(const char *sjis, char *out, size_t outsz) {
    if (sjis_to_utf8(sjis, strlen(sjis), out, outsz) < 0)
        snprintf(out, outsz, "%s", sjis);
}

int cmd_list(image_t *im, const char *path) {
    dir_t d;
    char psjis[512];
    if (path) {
        if (utf8_to_sjis(path, psjis, sizeof(psjis)) < 0) {
            fprintf(stderr, "error: 変換失敗\n");
            return -1;
        }
    } else
        psjis[0] = 0;
    if (resolve_dir(im, psjis, &d)) {
        fprintf(stderr, "error: ディレクトリ '%s' が見つかりません\n", path ? path : "/");
        return -1;
    }

    printf("Volume: %s%s%s  (%s, %u sectors, %u-byte cluster, FAT%d)\n", im->path,
           path ? " : " : "", path ? path : "", im->fat_bits == 12 ? "2HD/FAT12" : "HD/FAT16",
           im->total_sectors, im->cluster_bytes, im->fat_bits);

    int files = 0;
    uint32_t used = 0;
    /* 列挙は手書きで（root/サブ 両対応） */
    uint32_t per = im->cluster_bytes / DE_SIZE_B;
    uint32_t cl = d.is_root ? 0 : d.first_cluster;
    uint32_t idx = 0;
    for (;;) {
        uint8_t *e;
        if (d.is_root) {
            if (idx >= im->root_entries)
                break;
            e = sector_ptr(im, im->root_start_sec) + (size_t)idx * DE_SIZE_B;
            idx++;
        } else {
            if (cl < 2 || fat_is_eoc(im, cl))
                break;
            e = cluster_ptr(im, cl) + (size_t)(idx % per) * DE_SIZE_B;
            idx++;
            if (idx % per == 0)
                cl = fat_get(im, cl);
        }
        if (e[DE_NAME] == 0x00)
            break;
        if (e[DE_NAME] == 0xE5)
            continue;
        uint8_t attr = e[DE_ATTR];
        if (attr == ATTR_LFN)
            continue; /* VFAT 断片は無視 */
        char sjis[NAMEBUF];
        decode_name(e, sjis);
        if (strcmp(sjis, ".") == 0 || strcmp(sjis, "..") == 0)
            continue;
        char name[160];
        name_for_display(sjis, name, sizeof(name));
        if (attr & ATTR_VOLUME) {
            printf("  %-21s  <volume>\n", name);
            continue;
        }
        char tags[8];
        int t = 0;
        if (attr & ATTR_DIR)
            tags[t++] = 'D';
        if (attr & ATTR_READONLY)
            tags[t++] = 'R';
        if (attr & ATTR_HIDDEN)
            tags[t++] = 'H';
        if (attr & ATTR_SYSTEM)
            tags[t++] = 'S';
        tags[t] = 0;
        if (attr & ATTR_DIR)
            printf("  %-21s  %10s  %s\n", name, "<dir>", tags);
        else {
            uint32_t sz = rd32(e + DE_SIZE);
            printf("  %-21s  %10u  %s\n", name, sz, tags);
            used += sz;
            files++;
        }
    }
    printf("  %d file(s), %u bytes\n", files, used);
    return 0;
}

/* 親ディレクトリ d に base.ext のファイルエントリを作成（上書き対応） */
static int put_file(image_t *im, const dir_t *d, const char *base, const char *ext,
                    const uint8_t *data, size_t size, time_t mtime, uint8_t attr) {
    char nm[NAMEBUF];
    if (ext[0])
        snprintf(nm, sizeof(nm), "%s.%s", base, ext);
    else
        snprintf(nm, sizeof(nm), "%s", base);
    uint8_t *old = dir_find(im, d, nm, -1);
    if (old) {
        if (old[DE_ATTR] & ATTR_DIR) {
            fprintf(stderr, "error: '%s' は既にディレクトリです\n", nm);
            return -1;
        }
        free_chain(im, rd16(old + DE_CLUSTER));
        old[DE_NAME] = 0xE5;
    }
    uint32_t first;
    if (write_file_data(im, data, size, &first)) {
        fprintf(stderr, "error: 空き容量が足りません\n");
        return -1;
    }
    uint8_t *slot = dir_alloc_slot(im, d);
    if (!slot) {
        fprintf(stderr, "error: ディレクトリが満杯です\n");
        return -1;
    }
    memset(slot, 0, DE_SIZE_B);
    encode_name(slot, base, ext);
    slot[DE_ATTR] = attr;
    uint16_t fdate, ftime;
    fat_datetime(mtime, &fdate, &ftime);
    wr16(slot + DE_TIME, ftime);
    wr16(slot + DE_DATE, fdate);
    wr16(slot + DE_CLUSTER, (uint16_t)first);
    wr32(slot + DE_SIZE, (uint32_t)size);
    return 0;
}

static const char *host_basename(const char *p) {
    const char *b = strrchr(p, '/');
#if defined(_WIN32)
    const char *bb = strrchr(p, '\\');
    if (bb > b)
        b = bb;
#endif
    return b ? b + 1 : p;
}

int cmd_copy(image_t *im, const char *hostpath, const char *destpath) {
    FILE *hf = hu_fopen(hostpath, "rb");
    if (!hf) {
        fprintf(stderr, "error: cannot open '%s'\n", hostpath);
        return -1;
    }
    fseek(hf, 0, SEEK_END);
    long fsz = ftell(hf);
    fseek(hf, 0, SEEK_SET);
    if (fsz < 0) {
        fclose(hf);
        return -1;
    }
    uint8_t *fdata = fsz ? malloc((size_t)fsz) : NULL;
    if (fsz && fread(fdata, 1, (size_t)fsz, hf) != (size_t)fsz) {
        fprintf(stderr, "error: short read of host file\n");
        fclose(hf);
        free(fdata);
        return -1;
    }
    fclose(hf);
    time_t mtime;
    if (hu_get_mtime(hostpath, &mtime) != 0)
        mtime = time(NULL);

    /* destpath(UTF-8) → SJIS。ディレクトリとして解決できればその中へ、
     * できなければ親+leaf として解決し leaf をファイル名に。 */
    char destsjis[512];
    if (utf8_to_sjis(destpath, destsjis, sizeof(destsjis)) < 0) {
        fprintf(stderr, "error: 文字コード変換に失敗しました\n");
        free(fdata);
        return -1;
    }
    dir_t destdir;
    char namesjis[64];
    if (resolve_dir(im, destsjis, &destdir) == 0) {
        char bsj[64];
        if (utf8_to_sjis(host_basename(hostpath), bsj, sizeof(bsj)) < 0) {
            free(fdata);
            return -1;
        }
        snprintf(namesjis, sizeof(namesjis), "%s", bsj);
    } else {
        char leaf[64];
        if (resolve_parent(im, destsjis, &destdir, leaf, sizeof(leaf))) {
            free(fdata);
            return -1;
        }
        snprintf(namesjis, sizeof(namesjis), "%s", leaf);
    }

    char base[NAME_MAX_BASE + 1], ext[NAME_MAX_EXT + 1];
    if (split_name(namesjis, base, ext)) {
        free(fdata);
        return -1;
    }

    int rc = put_file(im, &destdir, base, ext, fdata, (size_t)fsz, mtime, ATTR_ARCHIVE);
    free(fdata);
    if (rc)
        return -1;
    if (image_writeback(im))
        return -1;
    char disp[160];
    name_for_display(namesjis, disp, sizeof(disp));
    printf("copied: %s -> %s (%ld bytes)\n", hostpath, disp, fsz);
    return 0;
}

int cmd_del(image_t *im, const char *path) {
    char psjis[512];
    if (utf8_to_sjis(path, psjis, sizeof(psjis)) < 0) {
        fprintf(stderr, "error: 変換失敗\n");
        return -1;
    }
    dir_t parent;
    char leaf[64];
    if (resolve_parent(im, psjis, &parent, leaf, sizeof(leaf)))
        return -1;
    uint8_t *e = dir_find(im, &parent, leaf, 0); /* 非DIR のみ */
    if (!e) {
        fprintf(stderr, "error: ファイル '%s' が見つかりません\n", path);
        return -1;
    }
    free_chain(im, rd16(e + DE_CLUSTER));
    e[DE_NAME] = 0xE5;
    if (image_writeback(im))
        return -1;
    char disp[160];
    name_for_display(leaf, disp, sizeof(disp));
    printf("deleted: %s\n", disp);
    return 0;
}

int cmd_extract(image_t *im, const char *path, const char *hostout) {
    char psjis[512];
    if (utf8_to_sjis(path, psjis, sizeof(psjis)) < 0) {
        fprintf(stderr, "error: 変換失敗\n");
        return -1;
    }
    dir_t parent;
    char leaf[64];
    if (resolve_parent(im, psjis, &parent, leaf, sizeof(leaf)))
        return -1;
    uint8_t *e = dir_find(im, &parent, leaf, 0);
    if (!e) {
        fprintf(stderr, "error: ファイル '%s' が見つかりません\n", path);
        return -1;
    }
    uint32_t size = rd32(e + DE_SIZE);
    uint32_t first = rd16(e + DE_CLUSTER);
    uint8_t *buf = read_file_data(im, first, size);
    if (!buf) {
        fprintf(stderr, "error: 読み出し失敗\n");
        return -1;
    }
    FILE *of = hu_fopen(hostout, "wb");
    if (!of) {
        fprintf(stderr, "error: cannot create '%s'\n", hostout);
        free(buf);
        return -1;
    }
    if (size && fwrite(buf, 1, size, of) != size) {
        fprintf(stderr, "error: write failed\n");
        fclose(of);
        free(buf);
        return -1;
    }
    fclose(of);
    free(buf);
    time_t mtime = fat_datetime_to_unix(rd16(e + DE_DATE), rd16(e + DE_TIME));
    hu_set_mtime(hostout, mtime);
    printf("extracted: %s -> %s (%u bytes)\n", path, hostout, size);
    return 0;
}

/* parent の下に base.ext のサブディレクトリを新規作成する（"." ".." エントリも設定）。
 * cmd_mkdir と cmd_copyall（宛先ツリーの再帰的なディレクトリ作成）の共通処理。 */
static int create_subdir(image_t *im, const dir_t *parent, const char *base, const char *ext,
                         time_t mtime, dir_t *out) {
    uint32_t nc = fat_find_free(im, 2);
    if (!nc) {
        fprintf(stderr, "error: 空き容量が足りません\n");
        return -1;
    }
    fat_set(im, nc, fat_eoc(im));
    uint8_t *dc = cluster_ptr(im, nc);
    memset(dc, 0, im->cluster_bytes);
    uint16_t fdate, ftime;
    fat_datetime(mtime, &fdate, &ftime);
    /* "." エントリ（自分） */
    memset(dc + DE_NAME, ' ', 11);
    dc[DE_NAME] = '.';
    dc[DE_ATTR] = ATTR_DIR;
    wr16(dc + DE_TIME, ftime);
    wr16(dc + DE_DATE, fdate);
    wr16(dc + DE_CLUSTER, (uint16_t)nc);
    /* ".." エントリ（親; ルートは 0） */
    uint8_t *dd = dc + DE_SIZE_B;
    memset(dd + DE_NAME, ' ', 11);
    dd[DE_NAME] = '.';
    dd[DE_NAME + 1] = '.';
    dd[DE_ATTR] = ATTR_DIR;
    wr16(dd + DE_TIME, ftime);
    wr16(dd + DE_DATE, fdate);
    wr16(dd + DE_CLUSTER, (uint16_t)(parent->is_root ? 0 : parent->first_cluster));
    /* 親にディレクトリエントリを追加 */
    uint8_t *slot = dir_alloc_slot(im, parent);
    if (!slot) {
        fat_set(im, nc, 0);
        fprintf(stderr, "error: 親ディレクトリが満杯です\n");
        return -1;
    }
    memset(slot, 0, DE_SIZE_B);
    encode_name(slot, base, ext);
    slot[DE_ATTR] = ATTR_DIR;
    wr16(slot + DE_TIME, ftime);
    wr16(slot + DE_DATE, fdate);
    wr16(slot + DE_CLUSTER, (uint16_t)nc);
    dir_from_cluster(nc, out);
    return 0;
}

int cmd_mkdir(image_t *im, const char *path) {
    char psjis[512];
    if (utf8_to_sjis(path, psjis, sizeof(psjis)) < 0) {
        fprintf(stderr, "error: 変換失敗\n");
        return -1;
    }
    dir_t parent;
    char leaf[64];
    if (resolve_parent(im, psjis, &parent, leaf, sizeof(leaf)))
        return -1;
    if (dir_find(im, &parent, leaf, -1)) {
        fprintf(stderr, "error: '%s' は既に存在します\n", path);
        return -1;
    }
    char base[NAME_MAX_BASE + 1], ext[NAME_MAX_EXT + 1];
    if (split_name(leaf, base, ext))
        return -1;

    dir_t created;
    if (create_subdir(im, &parent, base, ext, time(NULL), &created))
        return -1;
    if (image_writeback(im))
        return -1;
    printf("mkdir: %s\n", path);
    return 0;
}

/* dst 側で path_sjis の全階層を mkdir -p 相当で確保し、末端ディレクトリを *out に返す。
 * 既存のディレクトリがあれば作成せず流用する（copyall を複数回・複数ソースに対して
 * 実行しても安全＝実機 COPYALL を複数枚のディスクに対して繰り返す運用に対応）。 */
static int ensure_dir_path(image_t *im, const char *path_sjis, dir_t *out) {
    char comps[16][32];
    int n = path_split(path_sjis, comps);
    if (n < 0) {
        fprintf(stderr, "error: パスが深すぎます\n");
        return -1;
    }
    dir_t d = DIR_ROOT;
    for (int i = 0; i < n; i++) {
        uint8_t *e = dir_find(im, &d, comps[i], 1);
        if (e) {
            dir_from_cluster(rd16(e + DE_CLUSTER), &d);
            continue;
        }
        char base[NAME_MAX_BASE + 1], ext[NAME_MAX_EXT + 1];
        if (split_name(comps[i], base, ext))
            return -1;
        dir_t nd;
        if (create_subdir(im, &d, base, ext, time(NULL), &nd))
            return -1;
        d = nd;
    }
    *out = d;
    return 0;
}

/* src の sdir 直下を dst の ddir へ再帰的にコピーする。ファイル属性
 * (system/hidden/readonly)・日時は保持する。ボリュームラベル・LFN 断片・削除済み
 * エントリ・"." ".." は対象外（cmd_list と同じスキップ規則）。 */
static int copy_tree(image_t *src, const dir_t *sdir, image_t *dst, const dir_t *ddir,
                     int *n_files, int *n_dirs) {
    uint32_t per = src->cluster_bytes / DE_SIZE_B;
    uint32_t cl = sdir->is_root ? 0 : sdir->first_cluster;
    uint32_t idx = 0;
    for (;;) {
        uint8_t *e;
        if (sdir->is_root) {
            if (idx >= src->root_entries)
                break;
            e = sector_ptr(src, src->root_start_sec) + (size_t)idx * DE_SIZE_B;
            idx++;
        } else {
            if (cl < 2 || fat_is_eoc(src, cl))
                break;
            e = cluster_ptr(src, cl) + (size_t)(idx % per) * DE_SIZE_B;
            idx++;
            if (idx % per == 0)
                cl = fat_get(src, cl);
        }
        if (e[DE_NAME] == 0x00)
            break;
        if (e[DE_NAME] == 0xE5)
            continue;
        uint8_t attr = e[DE_ATTR];
        if (attr == ATTR_LFN || (attr & ATTR_VOLUME))
            continue;
        char nm[NAMEBUF];
        decode_name(e, nm);
        if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0)
            continue;

        char base[NAME_MAX_BASE + 1], ext[NAME_MAX_EXT + 1];
        if (split_name(nm, base, ext)) {
            fprintf(stderr, "error: コピー元の名前 '%s' を解釈できません\n", nm);
            return -1;
        }
        time_t mtime = fat_datetime_to_unix(rd16(e + DE_DATE), rd16(e + DE_TIME));

        if (attr & ATTR_DIR) {
            dir_t src_sub;
            dir_from_cluster(rd16(e + DE_CLUSTER), &src_sub);
            uint8_t *existing = dir_find(dst, ddir, nm, 1);
            dir_t dst_sub;
            if (existing) {
                dir_from_cluster(rd16(existing + DE_CLUSTER), &dst_sub);
            } else if (create_subdir(dst, ddir, base, ext, mtime, &dst_sub)) {
                return -1;
            }
            if (n_dirs)
                (*n_dirs)++;
            if (copy_tree(src, &src_sub, dst, &dst_sub, n_files, n_dirs))
                return -1;
        } else {
            uint32_t size = rd32(e + DE_SIZE);
            uint32_t first = rd16(e + DE_CLUSTER);
            uint8_t *buf = size ? read_file_data(src, first, size) : NULL;
            if (size && !buf) {
                fprintf(stderr, "error: 読み出し失敗: %s\n", nm);
                return -1;
            }
            int rc = put_file(dst, ddir, base, ext, buf, size, mtime, attr);
            free(buf);
            if (rc)
                return -1;
            if (n_files)
                (*n_files)++;
        }
    }
    return 0;
}

/* copyall: src イメージの src_path 配下を丸ごと dst イメージの dst_path へ再帰コピーする
 * （実機 Human68k の `COPYALL /S A:\*.* C:` 相当）。異なる2つのイメージファイルを
 * 対象にする点が他コマンドと異なる（main.c で両方 image_load 済みのものを渡す）。 */
int cmd_copyall(image_t *src, const char *src_path, image_t *dst, const char *dst_path) {
    char spsjis[512], dpsjis[512];
    if (utf8_to_sjis(src_path, spsjis, sizeof(spsjis)) < 0 ||
        utf8_to_sjis(dst_path, dpsjis, sizeof(dpsjis)) < 0) {
        fprintf(stderr, "error: 変換失敗\n");
        return -1;
    }
    dir_t sdir;
    if (resolve_dir(src, spsjis, &sdir)) {
        fprintf(stderr, "error: コピー元ディレクトリ '%s' が見つかりません\n", src_path);
        return -1;
    }
    dir_t ddir;
    if (ensure_dir_path(dst, dpsjis, &ddir))
        return -1;

    int n_files = 0, n_dirs = 0;
    if (copy_tree(src, &sdir, dst, &ddir, &n_files, &n_dirs)) {
        image_writeback(dst); /* 途中まででも書き戻す（部分失敗時に全ロストを避ける） */
        return -1;
    }
    if (image_writeback(dst))
        return -1;
    printf("copyall: %s:%s -> %s:%s (%d file(s), %d dir(s))\n", src->path, src_path, dst->path,
           dst_path, n_files, n_dirs);
    return 0;
}

/* サブディレクトリが空か（. .. 以外のエントリが無いか） */
static int dir_is_empty(image_t *im, uint32_t first_cluster) {
    uint32_t per = im->cluster_bytes / DE_SIZE_B;
    uint32_t cl = first_cluster;
    while (cl >= 2 && !fat_is_eoc(im, cl)) {
        uint8_t *base = cluster_ptr(im, cl);
        for (uint32_t i = 0; i < per; i++) {
            uint8_t *e = base + (size_t)i * DE_SIZE_B;
            if (e[DE_NAME] == 0x00)
                return 1;
            if (e[DE_NAME] == 0xE5)
                continue;
            char nm[NAMEBUF];
            decode_name(e, nm);
            if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0)
                continue;
            return 0;
        }
        cl = fat_get(im, cl);
    }
    return 1;
}

int cmd_rmdir(image_t *im, const char *path) {
    char psjis[512];
    if (utf8_to_sjis(path, psjis, sizeof(psjis)) < 0) {
        fprintf(stderr, "error: 変換失敗\n");
        return -1;
    }
    dir_t parent;
    char leaf[64];
    if (resolve_parent(im, psjis, &parent, leaf, sizeof(leaf)))
        return -1;
    uint8_t *e = dir_find(im, &parent, leaf, 1); /* DIR のみ */
    if (!e) {
        fprintf(stderr, "error: ディレクトリ '%s' が見つかりません\n", path);
        return -1;
    }
    uint32_t cl = rd16(e + DE_CLUSTER);
    if (!dir_is_empty(im, cl)) {
        fprintf(stderr, "error: '%s' は空ではありません\n", path);
        return -1;
    }
    free_chain(im, cl);
    e[DE_NAME] = 0xE5;
    if (image_writeback(im))
        return -1;
    printf("rmdir: %s\n", path);
    return 0;
}

/* SCSI HDD (.HDS) 用フォーマットデータを作る（XEiJ SCMedia.java 互換、
 * design_hu68disk.md §4 参照）。
 *
 * 実データ検証で判明: SCSI Disk IPL(0x0400)/SCSI デバイスドライバ(0x0C00)/
 * パーティション IPL(0x8000〜) のブートコードが無いと、BPB・パーティションテーブルが
 * 完全に正しくても Human68k はドライブとして認識しない（このコード自体がドライブ登録を
 * 行っていると見られる）。よってブート非対応のデータディスクでも常にこれらを書く
 * （`scsi_ipl_data.h`、XEiJ 由来）。「ブート専用ドライブから起動する」機能自体
 * （Issue #22 の領分）は依然非スコープ。
 *
 * sectorsPerCluster=1 固定で FAT16 領域サイズを自己無矛盾に決める（HumanMedia.java の
 * 一般化アルゴリズムの写経ではなく、独自の収束計算。10/20/40MB 等 X68000 実機 HDD の
 * 典型サイズ帯で FAT16 容量に収まることが前提）。
 * 成功時 malloc 済みバッファを返し *out_size に総バイト数を書く。失敗時 NULL。 */
/* Builds one partition using the proven Phase 3 geometry calculation.  The
 * multi-volume wrapper below reuses this byte-compatible formatter per slot. */
static uint8_t *make_scsi_single_format(uint32_t size_mb, const char *label, int removable,
                                        size_t *out_size, uint32_t *out_partition_sectors) {
    const uint32_t bytes_per_record = 512;
    const uint32_t bps = 1024, nfats = 2, reserved = 1, root_ent = 512;
    const uint32_t part_start_sector = 32;  /* = 32768 / 1024（XEiJ 固定値） */
    const uint32_t bytes_shift_sector = 10; /* log2(bps) */
    const uint32_t max_clusters = 65520;    /* HumanMedia.java の上限（0xfff0） */

    uint64_t disk_end_record = (32768ULL + (uint64_t)size_mb * 1024 * 1024) / bytes_per_record;
    uint64_t disk_end_byte = (uint64_t)bytes_per_record * disk_end_record;
    uint32_t disk_end_sector = (uint32_t)(disk_end_byte / bps);
    if (disk_end_sector <= part_start_sector) {
        fprintf(stderr, "error: --size が小さすぎます\n");
        return NULL;
    }
    uint32_t partition_sectors = disk_end_sector - part_start_sector;

    uint32_t root_sectors = (root_ent * DE_SIZE_B + bps - 1) / bps; /* 512 エントリ -> 16 */
    if (partition_sectors <= reserved + root_sectors) {
        fprintf(stderr, "error: --size が小さすぎます\n");
        return NULL;
    }
    uint32_t fat_and_data_sectors = partition_sectors - reserved - root_sectors;

    /* 1クラスタあたりのセクタ数を決める（HumanMedia.java L93-113 を移植）。データ領域の
     * クラスタ数の上限を65520として、なるべく小さい2^nのクラスタサイズを選ぶ。
     * 65520クラスタ分のFAT領域に必要なセクタ数はクラスタサイズによらない定数
     * （FATエントリ数=クラスタ数なので）。これに全パーティションが収まらないほど
     * 容量が大きい場合はクラスタサイズを2倍ずつ増やしていく（＝1GB級のHDDでも
     * FAT16のクラスタ数上限内に収める、実機の仕様）。 */
    uint32_t full_fat16_sectors = (2 * (max_clusters + 2) + bps - 1) >> bytes_shift_sector;
    uint32_t shift = 0;
    while (full_fat16_sectors * nfats + (max_clusters << shift) < fat_and_data_sectors) {
        shift++;
        if (shift > 24) {
            fprintf(stderr, "error: --size が大きすぎます\n");
            return NULL;
        }
    }
    uint32_t spc = 1u << shift;

    /* データ領域のクラスタ数+2・1個のFAT領域のセクタ数を決める（HumanMedia.java L144,
     * 149-151）。
     * 最大値で仮のFAT領域セクタ数を見積もり、それを差し引いた残りで実際のクラスタ数を
     * 確定する（1回の補正で収束する、写経元コメント参照）。 */
    uint32_t data_clusters_plus2 = (fat_and_data_sectors >> shift) + 2;
    uint32_t fat_sectors_estimate = (2 * data_clusters_plus2 + bps - 1) >> bytes_shift_sector;
    if (fat_and_data_sectors < fat_sectors_estimate * nfats) {
        fprintf(stderr, "error: --size が小さすぎます（FAT 領域が確保できません）\n");
        return NULL;
    }
    data_clusters_plus2 = ((fat_and_data_sectors - fat_sectors_estimate * nfats) >> shift) + 2;
    uint32_t fat_sectors =
        (fat_and_data_sectors - ((data_clusters_plus2 - 2) << shift)) / nfats;

    /* SCSI HDD は常に FAT16 として書く（FAT12 へのフォールバックは非対応）。
     * FAT12/16の境界は実データ領域クラスタ数4085（HumanMedia.java L117-122）。 */
    if (data_clusters_plus2 - 2 < 4085) {
        fprintf(stderr, "error: --size が小さすぎます（SCSI HDD は FAT16 専用、4085クラスタ以上"
                        "必要です）\n");
        return NULL;
    }
    if (data_clusters_plus2 > max_clusters + 4) {
        fprintf(stderr, "error: --size が大きすぎます（FAT16 のクラスタ数上限を超過）\n");
        return NULL;
    }

    size_t size = (size_t)disk_end_byte;
    uint8_t *d = calloc(1, size);
    if (!d)
        return NULL;

    /* SCSI デバイスパラメータ（0x0000, BE。§2.2）。
     * $0E=RW($00以外=拡張Read/Write可)、$0F=EJ($00以外=EJECT/メディア交換可)。
     * removable指定時はEJ=$01にしてMO等のリムーバブルメディアとして扱う
     * （Issue #38、inside_x68000_1992.md図9）。 */
    memcpy(d + 0x00, "X68SCSI1", 8);
    wr16be(d + 0x08, (uint16_t)bytes_per_record);
    wr32be(d + 0x0A, (uint32_t)disk_end_record);
    d[0x0E] = 0x01;
    d[0x0F] = removable ? 0x01u : 0x00u;
    memcpy(d + 0x10, "Human68K SCSI-DISK by Keisoku Giken", 36);

    /* SCSI Disk IPL(0x0400) / デバイスドライバ(0x0C00)。実データ検証済み: これが無いと
     * ドライブとして認識されない（上記コメント参照）。 */
    memcpy(d + 0x0400, SPC_DISK_IPL, SPC_DISK_IPL_LEN);
    memcpy(d + 0x0C00, SPC_DEVICE_DRIVER, SPC_DEVICE_DRIVER_LEN);

    /* パーティションテーブル（0x0800, BE。§2.3。単一パーティション、flag=0=自動起動） */
    memcpy(d + 0x0800, "X68K", 4);
    wr32be(d + 0x0804, disk_end_sector);
    wr32be(d + 0x0808, disk_end_sector);
    wr32be(d + 0x080C, disk_end_sector);
    memcpy(d + 0x0810, "Huma", 4);
    memcpy(d + 0x0814, "n68k", 4);
    wr32be(d + 0x0818, part_start_sector);
    wr32be(d + 0x081C, partition_sectors);

    /* パーティション先頭: パーティション IPL(実コード) + BPB（0x8000, BE。§2.4） */
    uint32_t part_off = part_start_sector * bps;
    uint8_t *pb = d + part_off;
    memcpy(pb, SPC_PARTITION_IPL, SPC_PARTITION_IPL_LEN);
    wr16be(pb + 0x12, (uint16_t)bps);
    pb[0x14] = (uint8_t)spc;
    pb[0x15] = (uint8_t)nfats;
    wr16be(pb + 0x16, (uint16_t)reserved);
    wr16be(pb + 0x18, (uint16_t)root_ent);
    wr16be(pb + 0x1A, 0); /* SCSI は 0 固定、+0x1E(long) を使う */
    pb[0x1C] = 0xF7;      /* FATID (SCSI FAT16) */
    pb[0x1D] = (uint8_t)fat_sectors;
    wr32be(pb + 0x1E, partition_sectors);
    wr32be(pb + 0x22, part_start_sector);

    /* 各 FAT の先頭に FAT16 マーカー（F7 FF FF FF、実データ確認済み） */
    for (uint32_t n = 0; n < nfats; n++) {
        uint8_t *f = pb + (size_t)(reserved + n * fat_sectors) * bps;
        f[0] = 0xF7;
        f[1] = 0xFF;
        f[2] = 0xFF;
        f[3] = 0xFF;
    }

    /* ボリュームラベル（任意、§5.5） */
    if (label && label[0]) {
        char lsjis[64];
        if (utf8_to_sjis(label, lsjis, sizeof(lsjis)) < 0) {
            fprintf(stderr, "error: ラベルの文字コード変換に失敗しました\n");
            free(d);
            return NULL;
        }
        uint32_t root_start_sector = reserved + fat_sectors * nfats;
        uint8_t *root = pb + (size_t)root_start_sector * bps;
        memset(root, ' ', 11);
        size_t llen = strlen(lsjis);
        if (llen > 11)
            llen = 11;
        memcpy(root, lsjis, llen);
        root[DE_ATTR] = ATTR_VOLUME;
    }

    *out_size = size;
    *out_partition_sectors = partition_sectors;
    return d;
}

/* Assemble a SCSI disk from independently planned FAT16 volumes.  Reusing the
 * former single-volume formatter keeps its layout byte-for-byte identical for
 * the one-volume case while making each volume's BPB/FAT self-contained. */
static uint8_t *make_scsi_format(uint32_t size_mb, const scsi_partition_spec_t *specs,
                                 int spec_count, int removable, size_t *out_size,
                                 uint32_t *out_partition_sectors) {
    const uint32_t start0 = 32;
    uint64_t total_mb = 0;
    if (spec_count <= 0)
        return NULL;
    for (int i = 0; i < spec_count; i++)
        total_mb += specs[i].size_mb;
    if (total_mb == 0 || total_mb > UINT32_MAX || (size_mb && total_mb != size_mb)) {
        fprintf(stderr, "error: --size と --volume の合計が一致しません\n");
        return NULL;
    }
    uint64_t end_sector64 = start0 + total_mb * 1024;
    uint64_t total_bytes64 = end_sector64 * 1024;
    if (end_sector64 > 0x00ffffffu || total_bytes64 > SIZE_MAX) {
        fprintf(stderr, "error: --size が大きすぎます\n");
        return NULL;
    }
    size_t first_size;
    uint32_t first_sectors;
    uint8_t *first = make_scsi_single_format(specs[0].size_mb, specs[0].label, removable,
                                             &first_size, &first_sectors);
    if (!first)
        return NULL;
    size_t total_bytes = (size_t)total_bytes64;
    uint8_t *d = calloc(1, total_bytes);
    if (!d) {
        free(first);
        return NULL;
    }
    memcpy(d, first, start0 * 1024); /* device parameter, IPL and driver */
    free(first);
    uint32_t end_sector = (uint32_t)end_sector64;
    wr32be(d + 0x0a, (uint32_t)(total_bytes / 512));
    memcpy(d + 0x0800, "X68K", 4);
    wr32be(d + 0x0804, end_sector);
    wr32be(d + 0x0808, end_sector);
    wr32be(d + 0x080c, end_sector);
    uint32_t start = start0;
    for (int i = 0; i < spec_count; i++) {
        size_t one_size;
        uint32_t sectors;
        uint8_t *one;
        if (i == 0) {
            one = make_scsi_single_format(specs[i].size_mb, specs[i].label, removable,
                                          &one_size, &sectors);
        } else {
            one = make_scsi_single_format(specs[i].size_mb, specs[i].label, removable,
                                          &one_size, &sectors);
        }
        if (!one || (uint64_t)start + sectors > end_sector) {
            free(one);
            free(d);
            return NULL;
        }
        memcpy(d + (size_t)start * 1024, one + start0 * 1024, (size_t)sectors * 1024);
        wr32be(d + (size_t)start * 1024 + 0x22, start); /* absolute hidden sectors */
        uint8_t *entry = d + 0x0810 + (size_t)i * SCSI_PART_ENTRY_SIZE;
        memcpy(entry, "Human68k", 8);
        wr32be(entry + 8, ((uint32_t)(i == 0 ? 0 : 2) << 24) | start);
        wr32be(entry + 12, sectors);
        start += sectors;
        free(one);
    }
    *out_size = total_bytes;
    *out_partition_sectors = spec_count == 1 ? end_sector - start0 : end_sector;
    return d;
}

/* 領域を確保しない空の SCSI HDD イメージを作る（XEiJ の「領域確保」OFF 相当。
 * SCMedia.java scmMakeFormatData() の partitioningOn=false 経路）。
 *
 * ディスク先頭 32KB のヘッダ（SCSI ディスクパラメータ・Disk IPL・デバイスドライバ・
 * パーティションテーブル署名）だけを作り、パーティションエントリ（0x0810〜）も
 * パーティション領域（0x8000〜）も 0 のままにする。Human68k から見ると「初期化済み
 * だが領域未確保のディスク」であり、NetBSD/x68k のように自前でディスクラベルと
 * ファイルシステムを書く OS のインストール先に使う。
 *
 * ヘッダ（32KB）だけを malloc して返し、*out_total_bytes にディスク全体のバイト数を
 * 書く。残りの領域は呼び出し側がファイル末尾を伸ばして 0 で確保する。失敗時 NULL。 */
static uint8_t *make_scsi_empty_format(uint32_t size_mb, int removable, size_t *out_header_size,
                                       uint64_t *out_total_bytes, uint32_t *out_disk_sectors) {
    const uint32_t bytes_per_record = 512;
    const uint32_t bps = 1024;
    const uint32_t part_start_sector = 32; /* = 32768 / 1024（XEiJ 固定値） */
    const size_t header_size = (size_t)part_start_sector * bps;

    uint64_t disk_end_record = (32768ULL + (uint64_t)size_mb * 1024 * 1024) / bytes_per_record;
    uint64_t disk_end_byte = (uint64_t)bytes_per_record * disk_end_record;
    uint64_t disk_end_sector = disk_end_byte / bps;
    if (disk_end_sector <= part_start_sector) {
        fprintf(stderr, "error: --size が小さすぎます\n");
        return NULL;
    }
    if (disk_end_sector > 0x00ffffffu) { /* 開始セクタ番号は 24 ビット */
        fprintf(stderr, "error: --size が大きすぎます\n");
        return NULL;
    }
    uint8_t *d = calloc(1, header_size);
    if (!d)
        return NULL;

    /* SCSI デバイスパラメータ（0x0000, BE）。単一区画版と同一。 */
    memcpy(d + 0x00, "X68SCSI1", 8);
    wr16be(d + 0x08, (uint16_t)bytes_per_record);
    wr32be(d + 0x0A, (uint32_t)disk_end_record);
    d[0x0E] = 0x01;
    d[0x0F] = removable ? 0x01u : 0x00u;
    memcpy(d + 0x10, "Human68K SCSI-DISK by Keisoku Giken", 36);

    /* SCSI Disk IPL(0x0400) / デバイスドライバ(0x0C00)。領域未確保でもドライブとして
     * 認識させるために書く（XEiJ も partitioningOn によらず常に書いている）。 */
    memcpy(d + 0x0400, SPC_DISK_IPL, SPC_DISK_IPL_LEN);
    memcpy(d + 0x0C00, SPC_DEVICE_DRIVER, SPC_DEVICE_DRIVER_LEN);

    /* パーティションテーブル（0x0800, BE）。+0x04 は「確保済みの最後の区画の末尾までの
     * セクタ数」で、1 つも確保していないので区画領域の開始位置そのものを書く。 */
    memcpy(d + 0x0800, "X68K", 4);
    wr32be(d + 0x0804, part_start_sector);
    wr32be(d + 0x0808, (uint32_t)disk_end_sector);
    wr32be(d + 0x080C, (uint32_t)disk_end_sector);

    *out_header_size = header_size;
    *out_total_bytes = disk_end_byte;
    *out_disk_sectors = (uint32_t)disk_end_sector;
    return d;
}

static void partition_label(image_t *im, char *out, size_t outsz) {
    uint8_t *root = image_ptr(im, (uint64_t)im->root_start_sec * im->bytes_per_sector,
                              (size_t)im->root_entries * DE_SIZE_B);
    for (uint32_t i = 0; i < im->root_entries; i++) {
        uint8_t *e = root + (size_t)i * DE_SIZE_B;
        if (e[DE_NAME] == 0)
            break;
        if ((e[DE_ATTR] & ATTR_VOLUME) != 0) {
            char raw[12];
            memcpy(raw, e, 11);
            raw[11] = 0;
            for (int j = 10; j >= 0 && raw[j] == ' '; j--)
                raw[j] = 0;
            if (sjis_to_utf8(raw, strlen(raw), out, outsz) < 0)
                snprintf(out, outsz, "%s", raw);
            return;
        }
    }
    snprintf(out, outsz, "-");
}

int cmd_partitions(image_t *im, int all) {
    if (im->kind != IMG_SCSI_HDD) {
        fprintf(stderr, "error: SCSI パーティションテーブルを持たないイメージです\n");
        return -1;
    }
    printf("Part  Status    Type       Start    Sectors  Size(MiB)  Label\n");
    for (int i = 0; i < SCSI_PART_TABLE_MAX; i++) {
        partition_t *p = &im->partitions[i];
        if (!all && !p->occupied)
            continue;
        char type[9], status[12], label[160] = "-";
        memcpy(type, p->type, 8);
        type[8] = 0;
        for (int n = 7; n >= 0 && type[n] == ' '; n--)
            type[n] = 0;
        if (p->is_human68k)
            snprintf(type, sizeof(type), "Human68k");
        if (p->flag == 0)
            snprintf(status, sizeof(status), "boot");
        else if (p->flag == 1)
            snprintf(status, sizeof(status), "disabled");
        else if (p->flag == 2)
            snprintf(status, sizeof(status), "usable");
        else
            snprintf(status, sizeof(status), "0x%02X", p->flag);
        if (p->is_human68k && p->occupied) {
            /* probe には自前のバッファを持たせる（親の data を解放させない）。
             * ラベル読みに必要なルートディレクトリまでしかマップしない。 */
            image_t probe = *im;
            probe.data = NULL;
            probe.map_offset = 0;
            probe.map_size = 0;
            probe.error_buffer = NULL;
            probe.error_buffer_size = 0;
            if (image_probe_partition(&probe, i + 1) == 0) {
                partition_label(&probe, label, sizeof(label));
                if (probe.part_size != (uint64_t)probe.total_sectors * probe.bytes_per_sector)
                    strncat(label, " (BPB size differs)", sizeof(label) - strlen(label) - 1);
            } else
                snprintf(label, sizeof(label), "<invalid BPB>");
            image_free(&probe);
        }
        if (!p->occupied) {
            snprintf(status, sizeof(status), "-");
            snprintf(type, sizeof(type), "-");
        }
        printf("%-4d  %-8s  %-9s  %7u  %9u  %9.1f  %s\n", i + 1, status, type, p->start_sector,
               p->sector_count, (double)p->byte_size / (1024.0 * 1024.0), label);
    }
    return 0;
}

/* mkfs: 空の Human68k フォーマット済みイメージを新規作成（2HD/FAT12 または SCSI HDD/FAT16）。
 * no_partition=1 のときは SCSI HDD の領域を確保せず、ヘッダだけの空 HDS を作る。
 * 既存ファイルは force=0 なら確認する（誤消去防止）。 */
int cmd_mkfs(const char *path, const char *type, int force, int size_mb, const char *label,
             int removable, int no_partition, const scsi_partition_spec_t *volumes,
             int volume_count) {
    if (type && strcmp(type, "2hd") != 0 && strcmp(type, "scsi") != 0) {
        fprintf(stderr, "error: --type は 2hd または scsi のみ対応\n");
        return -1;
    }
    int is_scsi = type && strcmp(type, "scsi") == 0;
    if (!is_scsi && label) {
        fprintf(stderr, "error: --label は --type scsi のみ対応\n");
        return -1;
    }
    if (!is_scsi && removable) {
        fprintf(stderr, "error: --removable は --type scsi のみ対応\n");
        return -1;
    }
    if (!is_scsi && volume_count) {
        fprintf(stderr, "error: --volume は --type scsi のみ対応\n");
        return -1;
    }
    if (volume_count && label) {
        fprintf(stderr, "error: --volume と --label は併用できません\n");
        return -1;
    }
    if (!is_scsi && no_partition) {
        fprintf(stderr, "error: --no-partition は --type scsi のみ対応\n");
        return -1;
    }
    if (no_partition && (label || volume_count)) {
        fprintf(stderr, "error: --no-partition と --label/--volume は併用できません\n");
        return -1;
    }
    if (!force) {
        FILE *chk = hu_fopen(path, "rb");
        if (chk) { /* 既存ファイル → 確認する */
            fclose(chk);
            fprintf(stderr, "'%s' は既に存在します。フォーマットしますか？ (y/N): ", path);
            fflush(stderr);
            int c = getchar();
            int yes = (c == 'y' || c == 'Y');
            while (c != '\n' && c != EOF)
                c = getchar(); /* 行末まで読み捨て */
            if (!yes) {
                fprintf(stderr, "中止しました\n");
                return -1;
            }
        }
    }
    uint8_t *d;
    size_t size;        /* 実際に書き出すバイト数 */
    uint64_t file_size; /* 最終的なファイルサイズ（size を超える分は 0 で確保する） */
    uint32_t report_sectors;
    const char *report_kind;

    if (is_scsi && no_partition) {
        uint32_t mb = size_mb > 0 ? (uint32_t)size_mb : 10;
        d = make_scsi_empty_format(mb, removable, &size, &file_size, &report_sectors);
        if (!d)
            return -1;
        report_kind = "SCSI HDD, unpartitioned";
    } else if (is_scsi) {
        uint32_t mb = size_mb > 0 ? (uint32_t)size_mb : (volume_count ? 0 : 10);
        scsi_partition_spec_t default_volume = {mb ? mb : 10, label};
        const scsi_partition_spec_t *specs = volume_count ? volumes : &default_volume;
        int count = volume_count ? volume_count : 1;
        d = make_scsi_format(mb, specs, count, removable, &size, &report_sectors);
        if (!d)
            return -1;
        file_size = size;
        report_kind = "SCSI HDD, FAT16";
    } else {
        const uint32_t bps = 1024, total = 1232, spf = 2, nfats = 2, reserved = 1;
        const uint32_t root_ent = 192, spc = 1;
        size = (size_t)total * bps;
        d = calloc(1, size);
        if (!d)
            return -1;
        /* ブートセクタ（BRA.S + OEM + BPB。IPL 本体は無し＝データ専用、B: 等で読める）。
         * IPL コードは含まない。OEM 名 "X68IPL30" は Human68k 3.02 のフォーマットに合わせた
         * 文字列で、Human68k の著作権は権利各社に帰属する。無償公開されたシャープ製ソフトの
         * 許諾条件は LICENSES/Sharp-Products-Users-Forum-License.txt に添付している。 */
        d[0] = 0x60;
        d[1] = 0x3C;
        d[2] = 0x90;
        memcpy(d + 3, "X68IPL30", 8);
        wr16(d + 0x0B, (uint16_t)bps);
        d[0x0D] = (uint8_t)spc;
        wr16(d + 0x0E, (uint16_t)reserved);
        d[0x10] = (uint8_t)nfats;
        wr16(d + 0x11, (uint16_t)root_ent);
        wr16(d + 0x13, (uint16_t)total);
        d[0x15] = 0xFE;
        wr16(d + 0x16, (uint16_t)spf);
        wr16(d + 0x18, 8);
        wr16(d + 0x1A, 2); /* sectors/track, heads */
        /* 各 FAT の先頭予約: FE FF FF（media 0xFE の 1.5 バイト FAT） */
        for (uint32_t n = 0; n < nfats; n++) {
            uint8_t *f = d + (size_t)(reserved + n * spf) * bps;
            f[0] = 0xFE;
            f[1] = 0xFF;
            f[2] = 0xFF;
        }
        report_sectors = total;
        file_size = size;
        report_kind = "2HD, FAT12";
    }
    FILE *fp = hu_fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "error: cannot create '%s'\n", path);
        free(d);
        return -1;
    }
    if (fwrite(d, 1, size, fp) != size) {
        fprintf(stderr, "error: write failed\n");
        fclose(fp);
        free(d);
        return -1;
    }
    /* 残りは 0 のまま末尾を伸ばして確保する。穴あきファイルに対応したファイルシステム
     * なら実消費は書き出した分だけで済む。 */
    if (file_size > size) {
        if (file_size - 1 > (uint64_t)LONG_MAX) {
            fprintf(stderr, "error: この環境では扱えないサイズです\n");
            fclose(fp);
            free(d);
            return -1;
        }
        if (fseek(fp, (long)(file_size - 1), SEEK_SET) || fputc(0, fp) == EOF) {
            fprintf(stderr, "error: write failed\n");
            fclose(fp);
            free(d);
            return -1;
        }
    }
    fclose(fp);
    free(d);
    printf("mkfs: %s (%s, %u sectors)\n", path, report_kind, report_sectors);
    if (is_scsi && volume_count > 1) {
        uint32_t start = 32;
        for (int i = 0; i < volume_count; i++) {
            printf("  part %d: start %u, %u MiB%s%s\n", i + 1, start, volumes[i].size_mb,
                   volumes[i].label ? ", label " : "",
                   volumes[i].label ? volumes[i].label : "");
            start += volumes[i].size_mb * 1024;
        }
    }
    return 0;
}
