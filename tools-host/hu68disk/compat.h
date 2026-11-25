/* compat.h - hu68disk の OS 差（POSIX / Windows）を吸収する薄い層
 *
 * Windows: ホストパス・argv は UTF-8 で扱い（日本語ファイル名対応）、_wfopen 等へ変換する。
 * POSIX:   標準関数をそのまま使う。 */
#ifndef HU68DISK_COMPAT_H
#define HU68DISK_COMPAT_H

#include <stdio.h>
#include <time.h>

#ifdef _WIN32
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
typedef int64_t hu_off_t;
#define hu_fseek      _fseeki64
#define hu_ftell      _ftelli64
#define strcasecmp    _stricmp
#define strncasecmp   _strnicmp
#else
#include <strings.h>
#include <sys/types.h>
typedef off_t hu_off_t;
#define hu_fseek      fseeko
#define hu_ftell      ftello
#endif

/* ホストパス(UTF-8)でファイルを開く。 */
FILE *hu_fopen(const char *path, const char *mode);
/* ファイルの更新日時を設定する。失敗は無視してよい用途（0=OK）。 */
int hu_set_mtime(const char *path, time_t t);
/* ファイルの更新日時を取得する。0=OK。 */
int hu_get_mtime(const char *path, time_t *t);
/* 2 つのパスが同一ファイルか。判定できなければパス文字列の一致で代用。 */
int hu_same_file(const char *a, const char *b);
/* 起動時に 1 回呼ぶ。Windows は argv を UTF-8 へ作り直し、コンソール出力を UTF-8 にする。 */
void hu_init_args(int *argc, char ***argv);

#endif
