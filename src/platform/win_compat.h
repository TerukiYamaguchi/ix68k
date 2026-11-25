#pragma once
/*
 * win_compat.h — MSVC/Windowsで欠けているPOSIX機能の最小互換層。
 * _WIN32 以外では何も定義しない。POSIX側の実装（main.c/x68k_bus.c/hostfile.c/
 * hostdrv_remote.c）と挙動を揃えるためのシムであり、汎用ライブラリ化は狙わない
 * （実際にこのリポジトリが使う関数だけをカバーする）。
 */
#ifdef _WIN32

#include <direct.h> /* _mkdir/_rmdir */
#include <fcntl.h>  /* _O_* */
#include <io.h>     /* _open/_close/_read/_write/_access/_unlink/_chsize_s */
#include <stdint.h>
#include <stdio.h> /* _fdopen */
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h> /* struct timespec（clang-clは配列引数で完全型を要求する） */

/* 単純な名前置換（すべてファイルディスクリプタ/ファイルパス操作。ソケットの
 * close()はsrc/mcp/server.c側で別途closesocket()を使うため衝突しない）。
 * パスを取る関数（unlink/open/fopen/rmdir/access/mkdir/chmod）はUTF-8→UTF-16
 * 変換を挟むwin_compat_*ラッパー経由にする（MSVCのANSI版CランタイムAPIは
 * char*をシステムのANSIコードページとして解釈するため、/utf-8でUTF-8バイト列のまま
 * 埋め込まれる日本語等のファイル名を渡すと文字化けし失敗する。_w系wide APIはANSI
 * コードページに依存しないため、日本語以外のロケールでも正しく動く）。
 * stat()だけは`struct stat`という型名としても使われており、`stat`一語の
 * オブジェクト形式マクロは型と関数呼び出しの両方を単純に置換してしまうため、
 * 関数呼び出し側だけをwin_compat_stat()にリネームする対応にしてある（呼び出し側:
 * hostdrv_remote.c、POSIX側では`#define win_compat_stat stat`で吸収する）。
 * fstat()はfd起点でパスの文字コード問題が無いため_fstatのまま。 */
#define strcasecmp        _stricmp
#define strncasecmp       _strnicmp
#define unlink            win_compat_unlink
#define fdopen            _fdopen
#define open              win_compat_open
#define fopen             win_compat_fopen
#define close             _close
/* read/writeは関数形式マクロ: 構造体メンバー名 read/write（mc68k_bus_access_t等）を置換しない。
 */
#define read(fd, buf, n)  _read(fd, buf, n)
#define write(fd, buf, n) _write(fd, buf, n)
#define fstat             _fstat
#define stat              _stat
#define rmdir             win_compat_rmdir
#define access            win_compat_access
#define chmod             win_compat_chmod
#define isatty            _isatty
#define STDIN_FILENO      0
#define STDERR_FILENO     2

int win_compat_open(const char *path, int flags, ...);
/* HOSTDRV $4A _OPEN専用: CreateFileWでFILE_WRITE_ATTRIBUTESを常時付与して開き、
 * _open_osfhandle()でCRT fdへ変換する。通常のwin_compat_open()
 * （_wopen経由）が作るHANDLEはO_RDONLYで開いた場合GENERIC_READのみになり、
 * 後続の$4F _FILEDATE（日時設定）が呼ぶ_futime64→SetFileTime()がFILE_WRITE_ATTRIBUTES
 * 権限不足のERROR_ACCESS_DENIEDで失敗する。ファイル内容への書き込み可否は
 * GENERIC_WRITEの有無だけで決まるため、FILE_WRITE_ATTRIBUTESを追加してもHuman68k側が
 * 期待する「読み取り専用オープン＝内容を書けない」という意味は変わらない。
 * flagsはopen()と同じO_RDONLY/O_WRONLY/O_RDWR|O_BINARYの組み合わせ。 */
int win_compat_open_hostdrv(const char *path, int flags);
FILE *win_compat_fopen(const char *path, const char *mode);
int win_compat_unlink(const char *path);
int win_compat_rmdir(const char *path);
int win_compat_access(const char *path, int mode);
/* struct stat引数は上のstat→_statマクロにより`struct _stat`へ展開される。 */
int win_compat_stat(const char *path, struct stat *buf);

#ifndef W_OK
#define W_OK 2
#endif

/* open()フラグ: MSVCの<fcntl.h>は_O_*が正式名（O_*が既に定義済みの場合も多いが、
 * 未定義環境向けに明示しておく）。_O_BINARYが無いとWindowsのopen()既定は
 * テキストモード(CRLF変換)になり、HOSTDRVが扱う任意バイナリを破壊するため必須。 */
#ifndef O_CREAT
#define O_CREAT _O_CREAT
#endif
#ifndef O_RDWR
#define O_RDWR _O_RDWR
#endif
#ifndef O_EXCL
#define O_EXCL _O_EXCL
#endif
#ifndef O_TRUNC
#define O_TRUNC _O_TRUNC
#endif
#ifndef O_WRONLY
#define O_WRONLY _O_WRONLY
#endif
#ifndef O_RDONLY
#define O_RDONLY _O_RDONLY
#endif
#ifndef O_BINARY
#define O_BINARY _O_BINARY
#endif

/* MSVCにはmode_tが無い（chmod()の第2引数用、権限ビットの意味はPOSIXと同じ
 * 0400=読出/0200=書込のみ_chmod()が解釈する）。 */
typedef int mode_t;

int win_compat_chmod(const char *path, mode_t mode);

/* S_ISDIR: MSVCの<sys/stat.h>は_S_IFDIR/_S_IFMTは提供するがマクロ自体が無い。 */
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif

/* S_IWUSR: MSVCの_stat()はread-only属性を_S_IWRITEの有無に反映する
 * （hostdrv_attr_for_stat()のREADONLY判定用）。 */
#ifndef S_IWUSR
#define S_IWUSR _S_IWRITE
#endif

/* EDQUOT: MSVCのerrno.hに無い（Windowsのローカルファイルシステムでは発生しない
 * エラーだが、switch-caseの分岐先として値だけ要る）。他のerrno値と衝突しない
 * 適当な大きい値を割り当てる。 */
#ifndef EDQUOT
#define EDQUOT 10001
#endif

/* ssize_t: MSVCに無い。recv/send等の戻り値・ループカウンタ用
 * （server.c/hostdrv_remote.c共通）。 */
#if !defined(_SSIZE_T_DEFINED)
typedef long ssize_t;
#define _SSIZE_T_DEFINED
#endif

/* localtime_r: POSIX版は引数順が(time_t*, tm*)、MSVCのlocaltime_sは(tm*, time_t*)。 */
struct tm *localtime_r(const time_t *timep, struct tm *result);

/* pread/pwrite: fdの現在位置を変えずに指定オフセットで読み書きする。
 * HOSTDRV側は毎回FCBが持つ絶対オフセットを渡すため、fd位置を呼び出し前の値に
 * 戻す必要はない（単純にlseek+read/writeで足りる）。 */
ssize_t pread(int fd, void *buf, size_t count, long offset);
ssize_t pwrite(int fd, const void *buf, size_t count, long offset);

/* futimens(fd, times): Win相当は_futime64(fd, &utimbuf)。times[0]=atime,
 * times[1]=mtimeのtv_secのみ使う（HOSTDRV側はnsec精度を要求しない）。 */
struct timespec;
int futimens(int fd, const struct timespec times[2]);

/* clock_gettime/CLOCK_MONOTONIC: MSVCには無い。platform_null.c（PLATFORM_NULL、
 * ヘッドレスビルド）が時刻取得に使う。QueryPerformanceCounterベースで実装
 * （CLOCK_MONOTONIC以外のclk_idはこのリポジトリでは使わない）。 */
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
int clock_gettime(int clk_id, struct timespec *ts);

/* nanosleep: MSVCには無い。Sleep()ベース（ミリ秒精度）。ヘッドレス用途
 * （CI/MCP経由の自動操作）ではミリ秒未満の精度は不要なため十分。 */
int nanosleep(const struct timespec *req, struct timespec *rem);

/* setenv: MSVCの_putenv_s(name, value)相当（overwrite=0時は既存値を尊重）。
 * テスト側(tests/test_nmi_pause.c等)がSDL_VIDEODRIVER設定に使う。 */
int setenv(const char *name, const char *value, int overwrite);

/* mkdir(path, mode): POSIX2引数 → Win _wmkdir(path)は1引数（mode概念が無い）。
 * modeは無視する（呼び出し元はディレクトリのパーミッションを問わない用途のみ）。 */
int win_compat_mkdir(const char *path, mode_t mode);
#define mkdir(path, mode) win_compat_mkdir(path, mode)

/* ftruncate(fd, len): Win相当は_chsize_s()（64bit長対応、errno_tを返す）。 */
int ftruncate(int fd, long length);

/* dirent.h 最小シム（d_nameのみ実装。hostdrv_remote.cはd_type不使用）。
 * 内部はFindFirstFileW/FindNextFileWでUTF-16列挙し、d_nameはUTF-8へ変換して返す
 * （非ASCIIファイル名対応）。UTF-16のMAX_PATH文字はUTF-8では最大3倍の
 * バイト数になりうるため、バッファはMAX_PATHの3倍取る。 */
typedef struct DIR DIR;
struct dirent {
    char d_name[260 * 3];
};
DIR *opendir(const char *path);
struct dirent *readdir(DIR *dirp);
int closedir(DIR *dirp);

/* mkstemp(): templateの末尾6文字("XXXXXX")を一意な文字列に置き換えてO_CREAT|O_EXCLで
 * 開く。呼び出し元は/tmpが無いWindowsでは win_get_temp_template() で実行時に
 * ベースパスを組み立ててから渡すこと（HF_PUT_TMP_TEMPLATEのコンパイル時定数は
 * POSIX版のみで使う）。 */
int mkstemp(char *tmpl);

/* mkdtemp(): mkstempのディレクトリ版。成功時tmplへのポインタ、失敗時NULL。 */
char *mkdtemp(char *tmpl);

/* HOSTFILE一時ファイル用: "<GetTempPathW結果のUTF-8変換><prefix>XXXXXX" をbufへ書く
 * （mkstempにそのまま渡せる形。ユーザー名等に非ASCII文字を含むTEMPパスでも
 * 正しく取得できるようGetTempPathWを使う）。 */
void win_get_temp_template(char *buf, size_t bufsz, const char *prefix);

/* 固定ファイル名版: "<GetTempPathW結果のUTF-8変換><filename>" をbufへ書く。
 * テストコードの"/tmp/foo"直書き（Windowsにはそのパスが存在しない）の置換用。 */
void win_get_temp_path(char *buf, size_t bufsz, const char *filename);

/* $50 _DSKFRE用: statvfs相当の値をGetDiskFreeSpaceExW一発で取る。
 * 成功時0、失敗時-1（out_*は0のまま＝呼び出し元は既存のstatvfs失敗時と同じ
 * 「空き0扱い」にフォールバックする）。 */
int win_get_disk_free(const char *path, uint64_t *out_free_bytes, uint64_t *out_total_bytes);

#endif /* _WIN32 */
