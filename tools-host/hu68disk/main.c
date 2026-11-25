/* main.c - CLI dispatch.  Option parsing is deliberately strict: malformed
 * selections must never silently become the default partition. */
#include "hu68disk.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"

#define HU68DISK_VERSION "1.0"

static void usage(void) {
    fprintf(stderr,
            "hu68disk " HU68DISK_VERSION "\nusage:\n"
            "  hu68disk partitions <image> [--all]\n"
            "  hu68disk list <image> [--part N] [path]\n"
            "  hu68disk copy|del|extract|mkdir|rmdir <image> [--part N] ...\n"
            "  hu68disk copyall <src-image> <src-path> <dst-image> <dst-path> [--src-part N] "
            "[--dst-part N]\n"
            "  hu68disk mkfs <image> [--type 2hd|scsi] [--size MiB] [--label LABEL]\n"
            "                       [--volume MiB[:LABEL]]... [--removable] [--no-partition]\n"
            "                       [--force]\n"
            "  hu68disk fsck <image> [--part N] [--fix] [--verbose] [--yes] [--no-backup]\n");
}
static int number(const char *s, int lo, int hi, int *out) {
    char *end;
    long v;
    if (!s || !*s)
        return -1;
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno || *end || v < lo || v > hi)
        return -1;
    *out = (int)v;
    return 0;
}
static int same_file(const char *a, const char *b) { return hu_same_file(a, b); }
static int finish(image_t *im, int rc) {
    if (im->range_error) {
        fprintf(stderr, "error: パーティション境界違反を検出しました\n");
        return 1;
    }
    return rc ? 1 : 0;
}

