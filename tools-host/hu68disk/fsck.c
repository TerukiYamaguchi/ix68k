/*
 * fsck.c - Human68k ディスクイメージ整合性検査・修復
 *
 * 検査は Pass 0(ジオメトリ)→ Pass 1(FATテーブル)→ Pass 2(ディレクトリツリー+クラスタ
 * 所有権)→ Pass 3(lost cluster) の順。詳細は design_hu68disk.md 参照。
 *
 * --fix の流れ（cmd_fsck）:
 *   1. 静かに一巡（quiet=1, fix=0）して問題数を数える（確認・バックアップ要否の判断用）
 *   2. 問題0ならそのまま診断出力を見せて終了
 *   3. 確認プロンプト（--yes で省略）・バックアップ作成（--no-backup で省略）
 *   4. 実際の修復パス（quiet=0, fix=1）→ FATコピー同期 → 書き戻し
 *   5. 二巡目をもう一度静かに実行し、残った問題数で最終結果を決める
 */
#include "hu68disk.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"

/* ---- 検査用コンテキスト ---- */
typedef struct {
    uint32_t
        *owner; /* クラスタ番号 -> 所有ファイルID（0=未所有）。添字はクラスタ番号そのもの */
    uint32_t nclusters; /* owner の要素数（= total_clusters + 2） */
    uint32_t next_file_id;
    uint32_t *visited; /* 訪問済みサブディレクトリの先頭クラスタ（循環検出用） */
    uint32_t visited_count;
    uint32_t visited_cap;
    int problems;
    int fixed;
    int fix;   /* 1 なら実際に修復を書き込む */
    int quiet; /* 1 なら printf を一切出さない（内部の下見・二巡目確認用） */
    int verbose;
    int files_scanned;
    int dirs_scanned;
} fsck_ctx;

/* 問題を1件記録する。ctx->fix なら fixed で「-> fixed / -> could not fix」を出す。
 * ctx->quiet なら件数のみ数えて出力はしない（下見・二巡目確認用）。 */
static void report(fsck_ctx *ctx, const char *where, int fixed, const char *fmt, ...) {
    ctx->problems++;
    if (fixed)
        ctx->fixed++;
    if (ctx->quiet)
        return;
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    printf("  %-24s %s", where, msg);
    if (ctx->fix)
        printf(" -> %s\n", fixed ? "fixed" : "could not fix");
    else
        printf("\n");
}

static void name_for_display(const char *sjis, char *out, size_t outsz) {
    if (sjis_to_utf8(sjis, strlen(sjis), out, outsz) < 0)
        snprintf(out, outsz, "%s", sjis);
}

/* dir.c:split_name と同じ禁止文字判定（制御文字, : ? *）。SJIS リードバイトは読み飛ばす。 */
static int name_has_invalid_char(const char *sjis) {
    size_t len = strlen(sjis);
    for (size_t i = 0; i < len;) {
        unsigned char c = (unsigned char)sjis[i];
        if (is_sjis_lead(c) && i + 1 < len) {
            i += 2;
            continue;
        }
        if (c < 0x20 || c == ':' || c == '?' || c == '*')
            return 1;
        i++;
    }
    return 0;
}

static void join_path(char *out, size_t outsz, const char *parent, const char *name) {
    if (parent[0] == '\0')
        snprintf(out, outsz, "/%s", name);
    else
        snprintf(out, outsz, "%s/%s", parent, name);
}

static int is_visited(const fsck_ctx *ctx, uint32_t cl) {
    for (uint32_t i = 0; i < ctx->visited_count; i++)
        if (ctx->visited[i] == cl)
            return 1;
    return 0;
}

static void mark_visited(fsck_ctx *ctx, uint32_t cl) {
    if (ctx->visited_count == ctx->visited_cap) {
        ctx->visited_cap = ctx->visited_cap ? ctx->visited_cap * 2 : 64;
        ctx->visited = realloc(ctx->visited, ctx->visited_cap * sizeof(uint32_t));
    }
    ctx->visited[ctx->visited_count++] = cl;
}

/* first から数えて n 番目（1始まり）のクラスタ番号を返す。チェーンが n に届く前に
 * 終端/破損したら 0 を返す。呼び出し側は「n はこのチェーンで既に確認済みの位置」
 * である前提（check_chain がチェーンを1回歩いた直後に使う）。 */
