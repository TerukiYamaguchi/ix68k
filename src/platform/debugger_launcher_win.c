/*
 * Windows版の ix68k-debugger 起動（debugger_launcher.h の実装）。
 * POSIX版（debugger_launcher.c）と同じ契約で、起動スクリプトは
 * tools-host\debugger\launch_debugger.cmd を cmd.exe 経由で実行する。
 * コンソール窓を出さないよう CREATE_NO_WINDOW で起動するため、スクリプトの
 * エラーメッセージ（tkinter のある Python が無い等）は見えない。
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stddef.h>
#include <wchar.h>

#include "platform/debugger_launcher.h"
#include "platform/platform.h"

#define LAUNCHER_ENV          L"IX68K_DEBUGGER_LAUNCHER"
#define LAUNCHER_ENV_UTF8     "IX68K_DEBUGGER_LAUNCHER"
#define LAUNCHER_DEFAULT_PATH L"tools-host\\debugger\\launch_debugger.cmd"
#define PATH_CAPACITY         MAX_PATH
/* cmd.exe /d /c ""<path>"" の固定部分と終端のぶんの余裕。 */
#define COMMAND_LINE_CAPACITY (PATH_CAPACITY + 32)

static HANDLE s_child = NULL;

/* 環境変数があればそれ、無ければ既定の相対パス（カレントディレクトリ基準）を
 * bufへ書いて返す。 */
static const wchar_t *launcher_path(wchar_t *buf, DWORD capacity) {
    DWORD length = GetEnvironmentVariableW(LAUNCHER_ENV, buf, capacity);
    if (length > 0 && length < capacity)
        return buf;
    return LAUNCHER_DEFAULT_PATH;
}

static int file_is_readable(const wchar_t *path) {
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

int pal_debugger_available(void) {
    wchar_t buf[PATH_CAPACITY];
    return file_is_readable(launcher_path(buf, PATH_CAPACITY));
}

/* 前回起動した子がまだ動いていれば1。終了済みならハンドルを閉じて0。 */
static int child_is_running(void) {
    if (!s_child)
        return 0;
    if (WaitForSingleObject(s_child, 0) == WAIT_TIMEOUT)
        return 1;
    CloseHandle(s_child);
    s_child = NULL;
    return 0;
}

pal_debugger_launch_result_t pal_debugger_launch(void) {
    if (child_is_running()) {
        pal_log("ix68k-debugger は既に起動しています\n");
        return PAL_DEBUGGER_ALREADY_RUNNING;
    }

    wchar_t buf[PATH_CAPACITY];
    const wchar_t *path = launcher_path(buf, PATH_CAPACITY);
    if (!file_is_readable(path)) {
        pal_log("ix68k-debugger の起動スクリプトが見つかりません"
                "（リポジトリルートで起動するか、%s を指定してください）\n",
                LAUNCHER_ENV_UTF8);
        return PAL_DEBUGGER_UNAVAILABLE;
    }

    /* cmd.exe /c は、コマンド全体を引用符で囲むとその内側の引用符を保つ。 */
    wchar_t command_line[COMMAND_LINE_CAPACITY];
    int written =
        _snwprintf(command_line, COMMAND_LINE_CAPACITY, L"cmd.exe /d /c \"\"%ls\"\"", path);
    if (written < 0 || written >= COMMAND_LINE_CAPACITY) {
        pal_log("ix68k-debugger を起動できません（起動スクリプトのパスが長すぎます）\n");
        return PAL_DEBUGGER_FAILED;
    }

    STARTUPINFOW startup;
    ZeroMemory(&startup, sizeof(startup));
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process;
    ZeroMemory(&process, sizeof(process));
    if (!CreateProcessW(NULL, command_line, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL,
                        &startup, &process)) {
        pal_log("ix68k-debugger を起動できません（CreateProcess: %lu）\n",
                (unsigned long)GetLastError());
        return PAL_DEBUGGER_FAILED;
    }
    CloseHandle(process.hThread);
    s_child = process.hProcess;
    pal_log("ix68k-debugger を起動しました（pid %lu）\n", (unsigned long)process.dwProcessId);
    return PAL_DEBUGGER_LAUNCHED;
}