int main(int argc, char **argv) {
    hu_init_args(&argc, &argv);
    if (argc == 2 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-V"))) {
        printf("hu68disk %s\n", HU68DISK_VERSION);
        return 0;
    }
    if (argc < 3) {
        usage();
        return 2;
    }
    const char *cmd = argv[1];
    if (!strcmp(cmd, "mkfs") || !strcmp(cmd, "format")) {
        const char *type = NULL, *label = NULL;
        int force = 0, removable = 0, no_partition = 0, size = 0, nvol = 0;
        scsi_partition_spec_t vols[SCSI_PART_TABLE_MAX];
        for (int i = 3, end = 0; i < argc; i++) {
            const char *a = argv[i];
            if (!end && !strcmp(a, "--")) {
                end = 1;
                continue;
            }
            if (!end && !strcmp(a, "--type")) {
                if (++i == argc || type)
                    goto bad;
                type = argv[i];
            } else if (!end && (!strcmp(a, "--force") || !strcmp(a, "-f"))) {
                if (force)
                    goto bad;
                force = 1;
            } else if (!end && !strcmp(a, "--size")) {
                if (++i == argc || size || number(argv[i], 1, INT_MAX, &size))
                    goto bad;
            } else if (!end && !strcmp(a, "--label")) {
                if (++i == argc || label)
                    goto bad;
                label = argv[i];
            } else if (!end && !strcmp(a, "--removable")) {
                if (removable)
                    goto bad;
                removable = 1;
            } else if (!end && !strcmp(a, "--no-partition")) {
                if (no_partition)
                    goto bad;
                no_partition = 1;
            } else if (!end && !strcmp(a, "--volume")) {
                char *colon, *copy;
                int mb;
                if (++i == argc || nvol == SCSI_PART_TABLE_MAX)
                    goto bad;
                copy = malloc(strlen(argv[i]) + 1);
                if (!copy)
                    return 1;
                strcpy(copy, argv[i]);
                colon = strchr(copy, ':');
                if (colon)
                    *colon++ = 0;
                if (number(copy, 1, INT_MAX, &mb)) {
                    free(copy);
                    goto bad;
                }
                vols[nvol].size_mb = (uint32_t)mb;
                vols[nvol].label = colon && *colon ? argv[i] + (colon - copy) : NULL;
                free(copy);
                nvol++;
            } else
                goto bad;
        }
        return cmd_mkfs(argv[2], type, force, size, label, removable, no_partition, vols, nvol)
                   ? 1
                   : 0;
    }
    if (!strcmp(cmd, "copyall")) {
        const char *pos[4];
        int np = 0, sp = -1, dp = -1;
        for (int i = 2, end = 0; i < argc; i++) {
            if (!end && !strcmp(argv[i], "--")) {
                end = 1;
                continue;
            }
            if (!end && !strcmp(argv[i], "--src-part")) {
                if (++i == argc || sp != -1 || number(argv[i], 1, 15, &sp))
                    goto bad;
            } else if (!end && !strcmp(argv[i], "--dst-part")) {
                if (++i == argc || dp != -1 || number(argv[i], 1, 15, &dp))
                    goto bad;
            } else if (!end && !strcmp(argv[i], "--part"))
                goto bad;
            else if (!end && argv[i][0] == '-')
                goto bad;
            else if (np < 4)
                pos[np++] = argv[i];
            else
                goto bad;
        }
        if (np != 4)
            goto bad;
        image_t src, dst;
        if (image_open(&src, pos[0]) || image_select_partition(&src, sp)) {
            image_free(&src);
            return 1;
        }
        if (image_open(&dst, pos[2]) || image_select_partition(&dst, dp)) {
            image_free(&src);
            image_free(&dst);
            return 1;
        }
        if (same_file(pos[0], pos[2]) && src.selected_part == dst.selected_part) {
            fprintf(stderr, "error: 同一イメージの同一区画への copyall は拒否します\n");
            image_free(&src);
            image_free(&dst);
            return 1;
        }
        int rc = cmd_copyall(&src, pos[1], &dst, pos[3]);
        rc = finish(&src, rc) || finish(&dst, rc);
        image_free(&src);
        image_free(&dst);
        return rc;
    }

    const char *image = argv[2], *pos[3];
    int np = 0, part = -1, all = 0, fix = 0, verbose = 0, yes = 0, nobak = 0;
    for (int i = 3, end = 0; i < argc; i++) {
        const char *a = argv[i];
        if (!end && !strcmp(a, "--")) {
            end = 1;
            continue;
        }
        if (!end && !strcmp(a, "--part")) {
            if (++i == argc || part != -1 || number(argv[i], 1, 15, &part))
                goto bad;
        } else if (!end && !strcmp(a, "--all") && !strcmp(cmd, "partitions")) {
            if (all)
                goto bad;
            all = 1;
        } else if (!end && !strcmp(a, "--fix") &&
                   (!strcmp(cmd, "fsck") || !strcmp(cmd, "check"))) {
            if (fix)
                goto bad;
            fix = 1;
        } else if (!end && (!strcmp(a, "--verbose") || !strcmp(a, "-v")) &&
                   (!strcmp(cmd, "fsck") || !strcmp(cmd, "check"))) {
            if (verbose)
                goto bad;
            verbose = 1;
        } else if (!end && (!strcmp(a, "--yes") || !strcmp(a, "-y")) &&
                   (!strcmp(cmd, "fsck") || !strcmp(cmd, "check"))) {
            if (yes)
                goto bad;
            yes = 1;
        } else if (!end && !strcmp(a, "--no-backup") &&
                   (!strcmp(cmd, "fsck") || !strcmp(cmd, "check"))) {
            if (nobak)
                goto bad;
            nobak = 1;
        } else if (!end && a[0] == '-')
            goto bad;
        else if (np < 3)
            pos[np++] = a;
        else
            goto bad;
    }
    image_t im;
    if (image_open(&im, image))
        return 1;
    if (!strcmp(cmd, "partitions")) {
        int rc = part != -1 || np ? 2 : cmd_partitions(&im, all);
        if (rc == 2)
            usage();
        image_free(&im);
        return rc ? (rc == 2 ? 2 : 1) : 0;
    }
    if (image_select_partition(&im, part)) {
        image_free(&im);
        return 1;
    }
    int rc;
    if (!strcmp(cmd, "list"))
        rc = np <= 1 ? cmd_list(&im, np ? pos[0] : NULL) : 2;
    else if (!strcmp(cmd, "copy"))
        rc = np == 2 ? cmd_copy(&im, pos[0], pos[1]) : 2;
    else if (!strcmp(cmd, "del") || !strcmp(cmd, "rm"))
        rc = np == 1 ? cmd_del(&im, pos[0]) : 2;
    else if (!strcmp(cmd, "extract") || !strcmp(cmd, "get"))
        rc = np == 2 ? cmd_extract(&im, pos[0], pos[1]) : 2;
    else if (!strcmp(cmd, "mkdir"))
        rc = np == 1 ? cmd_mkdir(&im, pos[0]) : 2;
    else if (!strcmp(cmd, "rmdir"))
        rc = np == 1 ? cmd_rmdir(&im, pos[0]) : 2;
    else if (!strcmp(cmd, "fsck") || !strcmp(cmd, "check"))
        rc = np == 0 ? cmd_fsck(&im, fix, verbose, yes, nobak) : 2;
    else
        rc = 2;
    if (rc == 2)
        usage();
    else
        rc = finish(&im, rc);
    image_free(&im);
    return rc == 2 ? 2 : rc;
bad:
    usage();
    return 2;
}