static uint32_t nth_cluster_in_chain(image_t *im, uint32_t first, uint32_t n) {
    uint32_t cl = first;
    for (uint32_t i = 1; i < n; i++) {
        uint32_t v = fat_get(im, cl);
        if (fat_is_eoc(im, v) || v < 2)
            return 0;
        cl = v;
    }
    return cl;
}

/* ---- Pass 0: ジオメトリ健全性。致命的な場合は非0を返す（以降のパスを中断） ---- */
static int pass0_geometry(image_t *im, fsck_ctx *ctx) {
    int fatal = 0;
    if (im->data_start_sec >= im->total_sectors) {
        report(ctx, "BPB", 0, "データ領域の開始セクタ(%u)が総セクタ数(%u)を超えています",
               im->data_start_sec, im->total_sectors);
        fatal = 1;
    }
    if (!(im->fat_start_sec < im->root_start_sec && im->root_start_sec < im->data_start_sec)) {
        report(ctx, "BPB", 0, "FAT/ルート/データ領域の順序が不正です (fat=%u root=%u data=%u)",
               im->fat_start_sec, im->root_start_sec, im->data_start_sec);
        fatal = 1;
    }
    if (im->fat_bits == 12 && im->total_clusters >= 4085) {
        report(ctx, "BPB", 0, "FAT12 なのにクラスタ数が %u です（4085 以上は FAT16 のはず）",
               im->total_clusters);
        fatal = 1;
    }
    if (im->fat_bits == 16 && im->total_clusters >= 65525) {
        report(ctx, "BPB", 0, "FAT16 のクラスタ数が上限(65524)を超えています（%u）",
               im->total_clusters);
        fatal = 1;
    }
    uint64_t bpb_bytes = (uint64_t)im->total_sectors * im->bytes_per_sector;
    if (bpb_bytes > im->part_size) {
        report(
            ctx, "BPB", 0,
            "BPB容量が選択パーティションを超えています（必要 %llu バイト、区画 %llu バイト）",
            (unsigned long long)bpb_bytes, (unsigned long long)im->part_size);
        fatal = 1;
    } else if (im->kind == IMG_SCSI_HDD && bpb_bytes != im->part_size) {
        report(ctx, "BPB", 0,
               "BPB容量(%llu)とパーティション容量(%llu)が一致しません（未使用末尾領域）",
               (unsigned long long)bpb_bytes, (unsigned long long)im->part_size);
    }
    return fatal;
}

/* ---- Pass 1: FAT テーブル自己検査（予約マーカーのみ） ----
 * 第2FAT以降との一致比較は行わない: 『C Compiler PRO-68K ver2.0 プログラマーズ
 * マニュアル』(XC20) 付録2に「第2FAT（未使用）」と明記されており、Human68k は
 * そもそも第2FATを維持しない。実イメージ（XEiJ生成のref_scsi10.HDSを含む）でも
 * 常時食い違っており、これを「問題」として数えると実運用で誤検知だらけになる
 * ため検出しない（2026-07-13、design_hu68disk.md 参照）。
 * 修復: 予約マーカーは標準値へ上書き（fat_set は全コピーに反映）。 */
static void pass1_fat_tables(image_t *im, fsck_ctx *ctx) {
    uint32_t exp_c0, exp_c1;
    if (im->fat_bits == 12) {
        exp_c0 = 0x0FFE; /* media 0xFE + 予約 */
        exp_c1 = 0x0FFF;
    } else {
        exp_c0 = 0xF7FF; /* SCSI FAT16 予約マーカー */
        exp_c1 = 0xFFFF;
    }
    uint32_t got_c0 = fat_get(im, 0), got_c1 = fat_get(im, 1);
    if (got_c0 != exp_c0 || got_c1 != exp_c1) {
        int fixed = 0;
        if (ctx->fix) {
            fat_set(im, 0, exp_c0);
            fat_set(im, 1, exp_c1);
            fixed = 1;
        }
        report(ctx, "FAT", fixed,
               "予約エントリ(cluster0/1)のマーカーが標準と異なります（cluster0=0x%X, "
               "cluster1=0x%X）",
               got_c0, got_c1);
    }
}

