/* compat.c - OS 差の吸収（compat.h 参照） */
#ifndef _WIN32
#define _POSIX_C_SOURCE   200809L
#define _FILE_OFFSET_BITS 64
#endif
#include "compat.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <sys/utime.h>

/* UTF-8 → UTF-16。呼び出し側が free する。失敗は NULL。 */
static wchar_t *to_wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0)
        return NULL;
    wchar_t *w = malloc((size_t)n * sizeof(wchar_t));
    if (w && MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) {
        free(w);
        return NULL;
    }
    return w;
}

FILE *hu_fopen(const char *path, const char *mode) {
    wchar_t *wp = to_wide(path), *wm = to_wide(mode);
    FILE *f = NULL;
    if (wp && wm)
        f = _wfopen(wp, wm);
    free(wp);
    free(wm);
    return f;
}

int hu_set_mtime(const char *path, time_t t) {
    wchar_t *wp = to_wide(path);
    struct _utimbuf tb = {t, t};
    int rc = wp ? _wutime(wp, &tb) : -1;
    free(wp);
    return rc;
}

int hu_get_mtime(const char *path, time_t *t) {
    wchar_t *wp = to_wide(path);
    struct _stat64i32 st;
    int rc = wp ? _wstat64i32(wp, &st) : -1;
    free(wp);
    if (rc == 0)
        *t = st.st_mtime;
    return rc;
}

static int file_id(const char *path, BY_HANDLE_FILE_INFORMATION *info) {
    wchar_t *wp = to_wide(path);
    if (!wp)
        return -1;
    HANDLE h = CreateFileW(wp, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(wp);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    BOOL ok = GetFileInformationByHandle(h, info);
    CloseHandle(h);
    return ok ? 0 : -1;
}

int hu_same_file(const char *a, const char *b) {
    BY_HANDLE_FILE_INFORMATION ia, ib;
    if (file_id(a, &ia) == 0 && file_id(b, &ib) == 0)
        return ia.dwVolumeSerialNumber == ib.dwVolumeSerialNumber &&
               ia.nFileIndexHigh == ib.nFileIndexHigh && ia.nFileIndexLow == ib.nFileIndexLow;
    return strcmp(a, b) == 0;
}

static char *to_utf8(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0)
        return NULL;
    char *s = malloc((size_t)n);
    if (s && WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL) <= 0) {
        free(s);
        return NULL;
    }
    return s;
}

void hu_init_args(int *argc, char ***argv) {
    int wargc = 0;
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    SetConsoleOutputCP(CP_UTF8);
    if (!wargv)
        return;
    char **nv = calloc((size_t)wargc + 1, sizeof(char *));
    if (!nv) {
        LocalFree(wargv);
        return;
    }
    for (int i = 0; i < wargc; i++) {
        nv[i] = to_utf8(wargv[i]);
        if (!nv[i]) { /* 変換失敗: ANSI の argv へ戻す */
            LocalFree(wargv);
            return;
        }
    }
    LocalFree(wargv);
    *argc = wargc;
    *argv = nv;
}
#else
#include <utime.h>

FILE *hu_fopen(const char *path, const char *mode) { return fopen(path, mode); }

int hu_set_mtime(const char *path, time_t t) {
    struct utimbuf tb = {t, t};
    return utime(path, &tb);
}

int hu_get_mtime(const char *path, time_t *t) {
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    *t = st.st_mtime;
    return 0;
}

int hu_same_file(const char *a, const char *b) {
    struct stat sa, sb;
    if (stat(a, &sa) == 0 && stat(b, &sb) == 0)
        return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
    return strcmp(a, b) == 0;
}

void hu_init_args(int *argc, char ***argv) {
    (void)argc;
    (void)argv;
}
#endif
