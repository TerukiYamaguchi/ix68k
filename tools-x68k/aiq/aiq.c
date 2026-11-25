#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <x68k/dos.h>
#include <x68k/iocs.h>

/* AIQ.X: 自然言語の指示を RS-232C 経由でホスト側ブリッジ(tools-host/aiq-bridge)へ
 * 送り、返ってきた Human68k コマンド列を確認のうえ順に実行する（Issue #64）。
 *
 * プロトコル（1行1メッセージ、CR/LF区切り、Shift_JIS）:
 *   送信: ASK <指示文>
 *   受信: MSG <回答の1行> と CMD <コマンド> を各0行以上、最後に END ／ エラー時は ERR <理由>
 *
 * 使い方: AIQ <指示文>
 * 各コマンドは COMMAND.X の /C で実行する（内部コマンドも外部コマンドも同じ経路）。
 * 1つでも失敗したら、そこで停止する。
 */

#define AIQ_VERSION    "0.9"
#define MAX_LINE_BYTES 255
#define LINE_BUF_BYTES (MAX_LINE_BYTES + 1)
#define MAX_COMMANDS   32
#define RX_TIMEOUT_CS  9000   /* 90秒（ブリッジの LLM 待ち 60秒より長く取る） */
#define CS_PER_DAY     8640000 /* _ONTIME は 1/100 秒単位で 0:00 に戻る */
#define KEY_ESC        0x1B
#define RS232_NOT_READY 0

#define PREFIX_ASK "ASK "
#define PREFIX_MSG "MSG "
#define PREFIX_CMD "CMD "
#define PREFIX_ERR "ERR "
#define LINE_END   "END"

#define EXIT_OK      0
#define EXIT_FAILURE_GENERIC 1

static const char *const DESTRUCTIVE_COMMANDS[] = {
    "DEL", "DELETE", "ERASE", "FORMAT", "RD", "RMDIR", "COPY", "MOVE", NULL,
};

static char s_commands[MAX_COMMANDS][LINE_BUF_BYTES];
static int s_command_count;
static int s_message_count;

static int now_cs(void) {
    return _iocs_ontime().sec;
}

static int elapsed_cs(int start_cs) {
    int diff = now_cs() - start_cs;
    return diff < 0 ? diff + CS_PER_DAY : diff;
}

static int esc_pressed(void) {
    if (!_iocs_b_keysns())
        return 0;
    return (_iocs_b_keyinp() & 0xFF) == KEY_ESC;
}

static void flush_rx(void) {
    while (_iocs_isns232c() != RS232_NOT_READY)
        _iocs_inp232c();
}

static int send_line(const char *text) {
    for (const char *p = text; *p; p++) {
        while (_iocs_osns232c() == RS232_NOT_READY) {
            if (esc_pressed())
                return -1;
        }
        _iocs_out232c((unsigned char)*p);
    }
    for (const char *p = "\r\n"; *p; p++) {
        while (_iocs_osns232c() == RS232_NOT_READY) {
        }
        _iocs_out232c((unsigned char)*p);
    }
    return 0;
}

enum read_result { READ_OK, READ_TIMEOUT, READ_ABORT, READ_OVERFLOW };

/* 空行を読み飛ばして1行読む。 */
static enum read_result read_line(char *buf) {
    int len = 0;
    int start_cs = now_cs();
    for (;;) {
        if (_iocs_isns232c() == RS232_NOT_READY) {
            if (esc_pressed())
                return READ_ABORT;
            if (elapsed_cs(start_cs) > RX_TIMEOUT_CS)
                return READ_TIMEOUT;
            continue;
        }
        int c = _iocs_inp232c() & 0xFF;
        if (c == '\r' || c == '\n') {
            if (len == 0)
                continue;
            buf[len] = '\0';
            return READ_OK;
        }
        if (len >= MAX_LINE_BYTES)
            return READ_OVERFLOW;
        buf[len++] = (char)c;
    }
}

static int has_prefix(const char *line, const char *prefix) {
    return strncmp(line, prefix, strlen(prefix)) == 0;
}

static int is_destructive(const char *command) {
    char word[16];
    int n = 0;
    while (command[n] && command[n] != ' ' && n < (int)sizeof(word) - 1) {
        word[n] = (char)toupper((unsigned char)command[n]);
        n++;
    }
    word[n] = '\0';
    for (int i = 0; DESTRUCTIVE_COMMANDS[i]; i++) {
        if (strcmp(word, DESTRUCTIVE_COMMANDS[i]) == 0)
            return 1;
    }
    return 0;
}

