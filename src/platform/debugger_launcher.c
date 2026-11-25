#include "platform/debugger_launcher.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "platform/platform.h"

#define LAUNCHER_ENV          "IX68K_DEBUGGER_LAUNCHER"
#define LAUNCHER_DEFAULT_PATH "tools-host/debugger/launch_debugger.sh"
#define LAUNCHER_SHELL        "/bin/sh"
#define NULL_DEVICE           "/dev/null"
#define CHILD_EXEC_FAILED     127

static pid_t s_child = 0;

static const char *launcher_path(void) {
    const char *override = getenv(LAUNCHER_ENV);
    return (override && override[0]) ? override : LAUNCHER_DEFAULT_PATH;
}

int pal_debugger_available(void) {
    return access(launcher_path(), R_OK) == 0;
}

/* 前回起動した子がまだ動いていれば1。終了済みなら回収して0。 */
static int child_is_running(void) {
    if (s_child <= 0)
        return 0;
    int status = 0;
    pid_t reaped = waitpid(s_child, &status, WNOHANG);
    if (reaped == 0)
        return 1;
    s_child = 0; /* 回収できた、または既に無い（ECHILD）。どちらも「動いていない」 */
    return 0;
}

pal_debugger_launch_result_t pal_debugger_launch(void) {
    if (child_is_running()) {
        pal_log("ix68k-debugger は既に起動しています\n");
        return PAL_DEBUGGER_ALREADY_RUNNING;
    }

    const char *path = launcher_path();
    if (access(path, R_OK) != 0) {
        pal_log("ix68k-debugger の起動スクリプトが見つかりません: "
                "%s（リポジトリルートで起動するか、%s を指定してください）\n",
                path, LAUNCHER_ENV);
        return PAL_DEBUGGER_UNAVAILABLE;
    }

    pid_t pid = fork();
    if (pid < 0) {
        pal_log("ix68k-debugger を起動できません（fork: %s）\n", strerror(errno));
        return PAL_DEBUGGER_FAILED;
    }
    if (pid == 0) {
        /* 子プロセス。ix68k本体のスレッド（SDL等）を引き継いでいるため、ここでは
         * async-signal-safeな関数（setsid/open/dup2/execv/_exit）だけを呼ぶ。
         * stdinは切り離し、stdout/stderrは起動元のターミナルへ残す（スクリプトの
         * エラーメッセージを読めるように）。 */
        setsid();
        int null_fd = open(NULL_DEVICE, O_RDONLY);
        if (null_fd >= 0) {
            dup2(null_fd, STDIN_FILENO);
            if (null_fd != STDIN_FILENO)
                close(null_fd);
        }
        char *const argv[] = {(char *)LAUNCHER_SHELL, (char *)path, NULL};
        execv(LAUNCHER_SHELL, argv);
        _exit(CHILD_EXEC_FAILED);
    }
    s_child = pid;
    pal_log("ix68k-debugger を起動しました（pid %d）\n", (int)pid);
    return PAL_DEBUGGER_LAUNCHED;
}