/* 空ファイル用に 1 クラスタ割り当て、エントリの開始クラスタに書く（--fix 用）。
 * Human68k は空ファイルにもクラスタを持たせる前提で、開始クラスタ 0 のエントリへの
 * ゼロ長 _WRITE は FAT[0] を通常クラスタとして辿り、白帯になる。成功なら 1、空き無しなら 0。 */
static int alloc_empty_file_cluster(image_t *im, fsck_ctx *ctx, uint8_t *e) {
    uint32_t c = fat_find_free(im, 2);
    if (!c)
        return 0;
    fat_set(im, c, fat_eoc(im));
    memset(cluster_ptr(im, c), 0, im->cluster_bytes);
    ctx->owner[c] = ctx->next_file_id++;
    wr16(e + DE_CLUSTER, (uint16_t)c);
    return 1;
}

/* ---- Pass 2: クラスタチェーン検査 ＋ 所有権マーク ＋ 修復 ----
 * is_dir: ディレクトリのクラスタ連鎖は DE_SIZE によるサイズ整合検査をスキップする
 * （Human68k はディレクトリの DE_SIZE を使わないため）。
 * e: 該当ディレクトリエントリへのポインタ（修復時に DE_CLUSTER/DE_SIZE を書き換える）。
 *
 * 修復方針（design_hu68disk.md §5）: チェーン破損（範囲外/free参照/循環/
 * クロスリンク）は「自ファイルの最後の正当なクラスタで EOC 打ち切り」に統一。
 * クロスリンクの場合、共有クラスタとその先は「先に見つかった別ファイル」の領域なので
 * 一切 free しない。破損以外（正常終端したがサイズ不整合）は、余剰なら free_chain で
 * 解放、不足なら DE_SIZE を実チェーン長に縮小。 */
static void check_chain(image_t *im, fsck_ctx *ctx, const char *where, uint8_t *e,
                        uint32_t first, uint32_t size, int is_dir) {
    if (first == 0) {
        if (is_dir) {
            report(ctx, where, 0,
                   "ディレクトリなのに割り当てクラスタがありません（DE_CLUSTER=0）");
        } else {
            if (size != 0) {
                int fixed = 0;
                if (ctx->fix) {
                    wr32(e + DE_SIZE, 0);
                    fixed = 1;
                }
                report(ctx, where, fixed, "DE_CLUSTER=0 だが DE_SIZE=%u（矛盾）", size);
            }
            int fixed = 0;
            if (ctx->fix)
                fixed = alloc_empty_file_cluster(im, ctx, e);
            report(ctx, where, fixed,
                   "開始クラスタ 0 のファイルです（Human68k ではゼロ長書き込みで白帯の原因に"
                   "なります。--fix で 1 クラスタ割り当てます）");
        }
        return;
    }

    uint32_t file_id = ctx->next_file_id++;
    uint32_t cl = first;
    uint32_t chain_len = 0;
    uint32_t max_steps = ctx->nclusters + 1;
    int broke = 0;
    char break_msg[128] = {0};
    for (;;) {
        if (cl < 2 || cl >= ctx->nclusters) {
            snprintf(break_msg, sizeof(break_msg),
                     "クラスタ番号 %u が範囲外です（チェーン破損）", cl);
            broke = 1;
            break;
        }
        uint32_t v = fat_get(im, cl);
        if (fat_is_free(v)) {
            snprintf(break_msg, sizeof(break_msg),
                     "クラスタ %u が未使用領域を参照しています（チェーン破損）", cl);
            broke = 1;
            break;
        }
        if (ctx->owner[cl] == file_id) {
            snprintf(break_msg, sizeof(break_msg), "クラスタ %u で循環チェーンを検出しました",
                     cl);
            broke = 1;
            break;
        }
        if (ctx->owner[cl] != 0) {
            snprintf(break_msg, sizeof(break_msg),
                     "クラスタ %u が別ファイルと共有されています（クロスリンク）", cl);
            broke = 1;
            break;
        }
        ctx->owner[cl] = file_id;
        chain_len++;
        if (chain_len > max_steps) {
            snprintf(break_msg, sizeof(break_msg),
                     "チェーンが異常に長く安全上限を超えました（循環の疑い）");
            broke = 1;
            break;
        }
        if (fat_is_eoc(im, v))
            break;
        cl = v;
    }

    if (broke) {
        int fixed = 0;
        if (ctx->fix) {
            if (chain_len == 0) {
                wr16(e + DE_CLUSTER, 0);
                if (!is_dir) {
                    wr32(e + DE_SIZE, 0);
                    alloc_empty_file_cluster(im, ctx, e);
                }
            } else {
                uint32_t prev = nth_cluster_in_chain(im, first, chain_len);
                fat_set(im, prev, fat_eoc(im));
                if (!is_dir)
                    wr32(e + DE_SIZE, chain_len * im->cluster_bytes);
            }
            fixed = 1;
        }
        report(ctx, where, fixed, "%s", break_msg);
        return;
    }

    if (is_dir)
        return;

    /* 想定クラスタ数。空ファイルも 1 クラスタ持つのが Human68k の形（上記の理由） */
    uint32_t expected = size ? (size + im->cluster_bytes - 1) / im->cluster_bytes : 1;
    if (chain_len > expected) {
        int fixed = 0;
        if (ctx->fix) {
            uint32_t cut_at = nth_cluster_in_chain(im, first, expected);
            uint32_t next = fat_get(im, cut_at);
            fat_set(im, cut_at, fat_eoc(im));
            if (!fat_is_eoc(im, next) && next >= 2)
                free_chain(im, next);
            fixed = 1;
        }
        report(ctx, where, fixed,
               "サイズ %u バイトに対しチェーンが %u クラスタ（想定 %u、余剰 %u）", size,
               chain_len, expected, chain_len - expected);
    } else if (chain_len < expected) {
        int fixed = 0;
        if (ctx->fix) {
            wr32(e + DE_SIZE, chain_len * im->cluster_bytes);
            fixed = 1;
        }
        report(ctx, where, fixed,
               "サイズ %u バイトに対しチェーンが %u クラスタ（想定 %u、不足 %u）", size,
               chain_len, expected, expected - chain_len);
    }
}