/* ブリッジの応答を受け取る。成功時 0、失敗時はメッセージを表示して 1 を返す。 */
static int receive_plan(void) {
    char line[LINE_BUF_BYTES];
    for (;;) {
        switch (read_line(line)) {
        case READ_OK:
            break;
        case READ_TIMEOUT:
            printf("AIQ: ホストからの応答がありません（タイムアウト）\r\n");
            return 1;
        case READ_ABORT:
            printf("AIQ: 中断しました\r\n");
            return 1;
        case READ_OVERFLOW:
            printf("AIQ: 応答の1行が長すぎます\r\n");
            return 1;
        }
        if (strcmp(line, LINE_END) == 0)
            return 0;
        if (has_prefix(line, PREFIX_ERR)) {
            printf("AIQ: ホストがエラーを返しました: %s\r\n", line + strlen(PREFIX_ERR));
            return 1;
        }
        if (has_prefix(line, PREFIX_MSG)) {
            printf("%s\r\n", line + strlen(PREFIX_MSG));
            s_message_count++;
            continue;
        }
        if (has_prefix(line, PREFIX_CMD)) {
            if (s_command_count >= MAX_COMMANDS) {
                printf("AIQ: コマンドが多すぎます（最大 %d）\r\n", MAX_COMMANDS);
                return 1;
            }
            strcpy(s_commands[s_command_count++], line + strlen(PREFIX_CMD));
            continue;
        }
        printf("AIQ: 不明な応答です: %s\r\n", line);
        return 1;
    }
}

static int confirm_plan(void) {
    printf("次のコマンドを実行します:\r\n");
    int destructive = 0;
    for (int i = 0; i < s_command_count; i++) {
        int danger = is_destructive(s_commands[i]);
        destructive |= danger;
        printf("  %s%s\r\n", danger ? "[!] " : "    ", s_commands[i]);
    }
    if (destructive)
        printf("[!] は内容を変更・削除する可能性があるコマンドです。\r\n");
    printf("実行しますか? [y/N] ");
    fflush(stdout); /* 改行なしの行は入力待ちの前に出力されないため */
    int answer = _dos_getchar();
    printf("\r\n");
    return answer == 'y' || answer == 'Y';
}

static int run_plan(void) {
    for (int i = 0; i < s_command_count; i++) {
        printf("> %s\r\n", s_commands[i]);
        int status = system(s_commands[i]);
        if (status != 0) {
            printf("AIQ: コマンドが失敗しました（終了コード %d）。以降は実行しません\r\n", status);
            return 1;
        }
    }
    return 0;
}

static int build_request(int argc, char **argv, char *request) {
    size_t len = strlen(PREFIX_ASK);
    strcpy(request, PREFIX_ASK);
    for (int i = 1; i < argc; i++) {
        size_t arg_len = strlen(argv[i]);
        if (len + arg_len + 1 > MAX_LINE_BYTES)
            return -1;
        if (i > 1)
            request[len++] = ' ';
        memcpy(request + len, argv[i], arg_len);
        len += arg_len;
    }
    request[len] = '\0';
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && (strcmp(argv[1], "-V") == 0 || strcmp(argv[1], "-v") == 0)) {
        printf("AIQ.X Ver.%s\r\n", AIQ_VERSION);
        return EXIT_OK;
    }
    if (argc < 2) {
        printf("AIQ.X Ver.%s\r\n", AIQ_VERSION);
        printf("使い方: AIQ <指示文>\r\n");
        return EXIT_FAILURE_GENERIC;
    }
    char request[LINE_BUF_BYTES];
    if (build_request(argc, argv, request) != 0) {
        printf("AIQ: 指示文が長すぎます（最大 %d バイト）\r\n", MAX_LINE_BYTES);
        return EXIT_FAILURE_GENERIC;
    }

    _iocs_set232c(-1); /* 現在の通信条件を変えずにドライバを初期化状態にする */
    flush_rx();
    if (send_line(request) != 0) {
        printf("AIQ: 中断しました\r\n");
        return EXIT_FAILURE_GENERIC;
    }
    printf("AIQ: ホストに問い合わせ中... (ESCで中断)\r\n");

    if (receive_plan() != 0)
        return EXIT_FAILURE_GENERIC;
    if (s_command_count == 0) {
        if (s_message_count == 0)
            printf("AIQ: 実行するコマンドはありません\r\n");
        return EXIT_OK;
    }
    if (s_message_count > 0)
        printf("\r\n");
    if (!confirm_plan()) {
        printf("AIQ: 実行を取りやめました\r\n");
        return EXIT_OK;
    }
    return run_plan() == 0 ? EXIT_OK : EXIT_FAILURE_GENERIC;
}
