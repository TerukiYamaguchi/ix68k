#ifdef _WIN32
#include "win_compat.h"

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utime.h>
#include <time.h>
#include <windows.h>

/* ---- UTF-8 <-> UTF-16 変換 ----
 * MSVCのANSI版CランタイムAPI（_open/_stat等）はchar*をシステムのANSI
 * コードページとして解釈するが、CMakeLists.txtの/utf-8オプションによりソース中の
 * 文字列リテラルはUTF-8バイト列のまま埋め込まれる。両者は同じ文字でも異なる
 * バイト表現のため、日本語等の非ASCIIパスをANSI版APIへそのまま渡すと解釈が
 * 破綻する。ANSIコードページに依存しない_w系wide APIへ経由させるための変換。 */
#define WIN_COMPAT_WPATH_MAX 1024

static int utf8_to_wide(const char *utf8, wchar_t *out, int out_wchars) {
    if (!utf8) {
        errno = EINVAL;
        return -1;
    }
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, out, out_wchars);
    if (n == 0) {
        errno = (GetLastError() == ERROR_INSUFFICIENT_BUFFER) ? ENAMETOOLONG : EINVAL;
        return -1;
    }
    return 0;
}

static int wide_to_utf8(const wchar_t *wide, char *out, int out_bytes) {
    int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, out_bytes, NULL, NULL);
    if (n == 0) {
        errno = (GetLastError() == ERROR_INSUFFICIENT_BUFFER) ? ENAMETOOLONG : EINVAL;
        return -1;
    }
    return 0;
}

/* ---- パスを取るファイルI/O関数のwideラッパー ---- */

