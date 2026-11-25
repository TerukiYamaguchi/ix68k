/*
 * debugger_launcher（メニューからの ix68k-debugger 起動）の単体テスト。
 * 本物のデバッガは起動せず、IX68K_DEBUGGER_LAUNCHER でダミーのシェルスクリプトを
 * 差し込んで、起動・二重起動の抑止・終了後の再起動・スクリプト欠如を確認する。
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "platform/debugger_launcher.h"

#define LAUNCHER_ENV       "IX68K_DEBUGGER_LAUNCHER"
#define WAIT_STEP_NS       10000000L /* 10ms */
#define WAIT_MAX_STEPS     500       /* 最大5秒 */
#define MARKER_TEXT_LENGTH 16

/* platform.hの実体はリンクしないので、テストが使うpal_logだけ差し替える。 */
void pal_log(const char *fmt, ...) {
    (void)fmt;
}

static int g_failures = 0;

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

static void sleep_step(void) {
    struct timespec ts = {0, WAIT_STEP_NS};
    nanosleep(&ts, NULL);
}

static int count_lines(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp)
        return 0;
    int lines = 0;
    int c;
    while ((c = fgetc(fp)) != EOF) {
        if (c == '\n')
            lines++;
    }
    fclose(fp);
    return lines;
}

/* path が expected 行以上になるまで待つ。 */
static int wait_for_lines(const char *path, int expected) {
    for (int i = 0; i < WAIT_MAX_STEPS; i++) {
        if (count_lines(path) >= expected)
            return 1;
        sleep_step();
    }
    return 0;
}

/* path が存在するようになるまで待つ。 */
static int wait_for_file(const char *path) {
    for (int i = 0; i < WAIT_MAX_STEPS; i++) {
        if (access(path, F_OK) == 0)
            return 1;
        sleep_step();
    }
    return 0;
}

static int write_script(const char *path, const char *body) {
    FILE *fp = fopen(path, "w");
    if (!fp)
        return 0;
    fputs(body, fp);
    fclose(fp);
    return chmod(path, 0755) == 0;
}

int main(void) {
    char dir[] = "/tmp/ix68k_launcher_test_XXXXXX";
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 1;
    }
    char script[256], started[256], release[256], finished[256];
    snprintf(script, sizeof(script), "%s/launch.sh", dir);
    snprintf(started, sizeof(started), "%s/started", dir);
    snprintf(release, sizeof(release), "%s/release", dir);
    snprintf(finished, sizeof(finished), "%s/finished", dir);

    /* 起動の証拠を残し、release ファイルができるまで居続け、終了時に finished へ
     * 1行追記するダミー。 */
    char body[1024];
    snprintf(body, sizeof(body),
             "#!/bin/sh\n"
             "echo ok > '%s'\n"
             "while [ ! -e '%s' ]; do sleep 0.01; done\n"
             "echo done >> '%s'\n",
             started, release, finished);
    CHECK(write_script(script, body));

    /* 1. スクリプトが無ければ利用不可で、起動もしない。 */
    char missing[256];
    snprintf(missing, sizeof(missing), "%s/missing.sh", dir);
    setenv(LAUNCHER_ENV, missing, 1);
    CHECK(pal_debugger_available() == 0);
    CHECK(pal_debugger_launch() == PAL_DEBUGGER_UNAVAILABLE);

    /* 2. スクリプトがあれば利用可能で、起動できる。 */
    setenv(LAUNCHER_ENV, script, 1);
    CHECK(pal_debugger_available() == 1);
    CHECK(pal_debugger_launch() == PAL_DEBUGGER_LAUNCHED);
    CHECK(wait_for_file(started));

    /* 3. 動いている間の再要求は二重起動にならない。 */
    CHECK(pal_debugger_launch() == PAL_DEBUGGER_ALREADY_RUNNING);

    /* 4. 終了すれば回収され、再度起動できる。 */
    FILE *fp = fopen(release, "w");
    CHECK(fp != NULL);
    if (fp)
        fclose(fp);
    pal_debugger_launch_result_t again = PAL_DEBUGGER_ALREADY_RUNNING;
    for (int i = 0; i < WAIT_MAX_STEPS && again == PAL_DEBUGGER_ALREADY_RUNNING; i++) {
        again = pal_debugger_launch();
        if (again == PAL_DEBUGGER_ALREADY_RUNNING)
            sleep_step();
    }
    CHECK(again == PAL_DEBUGGER_LAUNCHED);

    /* 後始末: 1回目・2回目の子は release が既にあるので自然に終わる。両方の終了を
     * 待ってから片付ける（子が残らないように）。 */
    CHECK(wait_for_lines(finished, 2));
    unlink(started);
    unlink(release);
    unlink(finished);
    unlink(script);
    rmdir(dir);

    if (g_failures) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("debugger_launcher_test: OK\n");
    return 0;
}