/* ディレクトリツリーを深さ優先で走査する。dir_first_cluster は自分自身の先頭クラスタ
 * （is_root なら未使用）。parent_first_cluster/parent_is_root は '..' の期待値算出用。 */
static void walk_dir(image_t *im, fsck_ctx *ctx, uint32_t dir_first_cluster, int is_root,
                     uint32_t parent_first_cluster, int parent_is_root, const char *pathbuf) {
    if (!is_root)
        ctx->dirs_scanned++;
    uint32_t per = im->cluster_bytes / DE_SIZE_B;
    uint32_t cl = is_root ? 0 : dir_first_cluster;
    uint32_t idx = 0;
    int seen_dot = 0, seen_dotdot = 0;
    for (;;) {
        uint8_t *e;
        if (is_root) {
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
        char sjis[NAMEBUF];
        decode_name(e, sjis);

        if (attr == ATTR_LFN) {
            char disp[160];
            name_for_display(sjis, disp, sizeof(disp));
            char childpath[600];
            join_path(childpath, sizeof(childpath), pathbuf, disp);
            int fixed = 0;
            if (ctx->fix) {
                e[DE_NAME] = 0xE5;
                fixed = 1;
            }
            report(ctx, childpath, fixed,
                   "VFAT ロングファイル名エントリ（属性 0x0F, Human68k には異物）");
            continue;
        }
        if (attr & ATTR_VOLUME)
            continue; /* ボリュームラベルはクラスタを持たない */

        int is_dot = strcmp(sjis, ".") == 0;
        int is_dotdot = strcmp(sjis, "..") == 0;
        if (is_dot || is_dotdot) {
            if (is_root) {
                report(ctx, "/", 0, "ルートディレクトリに '%s' エントリがあります（想定外）",
                       sjis);
                continue;
            }
            uint32_t cluster_field = rd16(e + DE_CLUSTER);
            if (is_dot) {
                seen_dot = 1;
                if (cluster_field != dir_first_cluster) {
                    int fixed = 0;
                    if (ctx->fix) {
                        wr16(e + DE_CLUSTER, (uint16_t)dir_first_cluster);
                        fixed = 1;
                    }
                    report(ctx, pathbuf, fixed,
                           "'.' がクラスタ %u を指していますが %u であるべきです",
                           cluster_field, dir_first_cluster);
                }
            } else {
                seen_dotdot = 1;
                uint32_t expected = parent_is_root ? 0 : parent_first_cluster;
                if (cluster_field != expected) {
                    int fixed = 0;
                    if (ctx->fix) {
                        wr16(e + DE_CLUSTER, (uint16_t)expected);
                        fixed = 1;
                    }
                    report(ctx, pathbuf, fixed,
                           "'..' がクラスタ %u を指していますが %u であるべきです",
                           cluster_field, expected);
                }
            }
            continue;
        }

        uint8_t known =
            ATTR_READONLY | ATTR_HIDDEN | ATTR_SYSTEM | ATTR_VOLUME | ATTR_DIR | ATTR_ARCHIVE;
        char disp[160];
        name_for_display(sjis, disp, sizeof(disp));
        char childpath[600];
        join_path(childpath, sizeof(childpath), pathbuf, disp);

        if (attr & (uint8_t)~known) {
            int fixed = 0;
            if (ctx->fix) {
                e[DE_ATTR] &= known;
                fixed = 1;
            }
            report(ctx, childpath, fixed, "未定義の属性ビットが立っています (0x%02X)", attr);
        }
        if (name_has_invalid_char(sjis))
            report(ctx, childpath, 0, "ファイル名に不正な文字が含まれています");

        uint32_t first = rd16(e + DE_CLUSTER);
        uint32_t size = rd32(e + DE_SIZE);
        int is_dir = (attr & ATTR_DIR) != 0;
        check_chain(im, ctx, childpath, e, first, size, is_dir);
        if (!is_dir)
            ctx->files_scanned++;
        if (ctx->fix)
            first = rd16(e + DE_CLUSTER); /* check_chain が修復で書き換えた可能性がある */

        if (is_dir && first != 0) {
            if (is_visited(ctx, first)) {
                report(ctx, childpath, 0,
                       "ディレクトリクラスタ %u は既に別の場所で使われています（循環）", first);
            } else {
                mark_visited(ctx, first);
                walk_dir(im, ctx, first, 0, dir_first_cluster, is_root, childpath);
            }
        }
    }
    if (!is_root) {
        if (!seen_dot)
            report(ctx, pathbuf, 0, "'.' エントリがありません");
        if (!seen_dotdot)
            report(ctx, pathbuf, 0, "'..' エントリがありません");
    }
}

/* ---- Pass 3: lost cluster（どのファイルからも到達されない割当済みクラスタ） ----
 * 修復: 解放する（design_hu68disk.md §5、FILEnnnn.CHK 回収は非スコープ）。 */
static void pass3_lost_clusters(image_t *im, fsck_ctx *ctx) {
    uint32_t bad_marker = im->fat_bits == 12 ? 0x0FF7 : 0xFFF7;
    uint32_t lost_count = 0, runs = 0;
    int in_run = 0;
    for (uint32_t cl = 2; cl < im->total_clusters + 2; cl++) {
        uint32_t v = fat_get(im, cl);
        int is_lost = !fat_is_free(v) && v != bad_marker && ctx->owner[cl] == 0;
        if (is_lost) {
            lost_count++;
            if (!in_run) {
                runs++;
                in_run = 1;
            }
            if (ctx->fix)
                fat_set(im, cl, 0);
        } else {
            in_run = 0;
        }
    }
    if (lost_count > 0)
        report(ctx, "lost", ctx->fix, "%u lost クラスタ（%llu バイト、%u 個の連続区間）",
               lost_count, (unsigned long long)lost_count * im->cluster_bytes, runs);
    else if (ctx->verbose && !ctx->quiet)
        printf("  lost cluster はありません\n");
}

typedef struct {
    int problems;
    int fixed;
    int fatal;
} pass_summary;

/* Pass 0〜3 を一通り実行する。quiet なら printf を一切出さず件数だけ数える
 * （下見・二巡目の確認用）。fix なら各パスが実際にイメージを書き換える。 */
static pass_summary run_passes(image_t *im, int fix, int quiet, int verbose) {
    fsck_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.nclusters = im->total_clusters + 2;
    ctx.owner = calloc(ctx.nclusters, sizeof(uint32_t));
    ctx.next_file_id = 1;
    ctx.fix = fix;
    ctx.quiet = quiet;
    ctx.verbose = verbose;

    pass_summary sum;
    memset(&sum, 0, sizeof(sum));
    if (!ctx.owner) {
        fprintf(stderr, "error: メモリ確保に失敗しました\n");
        sum.fatal = 1;
        return sum;
    }

    if (!quiet)
        printf("Pass 0: geometry\n");
    int before = ctx.problems;
    int fatal = pass0_geometry(im, &ctx);
    if (!quiet && ctx.problems == before && verbose)
        printf("  OK\n");
    if (fatal) {
        if (!quiet)
            printf("\nfatal: ジオメトリが致命的に破損しているため以降のパスを中断しました\n");
        sum.problems = ctx.problems;
        sum.fatal = 1;
        free(ctx.owner);
        free(ctx.visited);
        return sum;
    }

    if (!quiet)
        printf("Pass 1: FAT tables\n");
    before = ctx.problems;
    pass1_fat_tables(im, &ctx);
    if (!quiet && ctx.problems == before && verbose)
        printf("  OK\n");

    if (!quiet)
        printf("Pass 2: directories and files\n");
    before = ctx.problems;
    walk_dir(im, &ctx, 0, 1, 0, 1, "");
    if (!quiet && ctx.problems == before && verbose)
        printf("  OK\n");
    if (!quiet && verbose)
        printf("  scanned %d file(s), %d director(y/ies)\n", ctx.files_scanned,
               ctx.dirs_scanned);

    if (!quiet)
        printf("Pass 3: lost clusters\n");
    pass3_lost_clusters(im, &ctx);

    sum.problems = ctx.problems;
    sum.fixed = ctx.fixed;
    free(ctx.owner);
    free(ctx.visited);
    return sum;
}

/* y/Y のみ真。行末までstdinを読み捨てる（mkfsの確認プロンプトと同じ作法）。 */
static int confirm_prompt(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
    int c = getchar();
    int yes = (c == 'y' || c == 'Y');
    while (c != '\n' && c != EOF)
        c = getchar();
    return yes;
}

int cmd_fsck(image_t *im, int fix, int verbose, int assume_yes, int no_backup) {
    printf("hu68disk fsck: %s  (%s, %u clusters, %u-byte cluster)\n", im->path,
           im->fat_bits == 12 ? "2HD/FAT12" : "HD/FAT16", im->total_clusters,
           im->cluster_bytes);

    if (!fix) {
        pass_summary sum = run_passes(im, 0, 0, verbose);
        printf("\nSummary: %d problem(s) found\n", sum.problems);
        if (!sum.fatal && sum.problems > 0)
            printf("  (run with --fix to repair)\n");
        return (sum.fatal || sum.problems) ? -1 : 0;
    }

    /* --fix: まず静かに問題数を数える（確認・バックアップの要否判断のため） */
    pass_summary pre = run_passes(im, 0, 1, 0);
    if (pre.fatal) {
        printf("fatal: ジオメトリが致命的に破損しているため修復できません\n");
        printf("\nSummary: %d problem(s) found\n", pre.problems);
        return -1;
    }
    if (pre.problems == 0) {
        pass_summary sum = run_passes(im, 0, 0, verbose);
        printf("\nSummary: %d problem(s) found\n", sum.problems);
        return 0;
    }

    if (!assume_yes &&
        !confirm_prompt("%d 件の問題を検出しました。修復しますか？ (y/N): ", pre.problems)) {
        fprintf(stderr, "中止しました\n");
        return -1;
    }

    if (!no_backup) {
        char bak[1200];
        snprintf(bak, sizeof(bak), "%s.bak", im->path);
        FILE *chk = hu_fopen(bak, "rb");
        if (chk) {
            fclose(chk);
            if (!assume_yes &&
                !confirm_prompt("'%s' は既に存在します。上書きしますか？ (y/N): ", bak)) {
                fprintf(stderr, "中止しました\n");
                return -1;
            }
        }
        if (image_backup(im, bak)) {
            fprintf(stderr, "error: バックアップの作成に失敗しました ('%s')\n", bak);
            return -1;
        }
        printf("バックアップを作成しました: %s\n", bak);
    }

    pass_summary fixrun = run_passes(im, 1, 0, verbose);
    if (image_writeback(im))
        return -1;

    pass_summary verify = run_passes(im, 0, 1, 0);

    printf("\nSummary: %d problem(s) found, %d fixed\n", fixrun.problems, fixrun.fixed);
    if (verify.problems > 0) {
        printf("warning: %d problem(s) remain after --fix\n", verify.problems);
        return -1;
    }
    return 0;
}