int win_compat_open(const char *path, int flags, ...) {
    wchar_t wpath[WIN_COMPAT_WPATH_MAX];
    if (utf8_to_wide(path, wpath, WIN_COMPAT_WPATH_MAX) != 0)
        return -1;
    int mode = 0;
    if (flags & _O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    return _wopen(wpath, flags, mode);
}

int win_compat_open_hostdrv(const char *path, int flags) {
    wchar_t wpath[WIN_COMPAT_WPATH_MAX];
    if (utf8_to_wide(path, wpath, WIN_COMPAT_WPATH_MAX) != 0)
        return -1;

    int accmode = flags & (_O_RDONLY | _O_WRONLY | _O_RDWR);
    DWORD access = FILE_WRITE_ATTRIBUTES;
    if (accmode == _O_WRONLY)
        access |= GENERIC_WRITE;
    else if (accmode == _O_RDWR)
        access |= GENERIC_READ | GENERIC_WRITE;
    else
        access |= GENERIC_READ;

    HANDLE h = CreateFileW(wpath, access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        errno = (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) ? ENOENT : EACCES;
        return -1;
    }
    int osf_flags = _O_BINARY | (accmode == _O_RDONLY ? _O_RDONLY : 0);
    int fd = _open_osfhandle((intptr_t)h, osf_flags);
    if (fd < 0) {
        CloseHandle(h);
        return -1;
    }
    return fd;
}

FILE *win_compat_fopen(const char *path, const char *mode) {
    wchar_t wpath[WIN_COMPAT_WPATH_MAX], wmode[16];
    if (utf8_to_wide(path, wpath, WIN_COMPAT_WPATH_MAX) != 0)
        return NULL;
    if (utf8_to_wide(mode, wmode, sizeof(wmode) / sizeof(wmode[0])) != 0)
        return NULL;
    return _wfopen(wpath, wmode);
}

int win_compat_stat(const char *path, struct stat *buf) {
    wchar_t wpath[WIN_COMPAT_WPATH_MAX];
    if (utf8_to_wide(path, wpath, WIN_COMPAT_WPATH_MAX) != 0)
        return -1;
    return _wstat(wpath, buf);
}

int win_compat_unlink(const char *path) {
    wchar_t wpath[WIN_COMPAT_WPATH_MAX];
    if (utf8_to_wide(path, wpath, WIN_COMPAT_WPATH_MAX) != 0)
        return -1;
    return _wunlink(wpath);
}

int win_compat_rmdir(const char *path) {
    wchar_t wpath[WIN_COMPAT_WPATH_MAX];
    if (utf8_to_wide(path, wpath, WIN_COMPAT_WPATH_MAX) != 0)
        return -1;
    return _wrmdir(wpath);
}

int win_compat_access(const char *path, int mode) {
    wchar_t wpath[WIN_COMPAT_WPATH_MAX];
    if (utf8_to_wide(path, wpath, WIN_COMPAT_WPATH_MAX) != 0)
        return -1;
    return _waccess(wpath, mode);
}

int win_compat_chmod(const char *path, mode_t mode) {
    wchar_t wpath[WIN_COMPAT_WPATH_MAX];
    if (utf8_to_wide(path, wpath, WIN_COMPAT_WPATH_MAX) != 0)
        return -1;
    return _wchmod(wpath, mode);
}

int win_compat_mkdir(const char *path, mode_t mode) {
    (void)mode;
    wchar_t wpath[WIN_COMPAT_WPATH_MAX];
    if (utf8_to_wide(path, wpath, WIN_COMPAT_WPATH_MAX) != 0)
        return -1;
    return _wmkdir(wpath);
}

/* ---- dirent.h 最小シム ---- */

struct DIR {
    HANDLE h;
    WIN32_FIND_DATAW fd;
    int pending; /* FindFirstFileWで既に取得済みの1件目が未消費 */
    struct dirent entry;
};

DIR *opendir(const char *path) {
    wchar_t wpath[WIN_COMPAT_WPATH_MAX - 2], wpattern[WIN_COMPAT_WPATH_MAX];
    if (utf8_to_wide(path, wpath, sizeof(wpath) / sizeof(wpath[0])) != 0)
        return NULL;
    _snwprintf(wpattern, sizeof(wpattern) / sizeof(wpattern[0]), L"%s\\*", wpath);
    wpattern[sizeof(wpattern) / sizeof(wpattern[0]) - 1] = L'\0';

    DIR *d = malloc(sizeof(DIR));
    if (!d)
        return NULL;
    d->h = FindFirstFileW(wpattern, &d->fd);
    if (d->h == INVALID_HANDLE_VALUE) {
        free(d);
        return NULL;
    }
    d->pending = 1;
    return d;
}

struct dirent *readdir(DIR *dirp) {
    if (!dirp->pending) {
        if (!FindNextFileW(dirp->h, &dirp->fd))
            return NULL;
    }
    dirp->pending = 0;
    if (wide_to_utf8(dirp->fd.cFileName, dirp->entry.d_name, sizeof(dirp->entry.d_name)) != 0)
        dirp->entry.d_name[0] =
            '\0'; /* 変換失敗時は空文字列（name_to_pattern()側で除外される） */
    return &dirp->entry;
}

int closedir(DIR *dirp) {
    if (!dirp)
        return -1;
    FindClose(dirp->h);
    free(dirp);
    return 0;
}

/* ---- ftruncate ---- */

int ftruncate(int fd, long length) {
    return _chsize_s(fd, (__int64)length) == 0 ? 0 : -1;
}

/* ---- mkstemp ---- */

int mkstemp(char *tmpl) {
    size_t len = strlen(tmpl);
    static const char charset[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    if (len < 6 || strcmp(tmpl + len - 6, "XXXXXX") != 0)
        return -1;
    for (int attempt = 0; attempt < 100; attempt++) {
        for (int i = 0; i < 6; i++)
            tmpl[len - 6 + i] = charset[rand() % (sizeof(charset) - 1)];
        int fd = win_compat_open(tmpl, _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY,
                                 _S_IREAD | _S_IWRITE);
        if (fd >= 0)
            return fd;
    }
    return -1;
}

char *mkdtemp(char *tmpl) {
    size_t len = strlen(tmpl);
    static const char charset[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    if (len < 6 || strcmp(tmpl + len - 6, "XXXXXX") != 0)
        return NULL;
    for (int attempt = 0; attempt < 100; attempt++) {
        for (int i = 0; i < 6; i++)
            tmpl[len - 6 + i] = charset[rand() % (sizeof(charset) - 1)];
        if (win_compat_mkdir(tmpl, 0700) == 0)
            return tmpl;
    }
    return NULL;
}

/* ---- 一時ディレクトリ/空き容量 ---- */

/* Windowsのユーザー名等、%TEMP%の構成要素は非ASCII文字を含みうるため、
 * GetTempPathA（ANSIコードページ）ではなくGetTempPathW→UTF-8変換で取得する。 */
static void win_get_temp_dir_utf8(char *out, size_t out_bytes) {
    wchar_t wtmp[MAX_PATH];
    DWORD n = GetTempPathW((DWORD)(sizeof(wtmp) / sizeof(wtmp[0])), wtmp);
    if (n == 0 || n >= sizeof(wtmp) / sizeof(wtmp[0]) ||
        wide_to_utf8(wtmp, out, (int)out_bytes) != 0)
        strcpy(out, ".\\");
}

void win_get_temp_template(char *buf, size_t bufsz, const char *prefix) {
    char tmp[MAX_PATH * 3];
    win_get_temp_dir_utf8(tmp, sizeof(tmp));
    _snprintf(buf, bufsz, "%s%sXXXXXX", tmp, prefix);
    buf[bufsz - 1] = '\0';
}

void win_get_temp_path(char *buf, size_t bufsz, const char *filename) {
    char tmp[MAX_PATH * 3];
    win_get_temp_dir_utf8(tmp, sizeof(tmp));
    _snprintf(buf, bufsz, "%s%s", tmp, filename);
    buf[bufsz - 1] = '\0';
}

int win_get_disk_free(const char *path, uint64_t *out_free_bytes, uint64_t *out_total_bytes) {
    wchar_t wpath[WIN_COMPAT_WPATH_MAX];
    if (utf8_to_wide(path, wpath, WIN_COMPAT_WPATH_MAX) != 0)
        return -1;
    ULARGE_INTEGER free_avail, total, free_total;
    if (!GetDiskFreeSpaceExW(wpath, &free_avail, &total, &free_total))
        return -1;
    *out_free_bytes = free_avail.QuadPart;
    *out_total_bytes = total.QuadPart;
    return 0;
}

/* ---- 時刻・位置指定I/O ---- */

struct tm *localtime_r(const time_t *timep, struct tm *result) {
    return localtime_s(result, timep) == 0 ? result : NULL;
}

ssize_t pread(int fd, void *buf, size_t count, long offset) {
    if (_lseeki64(fd, offset, SEEK_SET) < 0)
        return -1;
    return _read(fd, buf, (unsigned int)count);
}

ssize_t pwrite(int fd, const void *buf, size_t count, long offset) {
    if (_lseeki64(fd, offset, SEEK_SET) < 0)
        return -1;
    return _write(fd, buf, (unsigned int)count);
}

int futimens(int fd, const struct timespec times[2]) {
    struct __utimbuf64 ub;
    ub.actime = (__time64_t)times[0].tv_sec;
    ub.modtime = (__time64_t)times[1].tv_sec;
    return _futime64(fd, &ub) == 0 ? 0 : -1;
}

int setenv(const char *name, const char *value, int overwrite) {
    if (!overwrite) {
        char buf[4];
        size_t len = 0;
        if (getenv_s(&len, buf, sizeof(buf), name) == 0 && len > 0)
            return 0; /* 既存値を維持 */
    }
    return _putenv_s(name, value) == 0 ? 0 : -1;
}

int clock_gettime(int clk_id, struct timespec *ts) {
    (void)clk_id; /* CLOCK_MONOTONICのみ対応 */
    static LARGE_INTEGER freq;
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    ts->tv_sec = (time_t)(now.QuadPart / freq.QuadPart);
    ts->tv_nsec = (long)((now.QuadPart % freq.QuadPart) * 1000000000LL / freq.QuadPart);
    return 0;
}

int nanosleep(const struct timespec *req, struct timespec *rem) {
    (void)rem; /* 中断時の残り時間報告は非対応（ヘッドレス用途では不要） */
    DWORD ms = (DWORD)(req->tv_sec * 1000 + req->tv_nsec / 1000000);
    Sleep(ms);
    return 0;
}

#endif /* _WIN32 */
