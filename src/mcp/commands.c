#include "mcp/commands.h"

#include "app/checkpoint.h"
#include "app_internal.h"
#include "mc68000/cpu.h"
#include "mc68000/debug.h"
#include "mcp/debugger.h"
#include "mcp/internal.h"
#include "mcp/perf.h"
#include "mcp/screenshot.h"
#include "mcp/text_event.h"
#include "mcp/text_probe.h"
#include "peripheral/mfp.h"
#include "platform/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * ワイヤコマンドは "<GROUP> [<ACTION>] [args...]" の2段構造で、グループ表→
 * アクション表のテーブル駆動だけでディスパッチする（if/strncmp連鎖は書かない）。
 * MCPツール1つがワイヤのグループ1つに対応するので、ツールを増やすときに
 * 「どこに足すか」を迷わずに済む。
 */

/* HELLO が返すプロトコル版。ワイヤ書式・グループ名・応答書式のいずれかを
 * 互換性なく変えたら上げること。クライアントは接続時にこれを検証し、
 * 「エミュレータが古い」ことを（コマンドが軒並みエラーになる前に）判別する。
 * v3: 長さプレフィックスのフレーミング＋tag＋base64廃止。
 * v4: stopinfoへMODE/CAP/EXECを常時追加。
 * v5: PERFグループ（サイクル計測）を追加。 */
#define MCP_PROTO_VERSION 5

typedef void (*mcp_action_fn)(x68k_bus_t *bus, mcp_context_t *ctx, const char *args);

typedef struct {
    const char *action; /* NULL = このグループはアクションを取らない（残り全部がargs） */
    mcp_action_fn handler;
} mcp_action_entry_t;

typedef struct {
    const char *group;
    const mcp_action_entry_t *actions;
    size_t count;
} mcp_group_entry_t;

/* ---- 応答ヘルパ -------------------------------------------------------- */

/*
 * エラー応答は "ERR <CODE> <message>" 固定。CODE を機械可読にしておくと、
 * 呼び出し側が「まだ来ていない（リトライ可）」と「引数が不正（再試行しても
 * 無駄）」を区別できる。
 */
#define MCP_ERR_UNKNOWN  "UNKNOWN"  /* 未知のグループ・アクション */
#define MCP_ERR_BADARG   "BADARG"   /* 引数の形式・範囲が不正 */
#define MCP_ERR_NOTREADY "NOTREADY" /* 相手側の操作待ちで今は応じられない */
#define MCP_ERR_TIMEOUT  "TIMEOUT"  /* 待ちが期限切れ（異常ではない） */
#define MCP_ERR_NOCPU    "NOCPU"    /* CPU未初期化 */
#define MCP_ERR_FULL     "FULL"     /* 登録数上限 */
#define MCP_ERR_IO       "IO"       /* ファイル・デバイスの入出力失敗 */
#define MCP_ERR_OOM      "OOM"      /* メモリ確保失敗 */
#define MCP_ERR_INTERNAL "INTERNAL" /* 実装内部の異常（stopinfo切り詰め等） */

static void mcp_err(const char *code, const char *msg) {
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "ERR %s %s\n", code, msg);
    mcp_reply(buf, (size_t)n);
}

static void mcp_ok(void) {
    mcp_reply("OK\n", 3);
}

static void mcp_send_dump_reply(const char *text, int text_len) {
    if (text_len < 0)
        text_len = 0;
    mcp_reply_bin((const uint8_t *)text, (size_t)text_len);
}

int mcp_build_stopinfo_reply(char *info, size_t info_sz, char *reply, size_t reply_sz) {
    int n = dbg_format_stopinfo(info, info_sz);
    if (n < 0 || (size_t)n >= info_sz)
        return -1;
    int len = snprintf(reply, reply_sz, "OK %s\n", info);
    if (len < 0 || (size_t)len >= reply_sz)
        return -1;
    return len;
}

static void mcp_send_stopinfo(void) {
    char info[MCP_STOPINFO_MAX];
    char reply[MCP_STOPINFO_MAX + 4];
    int len = mcp_build_stopinfo_reply(info, sizeof(info), reply, sizeof(reply));
    if (len < 0) {
        pal_log("mcp: stopinfo truncated (MCP_STOPINFO_MAX too small for current state)\n");
        mcp_err(MCP_ERR_INTERNAL, "stopinfo truncated");
        return;
    }
    mcp_reply(reply, (size_t)len);
}

/* args の先頭トークンを out へ取り出し、続きの先頭を返す。 */
static const char *mcp_next_token(const char *args, char *out, size_t outsz) {
    while (*args == ' ')
        args++;
    size_t n = 0;
    while (*args && *args != ' ') {
        if (n + 1 < outsz)
            out[n++] = *args;
        args++;
    }
    out[n] = '\0';
    while (*args == ' ')
        args++;
    return args;
}

/* ---- HELLO / システム -------------------------------------------------- */

static void mcp_cmd_hello(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)bus;
    (void)ctx;
    (void)args;
    char reply[96];
    int n = snprintf(reply, sizeof(reply), "OK PROTO=%d BUILD=%s %s\n", MCP_PROTO_VERSION,
                     __DATE__, __TIME__);
    mcp_reply(reply, (size_t)n);
}

/* RESET [FD0]: 引数 FD0 で実機の「OPT.1を押しながらリセット」相当。 */
static void mcp_cmd_reset(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)ctx;
    int force_fd0 = strcmp(args, "FD0") == 0;
    if (!force_fd0 && args[0] != '\0') {
        mcp_err(MCP_ERR_BADARG, "RESET takes nothing or FD0");
        return;
    }
    if (force_fd0) {
        app_reset_machine(bus);
        app_force_fd0_boot_begin(bus);
    } else {
        app_force_fd0_boot_cancel(bus);
        app_reset_machine(bus);
    }
    mcp_ok();
}

static void mcp_cmd_interrupt(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)ctx;
    (void)args;
    /* pull型IRQ受付ではCPUはpending_irqを見ないため、
     * mc68k_set_irq()を直接呼んでも効果がない。bus層のsticky NMIフラグ
     * （x68k_bus_irq_level()が最優先で読む）へ注入する。 */
    x68k_bus_set_nmi_pending(bus);
    mcp_ok();
}

/* ---- POWER ------------------------------------------------------------- */

/* 前面電源スイッチ操作。File > Power Switch (OFF) と同じ経路で
 * GPIP2を動かす。OFFでゲストの電源断処理（$E8E00F）が走るとix68kは終了する。 */
static void mcp_cmd_power_off(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)ctx;
    (void)args;
    x68k_bus_mfp_power_switch(bus, 0);
    mcp_ok();
}

static void mcp_cmd_power_on(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)ctx;
    (void)args;
    x68k_bus_mfp_power_switch(bus, 1);
    mcp_ok();
}

/* ---- WHITEBAND --------------------------------------------------------- */

static void mcp_cmd_whiteband_on(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)bus;
    (void)ctx;
    (void)args;
    s_app.whiteband_halt_on = 1;
    if (s_app.cpu)
        mc68k_set_pc_ring(s_app.cpu, 1);
    pal_log("WHITEBAND ON: _ABORTRST($FD) で HALT する\n");
    mcp_ok();
}

static void mcp_cmd_whiteband_off(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)bus;
    (void)ctx;
    (void)args;
    s_app.whiteband_halt_on = 0;
    pal_log("WHITEBAND OFF: _ABORTRST($FD) で HALT しない\n");
    mcp_ok();
}

/* 白帯検出で停止したCPUの再開。停止はデバッガの停止として表現されるので
 * DBG CONT と同じ解除経路を通る（whiteband(action="continue") 用の別名）。 */
static void mcp_cmd_whiteband_cont(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)bus;
    (void)ctx;
    (void)args;
    dbg_continue();
    mcp_ok();
}

/* ---- DIAG（診断ログ・トレース記録のON/OFF） ---------------------------- */

static void mcp_diag_iocs(int on) {
    s_app.trace_iocs = on;
    pal_log("DIAG IOCS %s: IOCS full trace\n", on ? "ON" : "OFF");
}
static void mcp_diag_badjump(int on) {
    dbg_set_badjump_log(on);
    if (on && s_app.cpu)
        mc68k_set_pc_ring(s_app.cpu, 1);
}
static void mcp_diag_badmovea(int on) {
    dbg_set_badmovea_log(on);
}
static void mcp_diag_badaddr(int on) {
    dbg_set_badaddr_log(on);
}
static void mcp_diag_badori(int on) {
    dbg_set_badori_log(on);
    if (on && s_app.cpu)
        mc68k_set_pc_ring(s_app.cpu, 1);
}
static void mcp_diag_pcring(int on) {
    if (s_app.cpu)
        mc68k_set_pc_ring(s_app.cpu, on);
}
static void mcp_diag_regring(int on) {
    if (s_app.cpu)
        mc68k_set_reg_ring(s_app.cpu, on);
}
static void mcp_diag_faultring(int on) {
    if (s_app.cpu)
        mc68k_set_fault_ring(s_app.cpu, on);
}
static void mcp_diag_pctrace1024(int on) {
    if (s_app.cpu)
        mc68k_set_pctrace1024(s_app.cpu, on);
}

static const struct {
    const char *kind;
    void (*set)(int on);
} s_diag_kinds[] = {
    {"IOCS", mcp_diag_iocs},           {"BADJUMP", mcp_diag_badjump},
    {"BADMOVEA", mcp_diag_badmovea},   {"BADADDR", mcp_diag_badaddr},
    {"BADORI", mcp_diag_badori},       {"PCRING", mcp_diag_pcring},
    {"REGRING", mcp_diag_regring},     {"PCTRACE1024", mcp_diag_pctrace1024},
    {"FAULTRING", mcp_diag_faultring},
};

/* DIAG <kind> <ON|OFF>。旧 TRACE_ON/BADJUMP_ON/... の16コマンドを1本に畳む。 */
static void mcp_cmd_diag(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)bus;
    (void)ctx;
    char kind[16], onoff[8];
    args = mcp_next_token(args, kind, sizeof(kind));
    mcp_next_token(args, onoff, sizeof(onoff));
    int on;
    if (strcmp(onoff, "ON") == 0)
        on = 1;
    else if (strcmp(onoff, "OFF") == 0)
        on = 0;
    else {
        mcp_err(MCP_ERR_BADARG, "DIAG <kind> ON|OFF");
        return;
    }
    for (size_t i = 0; i < sizeof(s_diag_kinds) / sizeof(s_diag_kinds[0]); i++) {
        if (strcmp(kind, s_diag_kinds[i].kind) == 0) {
            s_diag_kinds[i].set(on);
            mcp_ok();
            return;
        }
    }
    mcp_err(MCP_ERR_BADARG, "unknown diag kind");
}

/* ---- RING（履歴リングのダンプ取得） ------------------------------------ */

/* RING <PCRING|REGRING|CALLRING|FAULTRING> [count]。記録のON/OFFは DIAG 側（名前の衝突を解消）。 */
static void mcp_cmd_ring(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)bus;
    (void)ctx;
    if (!s_app.cpu) {
        mcp_err(MCP_ERR_NOCPU, "no cpu");
        return;
    }
    char kind[16];
    args = mcp_next_token(args, kind, sizeof(kind));
    long count = args[0] ? strtol(args, NULL, 10) : 0;
    static char text[DBG_LOG_CAP_SIZE];
    int text_len;
    if (strcmp(kind, "CALLRING") == 0)
        text_len = dbg_dump_call_ring(text, sizeof(text), (int)count);
    else if (strcmp(kind, "PCRING") == 0) {
        if (!mc68k_get_pc_ring(s_app.cpu))
            text_len =
                snprintf(text, sizeof(text), "PCRING disabled; enable DIAG PCRING ON\n");
        else
            text_len = dbg_dump_pc_ring(text, sizeof(text), (int)count);
    } else if (strcmp(kind, "REGRING") == 0)
        text_len = dbg_dump_reg_ring(text, sizeof(text), (int)count);
    else if (strcmp(kind, "FAULTRING") == 0)
        text_len = dbg_dump_fault_ring(text, sizeof(text), (int)count);
    else {
        mcp_err(MCP_ERR_BADARG, "RING PCRING|REGRING|CALLRING|FAULTRING [count]");
        return;
    }
    mcp_send_dump_reply(text, text_len);
}

/* ---- WATCH（RAM書き込みウォッチ） -------------------------------------- */

static void mcp_cmd_watch_on(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)ctx;
    char *endp;
    uint32_t addr = (uint32_t)strtoul(args, &endp, 16);
    uint32_t len = (*endp == ' ') ? (uint32_t)strtoul(endp + 1, NULL, 16) : 1;
    if (len == 0)
        len = 1;
    x68k_bus_set_ram_watch(bus, addr, len);
    pal_log("WATCH ON: RAM書き込みウォッチ有効化 addr=%06X len=%X\n", addr, len);
    mcp_ok();
}

static void mcp_cmd_watch_off(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)ctx;
    (void)args;
    x68k_bus_set_ram_watch(bus, 0, 0);
    pal_log("WATCH OFF: RAM書き込みウォッチ無効化\n");
    mcp_ok();
}

/* ---- HOSTFILE ---------------------------------------------------------- */

static void mcp_cmd_hostfile_put(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)ctx;
    /* MCP側のPUTはホストから送る方向。データ全体をRAMへ複製せず、ゲストの
     * HOSTFILE GETが要求するたびに開いたファイルから読む。 */
    long size = x68k_bus_hostfile_put(bus, args);
    if (size < 0) {
        mcp_err(MCP_ERR_IO, "open failed");
        return;
    }
    char reply[32];
    int len = snprintf(reply, sizeof(reply), "OK %ld\n", size);
    mcp_reply(reply, (size_t)len);
}

static void mcp_cmd_hostfile_get(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)ctx;
    /* MCP側のGETは、ゲストのHOSTFILE PUT済み一時ファイルをrenameで取得する。 */
    long size = x68k_bus_hostfile_get(bus, args);
    if (size < 0) {
        mcp_err(MCP_ERR_NOTREADY, "guest has not sent a file yet");
        return;
    }
    char reply[32];
    int len = snprintf(reply, sizeof(reply), "OK %ld\n", size);
    mcp_reply(reply, (size_t)len);
}

static void mcp_cmd_hostfile_status(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)ctx;
    (void)args;
    int state;
    uint32_t size;
    x68k_bus_hostfile_status(bus, &state, &size);
    char reply[32];
    int len = snprintf(reply, sizeof(reply), "OK %d %u\n", state, size);
    mcp_reply(reply, (size_t)len);
}

/* ---- DISK -------------------------------------------------------------- */

/* DISK INSERT <FD|SCSI> <index> <path> */
static void mcp_cmd_disk_insert(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)ctx;
    char media[8], index[8];
    args = mcp_next_token(args, media, sizeof(media));
    args = mcp_next_token(args, index, sizeof(index));
    int unit = (int)strtol(index, NULL, 10);
    if (args[0] == '\0') {
        mcp_err(MCP_ERR_BADARG, "DISK INSERT <FD|SCSI> <index> <path>");
        return;
    }
    if (strcmp(media, "FD") == 0) {
        int err = x68k_bus_insert_fdc_runtime(bus, unit, args);
        if (err == 0)
            mcp_ok();
        else if (err == X68K_FDC_ERR_ALREADY_INSERTED)
            mcp_err(MCP_ERR_NOTREADY, "disk present, eject first");
        else if (err == X68K_FDC_ERR_BAD_EXTENSION)
            mcp_err(MCP_ERR_IO, "load failed: unsupported extension");
        else if (err == X68K_FDC_ERR_SIZE_MISMATCH)
            mcp_err(MCP_ERR_IO, "load failed: file size does not match the media type");
        else
            mcp_err(MCP_ERR_IO, "load failed");
    } else if (strcmp(media, "SCSI") == 0) {
        if (x68k_bus_scsi_insert(bus, unit, args) == 0)
            mcp_ok();
        else
            mcp_err(MCP_ERR_IO, "load failed");
    } else
        mcp_err(MCP_ERR_BADARG, "media must be FD or SCSI");
}

/* DISK EJECT <FD|SCSI> <index> */
static void mcp_cmd_disk_eject(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)ctx;
    char media[8];
    args = mcp_next_token(args, media, sizeof(media));
    int unit = (int)strtol(args, NULL, 10);
    if (strcmp(media, "FD") == 0) {
        if (unit < 0 || unit >= 4) {
            mcp_err(MCP_ERR_BADARG, "FD index must be 0-3");
            return;
        }
        x68k_bus_eject_fdc(bus, unit);
        mcp_ok();
    } else if (strcmp(media, "SCSI") == 0) {
        if (unit < 0 || unit > 6) {
            mcp_err(MCP_ERR_BADARG, "SCSI index must be 0-6");
            return;
        }
        x68k_bus_scsi_eject(bus, unit);
        mcp_ok();
    } else
        mcp_err(MCP_ERR_BADARG, "media must be FD or SCSI");
}

/* DISK WP <FD|SCSI> <index> <ON|OFF>: 挿入中ディスクのライトプロテクトを
 * トグルする（FD・SCSI(MO)）。ホストの
 * ファイルパーミッションを実際に変更する。 */
static void mcp_cmd_disk_wp(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)ctx;
    char media[8], index[8], onoff[8];
    args = mcp_next_token(args, media, sizeof(media));
    args = mcp_next_token(args, index, sizeof(index));
    mcp_next_token(args, onoff, sizeof(onoff));
    int protect;
    if (strcmp(onoff, "ON") == 0)
        protect = 1;
    else if (strcmp(onoff, "OFF") == 0)
        protect = 0;
    else {
        mcp_err(MCP_ERR_BADARG, "DISK WP <FD|SCSI> <index> <ON|OFF>");
        return;
    }
    int unit = (int)strtol(index, NULL, 10);
    if (strcmp(media, "FD") == 0) {
        int err = x68k_bus_fdc_set_write_protect(bus, unit, protect);
        if (err == 0)
            mcp_ok();
        else if (err == X68K_FDC_ERR_INVALID_DRIVE)
            mcp_err(MCP_ERR_BADARG, "FD index must be 0-3");
        else if (err == X68K_FDC_ERR_NO_DISK || err == X68K_FDC_ERR_BUSY)
            mcp_err(MCP_ERR_NOTREADY, "disk not present or FDC busy");
        else
            mcp_err(MCP_ERR_IO, "write-protect toggle failed");
    } else if (strcmp(media, "SCSI") == 0) {
        int err = x68k_bus_scsi_set_write_protect(bus, unit, protect);
        if (err == 0)
            mcp_ok();
        else if (err == X68K_SCSI_ERR_INVALID_ID || err == X68K_SCSI_ERR_NOT_SUPPORTED)
            mcp_err(MCP_ERR_BADARG, "SCSI index must be 0-6 and removable(MO)");
        else if (err == X68K_SCSI_ERR_NO_MEDIA || err == X68K_SCSI_ERR_BUSY)
            mcp_err(MCP_ERR_NOTREADY, "media not present or SCSI bus busy");
        else
            mcp_err(MCP_ERR_IO, "write-protect toggle failed");
    } else
        mcp_err(MCP_ERR_BADARG, "DISK WP <FD|SCSI> <index> <ON|OFF>");
}

/* 装着中のディスクを "FD<n>=<path>" / "SCSI<n>=<path>" の行で返す。パスは空なら
 * メディア無し。挿入中のFDDについては "FD<n>.WP=<0|1>" 行を追加する。
 *SCSIはドライブが存在するIDだけ出す（存在しないIDは行ごと省く）。
 * メディア挿入中かつremovableなSCSI_UNIT_DISK(MO)についてのみ
 * "SCSI<n>.WP=<0|1>" 行を追加する（HDD/CD-ROMは値が固定で
 * 情報量が無いため省く）。 */
static void mcp_cmd_disk_status(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)ctx;
    (void)args;
    char text[4096];
    size_t n = 0;
    for (int d = 0; d < 4; d++) {
        const char *path = x68k_bus_fdc_disk_path(bus, d);
        int w = snprintf(text + n, sizeof(text) - n, "FD%d=%s\n", d, path ? path : "");
        if (w < 0 || (size_t)w >= sizeof(text) - n)
            break;
        n += (size_t)w;
        if (path) {
            w = snprintf(text + n, sizeof(text) - n, "FD%d.WP=%d\n", d,
                         x68k_bus_fdc_write_protected(bus, d));
            if (w < 0 || (size_t)w >= sizeof(text) - n)
                break;
            n += (size_t)w;
        }
    }
    for (int id = 0; id < 7; id++) {
        const char *path = NULL;
        if (!x68k_bus_scsi_unit_status(bus, id, &path))
            continue;
        int w = snprintf(text + n, sizeof(text) - n, "SCSI%d=%s\n", id, path ? path : "");
        if (w < 0 || (size_t)w >= sizeof(text) - n)
            break;
        n += (size_t)w;
        if (path && x68k_bus_scsi_unit_removable(bus, id) &&
            !x68k_bus_scsi_unit_is_cdrom(bus, id)) {
            w = snprintf(text + n, sizeof(text) - n, "SCSI%d.WP=%d\n", id,
                         x68k_bus_scsi_write_protected(bus, id));
            if (w < 0 || (size_t)w >= sizeof(text) - n)
                break;
            n += (size_t)w;
        }
    }
    mcp_reply_bin((const uint8_t *)text, n);
}

/* ---- KEY / MOUSE / JOY / PASTE ----------------------------------------- */

static void mcp_cmd_key_scan(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)bus;
    (void)ctx;
    iocs_key_wait_input_queued();
    pal_key_inject_raw((uint32_t)strtoul(args, NULL, 16));
    mcp_ok();
}

static void mcp_cmd_key_down(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)bus;
    (void)ctx;
    iocs_key_wait_input_queued();
    pal_key_inject_down((uint32_t)strtoul(args, NULL, 16));
    mcp_ok();
}

static void mcp_cmd_key_up(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)bus;
    (void)ctx;
    iocs_key_wait_input_queued();
    pal_key_inject_up((uint32_t)strtoul(args, NULL, 16));
    mcp_ok();
}

static void mcp_cmd_key_clear(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)bus;
    (void)args;
    pal_key_clear_queue();
    ctx->paste_len = 0;
    ctx->paste_idx = 0;
    s_app.key_input_paste_active = 0;
    mcp_ok();
}

/* KEY CHECK: いま B_KEYINP 待ちかを即座に返す（ノンブロッキング）。 */
static void mcp_cmd_key_check(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)bus;
    (void)ctx;
    (void)args;
    char reply[16];
    int len = snprintf(reply, sizeof(reply), "OK %d\n", s_app.key_input_waiting ? 1 : 0);
    mcp_reply(reply, (size_t)len);
}

/* KEY PASTE: PASTE キューの残りバイト数・MFP USART RX の未読有無・FIFO 破棄累計を返す。
 * IOCS を使わないゲスト（NetBSD 等）で paste_text が送信完了を判定するために使う。 */
static void mcp_cmd_key_paste(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)args;
    char reply[64];
    int len = snprintf(reply, sizeof(reply), "OK %d %d %u\n", ctx->paste_len - ctx->paste_idx,
                       x68k_bus_mfp_usart_rx_pending(bus) ? 1 : 0,
                       (unsigned)x68k_bus_mfp_usart_rx_dropped(bus));
    mcp_reply(reply, (size_t)len);
}

/* KEY KBDSTATE: USART TX（UDR書き込み）で受けたホスト→キーボードコマンドの保持状態と
 * 送信バイト累計を返す。LED/リピート設定が実際にキーボードへ届いたかの観測用。 */
static void mcp_cmd_key_kbdstate(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)ctx;
    (void)args;
    const kbd_state_t *k = x68k_bus_kbd_state(bus);
    char reply[96];
    int len = snprintf(reply, sizeof(reply), "OK LED=%02X BRIGHT=%X DELAY=%X RATE=%X TX=%u\n",
                       k->led, k->led_bright, k->repeat_delay, k->repeat_rate,
                       (unsigned)x68k_bus_mfp_usart_tx_count(bus));
    mcp_reply(reply, (size_t)len);
}

static void mcp_cmd_mouse_move(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)bus;
    (void)ctx;
    char *endp;
    long dx = strtol(args, &endp, 10);
    long dy = (endp && *endp == ' ') ? strtol(endp + 1, NULL, 10) : 0;
    pal_mouse_inject_motion((int)dx, (int)dy);
    mcp_ok();
}

static void mcp_cmd_mouse_button(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)bus;
    (void)ctx;
    char *endp;
    long left = strtol(args, &endp, 10);
    long right = (endp && *endp == ' ') ? strtol(endp + 1, NULL, 10) : 0;
    pal_mouse_inject_button((int)left, (int)right);
    mcp_ok();
}

static void mcp_cmd_joy_set(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)bus;
    (void)ctx;
    char *endp;
    long index = strtol(args, &endp, 10);
    unsigned long value = (endp && *endp == ' ') ? strtoul(endp + 1, NULL, 16) : 0xFFu;
    pal_joystick_inject((int)index, (uint8_t)value);
    mcp_ok();
}

static void mcp_cmd_joy_release(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)bus;
    (void)ctx;
    pal_joystick_release((int)strtol(args, NULL, 10));
    mcp_ok();
}

static void mcp_cmd_paste(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)bus;
    /* キューへの予約時点で前回のKEY WAITを解除する。実際の最初の文字はpollで
     * 注入されるので、ここで解除しないと run_command() の後段KEY WAITが古い
     * 待ちを見て早期完了してしまう。 */
    iocs_key_wait_input_queued();
    ctx->paste_len = 0;
    ctx->paste_idx = 0;
    while (args[0] && args[1] && ctx->paste_len < (int)sizeof(ctx->paste_queue)) {
        int h = (args[0] >= '0' && args[0] <= '9')   ? args[0] - '0'
                : (args[0] >= 'A' && args[0] <= 'F') ? args[0] - 'A' + 10
                : (args[0] >= 'a' && args[0] <= 'f') ? args[0] - 'a' + 10
                                                     : 0;
        int l = (args[1] >= '0' && args[1] <= '9')   ? args[1] - '0'
                : (args[1] >= 'A' && args[1] <= 'F') ? args[1] - 'A' + 10
                : (args[1] >= 'a' && args[1] <= 'f') ? args[1] - 'a' + 10
                                                     : 0;
        ctx->paste_queue[ctx->paste_len++] = (uint8_t)((h << 4) | l);
        args += 2;
    }
    s_app.key_input_paste_active = ctx->paste_len != 0;
    ctx->paste_next_us = pal_get_ticks_us();
    mcp_ok();
}

/* ---- MEM / REGS / DISASM ----------------------------------------------- */

static void mcp_cmd_mem_read(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)ctx;
    char *endp;
    uint32_t addr = (uint32_t)strtoul(args, &endp, 16);
    uint32_t len = (endp && *endp == ' ') ? (uint32_t)strtoul(endp + 1, NULL, 16) : 16;
    if (len > 256)
        len = 256;
    uint8_t data[256];
    for (uint32_t i = 0; i < len; i++)
        data[i] = x68k_bus_read8_full(bus, addr + i);
    mcp_reply_bin(data, len);
}

#define MCP_MEM_PEEK_SUPER   "SUPER"
#define MCP_FC_SUPER_PROGRAM 6u /* 68kファンクションコード: スーパーバイザプログラム空間 */

static void mcp_cmd_mem_peek(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)bus;
    (void)ctx;
    if (!s_app.cpu) {
        mcp_err(MCP_ERR_NOCPU, "no cpu");
        return;
    }
    char *endp;
    uint32_t addr = (uint32_t)strtoul(args, &endp, 16);
    uint32_t len = 16;
    const char *option = "";
    if (endp && *endp == ' ') {
        len = (uint32_t)strtoul(endp + 1, &endp, 16);
        option = endp ? endp : "";
    }
    if (len > 256)
        len = 256;
    while (*option == ' ')
        ++option;
    bool supervisor = strcmp(option, MCP_MEM_PEEK_SUPER) == 0;
    if (*option != '\0' && !supervisor) {
        mcp_err(MCP_ERR_BADARG, "usage: MEM PEEK <addr> <len> [SUPER]");
        return;
    }
    uint8_t data[256];
    /* SUPER は現行SRに依らずスーパーバイザプログラム空間で読む
     * （ユーザーモード中でもカーネルテキストを読める）。 */
    int rc = supervisor
                 ? mc68k_debug_read_bytes_fc(s_app.cpu, addr, data, len, MCP_FC_SUPER_PROGRAM)
                 : mc68k_debug_read_bytes(s_app.cpu, addr, data, len);
    if (rc != 0) {
        mcp_err(MCP_ERR_NOTREADY, "peek hooks unavailable");
        return;
    }
    mcp_reply_bin(data, len);
}

/* MEM WRITE <addr>。書き込むバイト列はヘッダ行ではなく、フレームの本文
 * （ctx->request_body）で渡す（base64廃止）。 */
static void mcp_cmd_mem_write(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    char *endp;
    uint32_t addr = (uint32_t)strtoul(args, &endp, 16);
    if (args[0] == '\0' || (endp && *endp != '\0')) {
        mcp_err(MCP_ERR_BADARG, "MEM WRITE <addr> (binary body follows)");
        return;
    }
    if (ctx->request_body_len == 0) {
        mcp_err(MCP_ERR_BADARG, "no data");
        return;
    }
    if (ctx->request_body_len > 256) {
        mcp_err(MCP_ERR_BADARG, "body too large (max 256 bytes)");
        return;
    }
    for (size_t i = 0; i < ctx->request_body_len; i++)
        x68k_bus_write_byte(bus, addr + (uint32_t)i, ctx->request_body[i]);
    mcp_ok();
}

static void mcp_cmd_regs(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)bus;
    (void)ctx;
    (void)args;
    if (!s_app.cpu) {
        mcp_err(MCP_ERR_NOCPU, "no cpu");
        return;
    }
    mc68k_regs_t regs;
    mc68k_get_regs(s_app.cpu, &regs);
    char reply[300];
    int len =
        snprintf(reply, sizeof(reply),
                 "OK D0=%08X D1=%08X D2=%08X D3=%08X D4=%08X D5=%08X D6=%08X D7=%08X "
                 "A0=%08X A1=%08X A2=%08X A3=%08X A4=%08X A5=%08X A6=%08X A7=%08X "
                 "PC=%08X SR=%04X USP=%08X SSP=%08X\n",
                 regs.d[0], regs.d[1], regs.d[2], regs.d[3], regs.d[4], regs.d[5], regs.d[6],
                 regs.d[7], regs.a[0], regs.a[1], regs.a[2], regs.a[3], regs.a[4], regs.a[5],
                 regs.a[6], regs.a[7], regs.pc, regs.sr, regs.usp, regs.ssp);
    mcp_reply(reply, (size_t)len);
}

/* ---- MMU（PMMU / access-control レジスタ） ------------------------------
 * 68000/68010/68020 はPMMUを持たないため対象外（0を返すと有効な値と区別が
 * つかなくなるので、MMU=UNSUPPORTEDを明示する）。68EC030はTC/CRP/SRPが常に0の
 * 契約（cpu_types.h）で意味を持たないため出さず、AC0/AC1/ACUSRの名前で
 * tt0/tt1/mmusrを表示する。 */
static const char *mcp_mmu_model_name(mc68k_model_t model) {
    switch (model) {
    case MC68K_MODEL_68000:
        return "68000";
    case MC68K_MODEL_68010:
        return "68010";
    case MC68K_MODEL_68020:
        return "68020";
    case MC68K_MODEL_68EC030:
        return "68EC030";
    case MC68K_MODEL_68030:
        return "68030";
    case MC68K_MODEL_68040:
        return "68040";
    case MC68K_MODEL_68060:
        return "68060";
    }
    return "UNKNOWN";
}

/* CONFIG GET <KEY>: 起動時設定の読み取り専用の問い合わせ。MCPサーバ（別プロセス）が
 * .cfg を読まずに設定値を知るための窓口。対応するキーだけを返す。 */
#define MCP_CONFIG_KEY_TEXT_AUTO_PROBE "TEXT_AUTO_PROBE"

static void mcp_cmd_config_get(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)bus;
    (void)ctx;
    char key[32];
    args = mcp_next_token(args, key, sizeof(key));
    if (!key[0] || args[0]) {
        mcp_err(MCP_ERR_BADARG, "CONFIG GET <" MCP_CONFIG_KEY_TEXT_AUTO_PROBE ">");
        return;
    }
    if (strcmp(key, MCP_CONFIG_KEY_TEXT_AUTO_PROBE) != 0) {
        mcp_err(MCP_ERR_BADARG, "unknown config key");
        return;
    }
    char reply[48];
    int n = snprintf(reply, sizeof(reply), "OK %s=%d\n", key, s_app.text_auto_probe ? 1 : 0);
    mcp_reply(reply, (size_t)n);
}

static void mcp_cmd_mmu_get(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)bus;
    (void)ctx;
    (void)args;
    if (!s_app.cpu) {
        mcp_err(MCP_ERR_NOCPU, "no cpu");
        return;
    }
    mc68k_model_t model = mc68k_get_model(s_app.cpu);
    char reply[160];
    int len;
    if (model == MC68K_MODEL_68040) {
        mc68k_mmu040_regs_t regs;
        mc68k_get_mmu040_regs(s_app.cpu, &regs);
        len = snprintf(reply, sizeof(reply),
                       "OK MODEL=68040 URP=%08X SRP=%08X TC=%04X DTT0=%08X DTT1=%08X "
                       "ITT0=%08X ITT1=%08X MMUSR=%08X\n",
                       regs.urp, regs.srp, regs.tc, regs.dtt0, regs.dtt1, regs.itt0, regs.itt1,
                       regs.mmusr);
        mcp_reply(reply, (size_t)len);
        return;
    }
    if (model == MC68K_MODEL_68060) {
        mc68k_mmu060_regs_t regs;
        mc68k_get_mmu060_regs(s_app.cpu, &regs);
        len = snprintf(reply, sizeof(reply),
                       "OK MODEL=68060 URP=%08X SRP=%08X TC=%08X DTT0=%08X DTT1=%08X "
                       "ITT0=%08X ITT1=%08X\n",
                       regs.urp, regs.srp, regs.tc, regs.dtt0, regs.dtt1, regs.itt0, regs.itt1);
        mcp_reply(reply, (size_t)len);
        return;
    }
    if (model != MC68K_MODEL_68030 && model != MC68K_MODEL_68EC030) {
        len = snprintf(reply, sizeof(reply), "OK MODEL=%s MMU=UNSUPPORTED\n",
                       mcp_mmu_model_name(model));
        mcp_reply(reply, (size_t)len);
        return;
    }
    mc68k_mmu_regs_t regs;
    mc68k_get_mmu_regs(s_app.cpu, &regs);
    if (model == MC68K_MODEL_68EC030)
        len = snprintf(reply, sizeof(reply), "OK MODEL=68EC030 AC0=%08X AC1=%08X ACUSR=%04X\n",
                       regs.tt0, regs.tt1, regs.mmusr);
    else
        len = snprintf(reply, sizeof(reply),
                       "OK MODEL=68030 TC=%08X CRP=%016llX SRP=%016llX TT0=%08X TT1=%08X "
                       "MMUSR=%04X\n",
                       regs.tc, (unsigned long long)regs.crp, (unsigned long long)regs.srp,
                       regs.tt0, regs.tt1, regs.mmusr);
    mcp_reply(reply, (size_t)len);
}

static void mcp_cmd_disasm(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)bus;
    (void)ctx;
    if (!s_app.cpu) {
        mcp_err(MCP_ERR_NOCPU, "no cpu");
        return;
    }
    char *endp;
    uint32_t addr = (uint32_t)strtoul(args, &endp, 16);
    long count = (endp && *endp == ' ') ? strtol(endp + 1, NULL, 10) : 1;
    if (count < 1)
        count = 1;
    if (count > 64)
        count = 64;
    char text[64 * 128];
    size_t text_len = 0;
    for (long i = 0; i < count; i++) {
        char mnem[64];
        int ilen = mc68k_disassemble(s_app.cpu, addr, mnem, sizeof(mnem));
        if (ilen < 0)
            break;
        uint8_t ibuf[12];
        uint32_t rawlen = (uint32_t)ilen > sizeof(ibuf) ? sizeof(ibuf) : (uint32_t)ilen;
        mc68k_debug_read_bytes(s_app.cpu, addr, ibuf, rawlen);
        char hexwords[40];
        size_t hp = 0;
        hexwords[0] = '\0';
        for (uint32_t w = 0; w < rawlen && hp + 6 < sizeof(hexwords); w += 2) {
            uint16_t word =
                (uint16_t)(((uint16_t)ibuf[w] << 8) | (w + 1 < rawlen ? ibuf[w + 1] : 0));
            hp += (size_t)snprintf(hexwords + hp, sizeof(hexwords) - hp, "%s%04X",
                                   w == 0 ? "" : " ", word);
        }
        int n = snprintf(text + text_len, sizeof(text) - text_len, "%08X  %-18s  %s\n", addr,
                         hexwords, mnem);
        if (n < 0 || (size_t)n >= sizeof(text) - text_len)
            break;
        text_len += (size_t)n;
        addr += (uint32_t)ilen;
    }
    mcp_reply_bin((const uint8_t *)text, text_len);
}

/* ---- TEXT / SCREENSHOT ------------------------------------------------- */

static void mcp_cmd_text_stdout(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)bus;
    (void)args;
    if (mcp_reply_bin(ctx->output, (size_t)ctx->output_len))
        ctx->output_len = 0;
}

/* TEXT EVENTS は TEV1 + count/u32 + dropped/u32 + epoch/u64 の後に、各イベントを
 * fixed metadata（seq/epoch/pc/x/y/attr/flags/source/probe/encoding/payload_len）と
 * 生payloadで連結する。送信成功時だけ全件を消費する。 */
static void mcp_cmd_text_events(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)bus;
    (void)ctx;
    if (args[0] != '\0') {
        mcp_err(MCP_ERR_BADARG, "TEXT EVENTS takes no arguments");
        return;
    }
    mcp_text_event_status_t st = mcp_text_events_status();
    /* ペイロードは全イベント合計でもバイトリングの64 KiBを超えない。イベント数だけで
     * 最大payloadを掛けると約66 MiBを確保してしまうため、ワイヤ上限約2 MiBで確保する。 */
    size_t total = MCP_TEXT_EVENTS_WIRE_HEADER_BYTES +
                   (size_t)st.count * MCP_TEXT_EVENT_WIRE_METADATA_BYTES + MCP_TEXT_EVENT_BYTES;
    uint8_t *out = malloc(total);
    if (!out) {
        mcp_err(MCP_ERR_OOM, "text events");
        return;
    }
    size_t p = 0;
#define PUT8(v) (out[p++] = (uint8_t)(v))
#define PUT16(v)                         \
    do {                                 \
        uint16_t put16v = (uint16_t)(v); \
        PUT8(put16v >> 8);               \
        PUT8(put16v);                    \
    } while (0)
#define PUT32(v)                         \
    do {                                 \
        uint32_t put32v = (uint32_t)(v); \
        PUT8(put32v >> 24);              \
        PUT8(put32v >> 16);              \
        PUT8(put32v >> 8);               \
        PUT8(put32v);                    \
    } while (0)
#define PUT64(v)                         \
    do {                                 \
        uint64_t put64v = (uint64_t)(v); \
        PUT32((uint32_t)(put64v >> 32)); \
        PUT32((uint32_t)put64v);         \
    } while (0)
    memcpy(out + p, "TEV1", 4);
    p += 4;
    PUT32(st.count);
    PUT32(st.dropped);
    PUT64(st.epoch);
    for (uint32_t i = 0; i < st.count; ++i) {
        mcp_text_event_t e;
        uint8_t payload[MCP_TEXT_EVENT_MAX_PAYLOAD];
        if (mcp_text_event_get(i, &e, payload, sizeof(payload)) != 0) {
            free(out);
            mcp_err(MCP_ERR_INTERNAL, "event read");
            return;
        }
        PUT64(e.seq);
        PUT64(e.epoch);
        PUT32(e.pc);
        PUT32(e.x);
        PUT32(e.y);
        PUT32(e.attr);
        PUT16(e.flags);
        memcpy(out + p, e.source, sizeof(e.source));
        p += sizeof(e.source);
        memcpy(out + p, e.probe, sizeof(e.probe));
        p += sizeof(e.probe);
        memcpy(out + p, e.encoding, sizeof(e.encoding));
        p += sizeof(e.encoding);
        PUT32(e.payload_len);
        memcpy(out + p, payload, e.payload_len);
        p += e.payload_len;
    }
#undef PUT8
#undef PUT16
#undef PUT32
#undef PUT64
    if (mcp_reply_bin(out, p))
        mcp_text_events_discard(st.count);
    free(out);
}

/* TEXT PROBE は、Python側で検証済みの固定定義だけをCPUコアへ渡す。 */
static mcp_text_probe_def_t s_text_probe_defs[MC68K_MAX_ENTRY_PROBES];
static size_t s_text_probe_count;

static int mcp_parse_u32(const char *text, uint32_t *out) {
    char *end;
    unsigned long value;
    if (!text[0])
        return -1;
    value = strtoul(text, &end, 0);
    if (*end != '\0' || value > UINT32_MAX)
        return -1;
    *out = (uint32_t)value;
    return 0;
}

/* MCPのアドレス表記は既存の BP / MEM と同様に、接頭辞なしでも16進数とする。 */
static int mcp_parse_hex_u32(const char *text, uint32_t *out) {
    char *end;
    unsigned long value;
    if (!text[0])
        return -1;
    value = strtoul(text, &end, 16);
    if (*end != '\0' || value > UINT32_MAX)
        return -1;
    *out = (uint32_t)value;
    return 0;
}

static int mcp_parse_probe_value(const char *text, mcp_text_probe_def_t *def) {
    if ((text[0] == 'D' || text[0] == 'A') && text[1] >= '0' && text[1] <= '7' &&
        text[2] == '\0') {
        def->value_source = text[0] == 'D' ? MCP_TEXT_PROBE_DREG : MCP_TEXT_PROBE_AREG;
        def->value_index = (uint8_t)(text[1] - '0');
        return 0;
    }
    if (text[0] == 'S' && (text[1] == '+' || text[1] == '-')) {
        char *end;
        unsigned long offset = strtoul(text + 2, &end, 16);
        if (text[2] == '\0' || *end != '\0' || offset > INT32_MAX)
            return -1;
        def->value_source = MCP_TEXT_PROBE_STACK;
        def->stack_offset = text[1] == '-' ? -(int32_t)offset : (int32_t)offset;
        return 0;
    }
    return -1;
}

#define MCP_PROBE_SPACE_SUPER \
    "SUPER" /* ADD の任意末尾トークン: スーパーバイザ実行中のヒットだけ採取 */

static void mcp_cmd_text_probe(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)bus;
    (void)ctx;
    char operation[16];
    args = mcp_next_token(args, operation, sizeof(operation));
    if (strcmp(operation, "STATUS") == 0) {
        if (args[0]) {
            mcp_err(MCP_ERR_BADARG, "TEXT PROBE STATUS takes no arguments");
            return;
        }
        char reply[64];
        int n = snprintf(reply, sizeof(reply), "OK COUNT=%zu ENABLED=%d\n", s_text_probe_count,
                         mcp_text_probe_is_enabled());
        mcp_reply(reply, (size_t)n);
        return;
    }
    if (strcmp(operation, "CLEAR") == 0) {
        if (args[0]) {
            mcp_err(MCP_ERR_BADARG, "TEXT PROBE CLEAR takes no arguments");
            return;
        }
        mcp_text_probe_disable(s_app.cpu);
        memset(s_text_probe_defs, 0, sizeof(s_text_probe_defs));
        s_text_probe_count = 0;
        mcp_ok();
        return;
    }
    if (strcmp(operation, "ENABLE") == 0) {
        if (args[0]) {
            mcp_err(MCP_ERR_BADARG, "TEXT PROBE ENABLE takes no arguments");
            return;
        }
        if (!s_app.cpu) {
            mcp_err(MCP_ERR_NOCPU, "no cpu");
            return;
        }
        if (s_text_probe_count == 0) {
            mcp_err(MCP_ERR_NOTREADY, "no probe definitions");
            return;
        }
        if (mcp_perf_owns_probe()) {
            mcp_err(MCP_ERR_NOTREADY, "entry probe is owned by PERF");
            return;
        }
        if (mcp_text_probe_enable(s_app.cpu, s_text_probe_defs, s_text_probe_count) != 0) {
            mcp_err(MCP_ERR_BADARG, "invalid probe definitions");
            return;
        }
        mcp_ok();
        return;
    }
    if (strcmp(operation, "ADD") != 0) {
        mcp_err(MCP_ERR_BADARG, "TEXT PROBE <ADD|CLEAR|ENABLE|STATUS>");
        return;
    }
    if (mcp_text_probe_is_enabled()) {
        mcp_err(MCP_ERR_NOTREADY, "clear probes before adding definitions");
        return;
    }
    if (s_text_probe_count == MC68K_MAX_ENTRY_PROBES) {
        mcp_err(MCP_ERR_FULL, "too many text probes");
        return;
    }
    char pc_text[16], action[16], value[24], size_text[16], source[32], probe[32], encoding[16],
        space[16], extra[2];
    args = mcp_next_token(args, pc_text, sizeof(pc_text));
    args = mcp_next_token(args, action, sizeof(action));
    args = mcp_next_token(args, value, sizeof(value));
    args = mcp_next_token(args, size_text, sizeof(size_text));
    args = mcp_next_token(args, source, sizeof(source));
    args = mcp_next_token(args, probe, sizeof(probe));
    args = mcp_next_token(args, encoding, sizeof(encoding));
    args = mcp_next_token(args, space, sizeof(space));
    mcp_next_token(args, extra, sizeof(extra));
    mcp_text_probe_def_t def = {.source = NULL,
                                .probe = NULL,
                                .encoding = NULL,
                                .x = MCP_TEXT_EVENT_UNSPECIFIED,
                                .y = MCP_TEXT_EVENT_UNSPECIFIED,
                                .attr = MCP_TEXT_EVENT_UNSPECIFIED};
    uint32_t size;
    def.supervisor_only = strcmp(space, MCP_PROBE_SPACE_SUPER) == 0;
    if (!pc_text[0] || !action[0] || !value[0] || !size_text[0] || !source[0] || !probe[0] ||
        !encoding[0] || extra[0] || (space[0] && strcmp(space, MCP_PROBE_SPACE_SUPER) != 0) ||
        mcp_parse_hex_u32(pc_text, &def.pc) != 0 || (def.pc & 1u) ||
        mcp_parse_u32(size_text, &size) != 0 || mcp_parse_probe_value(value, &def) != 0) {
        mcp_err(
            MCP_ERR_BADARG,
            "ADD <pc> <STRING|CHAR|CURSOR> <D0|A0|S+off> <size> <source> <probe> <encoding> "
            "[SUPER]");
        return;
    }
    if (strcmp(action, "STRING") == 0) {
        def.action = MCP_TEXT_PROBE_EMIT_STRING;
        def.max_length = size;
        def.value_width = 4;
    } else if (strcmp(action, "CHAR") == 0) {
        def.action = MCP_TEXT_PROBE_EMIT_CHAR;
        def.value_width = (uint8_t)size;
    } else if (strcmp(action, "CURSOR") == 0 && size == 0) {
        def.action = MCP_TEXT_PROBE_SET_CURSOR;
    } else {
        mcp_err(MCP_ERR_BADARG, "invalid action or size");
        return;
    }
    /* 文字列は静的な登録領域へ複写し、ワイヤ受信バッファの寿命に依存させない。 */
    static char sources[MC68K_MAX_ENTRY_PROBES][32], probes[MC68K_MAX_ENTRY_PROBES][32],
        encodings[MC68K_MAX_ENTRY_PROBES][16];
    size_t slot = s_text_probe_count;
    strcpy(sources[slot], source);
    strcpy(probes[slot], probe);
    strcpy(encodings[slot], encoding);
    def.source = sources[slot];
    def.probe = probes[slot];
    def.encoding = encodings[slot];
    s_text_probe_defs[s_text_probe_count++] = def;
    mcp_ok();
}

static void mcp_cmd_text_bitmap(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)ctx;
    (void)args;
    int cols = s_app.native_w / 8;
    int rows = s_app.native_h / 16;
    int bufsz = cols * rows + rows + 4;
    uint8_t *text = malloc((size_t)bufsz);
    if (!text) {
        mcp_err(MCP_ERR_OOM, "tvram text buffer");
        return;
    }
    int text_len = x68k_bus_tvram_to_text(bus, cols, rows, text, bufsz);
    if (text_len < 0)
        text_len = 0;
    mcp_reply_bin(text, (size_t)text_len);
    free(text);
}

static void mcp_cmd_screenshot(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)ctx;
    int mask = 0;
    if (args[0] != '\0') {
        char *endp;
        long m = strtol(args, &endp, 10);
        if (!endp || *endp != '\0' || m < 1 || m > 7) {
            mcp_err(MCP_ERR_BADARG, "mask must be 1-7");
            return;
        }
        mask = (int)m;
    }
    int total;
    uint8_t *buf = mcp_screenshot_make_ppm(bus, mask, &total);
    if (!buf) {
        mcp_err(MCP_ERR_OOM, "screenshot buffer");
        return;
    }
    mcp_reply_bin(buf, (size_t)total);
    free(buf);
}

/* ---- PERF（性能計測） --------------------------------------- */

#define MCP_PERF_NA "NA"

/* 64-bitサイクル値は10進ASCIIで返す（クライアント側の53-bit整数化を避ける）。 */
static int mcp_perf_format_status(char *buf, size_t bufsz) {
    mcp_perf_status_t st;
    mcp_perf_get_status(&st);
    char cpu_start[24] = MCP_PERF_NA, cpu_end[24] = MCP_PERF_NA, cpu_delta[24] = MCP_PERF_NA;
    char el_start[24] = MCP_PERF_NA, el_end[24] = MCP_PERF_NA, el_delta[24] = MCP_PERF_NA;
    if (st.state == MCP_PERF_RUNNING || st.state == MCP_PERF_COMPLETE) {
        snprintf(cpu_start, sizeof(cpu_start), "%llu", (unsigned long long)st.start.cpu);
        snprintf(el_start, sizeof(el_start), "%llu", (unsigned long long)st.start.elapsed);
    }
    if (st.state == MCP_PERF_COMPLETE) {
        snprintf(cpu_end, sizeof(cpu_end), "%llu", (unsigned long long)st.end.cpu);
        snprintf(el_end, sizeof(el_end), "%llu", (unsigned long long)st.end.elapsed);
        snprintf(cpu_delta, sizeof(cpu_delta), "%llu",
                 (unsigned long long)(st.end.cpu - st.start.cpu));
        snprintf(el_delta, sizeof(el_delta), "%llu",
                 (unsigned long long)(st.end.elapsed - st.start.elapsed));
    }
    return snprintf(buf, bufsz,
                    "OK SEQ=%llu STATE=%s START_PC=%06X END_PC=%06X CPU_START=%s CPU_END=%s "
                    "CPU_DELTA=%s ELAPSED_START=%s ELAPSED_END=%s ELAPSED_DELTA=%s "
                    "CPU_TOTAL=%llu ELAPSED_TOTAL=%llu\n",
                    (unsigned long long)st.sequence, mcp_perf_state_name(st.state),
                    (unsigned)st.start_pc, (unsigned)st.end_pc, cpu_start, cpu_end, cpu_delta,
                    el_start, el_end, el_delta, (unsigned long long)st.total.cpu,
                    (unsigned long long)st.total.elapsed);
}

static void mcp_perf_send_status(void) {
    char reply[384];
    int n = mcp_perf_format_status(reply, sizeof(reply));
    if (n < 0 || (size_t)n >= sizeof(reply)) {
        mcp_err(MCP_ERR_INTERNAL, "perf status truncated");
        return;
    }
    mcp_reply(reply, (size_t)n);
}

/* 16進アドレス1個を厳密に読む（後続ゴミ・空文字は拒否）。 */
static int mcp_perf_parse_pc(const char *text, uint32_t *out) {
    if (!text[0])
        return -1;
    char *endp;
    unsigned long value = strtoul(text, &endp, 16);
    if (*endp != '\0' || value > 0xFFFFFFu)
        return -1;
    *out = (uint32_t)value;
    return 0;
}

static void mcp_cmd_perf_arm(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)bus;
    (void)ctx;
    char start_text[16], end_text[16], extra[16];
    args = mcp_next_token(args, start_text, sizeof(start_text));
    args = mcp_next_token(args, end_text, sizeof(end_text));
    mcp_next_token(args, extra, sizeof(extra));
    uint32_t start_pc, end_pc;
    if (extra[0] || mcp_perf_parse_pc(start_text, &start_pc) != 0 ||
        mcp_perf_parse_pc(end_text, &end_pc) != 0) {
        mcp_err(MCP_ERR_BADARG, "PERF ARM <start_pc> <end_pc> (hex, 24-bit)");
        return;
    }
    if (!s_app.cpu) {
        mcp_err(MCP_ERR_NOCPU, "no cpu");
        return;
    }
    if (mcp_text_probe_is_enabled()) {
        mcp_err(MCP_ERR_NOTREADY, "entry probe is owned by TEXT PROBE");
        return;
    }
    if (dbg_next_exec_uses_debug_run()) {
        mcp_err(MCP_ERR_NOTREADY,
                "debug-run is active (DBG MODE ON, breakpoint, watchpoint or exception break)");
        return;
    }
    int rc = mcp_perf_arm(s_app.cpu, start_pc, end_pc);
    if (rc == MCP_PERF_ARM_BADARG) {
        mcp_err(MCP_ERR_BADARG, "start_pc and end_pc must be distinct even addresses");
        return;
    }
    if (rc != 0) {
        mcp_err(MCP_ERR_NOTREADY, "cannot register entry probes");
        return;
    }
    mcp_perf_send_status();
}

static void mcp_cmd_perf_status(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)bus;
    (void)ctx;
    if (args[0]) {
        mcp_err(MCP_ERR_BADARG, "PERF STATUS takes no arguments");
        return;
    }
    mcp_perf_send_status();
}

static void mcp_cmd_perf_clear(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)bus;
    (void)ctx;
    if (args[0]) {
        mcp_err(MCP_ERR_BADARG, "PERF CLEAR takes no arguments");
        return;
    }
    mcp_perf_clear();
    mcp_ok();
}

/* ---- BP / WP / EXC ----------------------------------------------------- */

static void mcp_cmd_bp_add(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)bus;
    (void)ctx;
    char *endp;
    uint32_t addr = (uint32_t)strtoul(args, &endp, 16);
    int id = dbg_bp_add(addr, endp && *endp == ' ' ? endp + 1 : "");
    if (id >= 0) {
        char reply[32];
        int len = snprintf(reply, sizeof(reply), "OK %d\n", id);
        mcp_reply(reply, (size_t)len);
    } else if (id == -2)
        mcp_err(MCP_ERR_BADARG, "bad condition");
    else if (id == -3)
        mcp_err(MCP_ERR_BADARG, "odd address");
    else
        mcp_err(MCP_ERR_FULL, "breakpoint table full");
}

static void mcp_cmd_bp_del(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)bus;
    (void)ctx;
    dbg_bp_del((int)strtol(args, NULL, 10));
    mcp_ok();
}

static void mcp_cmd_bp_enable(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)bus;
    (void)ctx;
    char *endp;
    long id = strtol(args, &endp, 10);
    long enabled = endp && *endp == ' ' ? strtol(endp + 1, NULL, 10) : 1;
    dbg_bp_set_enable((int)id, enabled != 0);
    mcp_ok();
}

static void mcp_cmd_bp_clear(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)bus;
    (void)ctx;
    (void)args;
    dbg_bp_clear();
    mcp_ok();
}

static void mcp_cmd_bp_list(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)bus;
    (void)ctx;
    (void)args;
    char list[1024], reply[1056];
    int count, len = dbg_bp_format_list(list, sizeof(list), &count);
    int reply_len = snprintf(reply, sizeof(reply), "OK %d %.*s\n", count, len, list);
    mcp_reply(reply, (size_t)reply_len);
}

static void mcp_cmd_wp_add(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)bus;
    (void)ctx;
    char *endp;
    uint32_t addr = (uint32_t)strtoul(args, &endp, 16);
    int size = endp && *endp == ' ' ? (int)strtol(endp + 1, NULL, 10) : 1;
    int id = dbg_wp_add(addr, size);
    if (id >= 0) {
        char reply[32];
        int len = snprintf(reply, sizeof(reply), "OK %d\n", id);
        mcp_reply(reply, (size_t)len);
    } else if (id == -2)
        mcp_err(MCP_ERR_BADARG, "size must be 1, 2 or 4");
    else
        mcp_err(MCP_ERR_NOTREADY, "no peek hooks");
}

static void mcp_cmd_wp_del(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)bus;
    (void)ctx;
    dbg_wp_del((int)strtol(args, NULL, 10));
    mcp_ok();
}

static void mcp_cmd_wp_enable(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)bus;
    (void)ctx;
    char *endp;
    long id = strtol(args, &endp, 10);
    long enabled = endp && *endp == ' ' ? strtol(endp + 1, NULL, 10) : 1;
    dbg_wp_set_enable((int)id, enabled != 0);
    mcp_ok();
}

static void mcp_cmd_wp_clear(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)bus;
    (void)ctx;
    (void)args;
    dbg_wp_clear();
    mcp_ok();
}

static void mcp_cmd_wp_list(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)bus;
    (void)ctx;
    (void)args;
    char list[512], reply[544];
    int count, len = dbg_wp_format_list(list, sizeof(list), &count);
    int reply_len = snprintf(reply, sizeof(reply), "OK %d %.*s\n", count, len, list);
    mcp_reply(reply, (size_t)reply_len);
}

static void mcp_cmd_exc_set(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)bus;
    (void)ctx;
    dbg_set_exc_mask((uint64_t)strtoull(args, NULL, 16));
    mcp_ok();
}

static void mcp_cmd_exc_get(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)bus;
    (void)ctx;
    (void)args;
    char reply[32];
    int len =
        snprintf(reply, sizeof(reply), "OK %016llX\n", (unsigned long long)dbg_get_exc_mask());
    mcp_reply(reply, (size_t)len);
}

/* ---- DBG（実行制御） --------------------------------------------------- */

static void mcp_cmd_dbg_status(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)bus;
    (void)ctx;
    (void)args;
    mcp_send_stopinfo();
}

static void mcp_cmd_dbg_cont(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)bus;
    (void)ctx;
    (void)args;
    dbg_continue();
    mcp_ok();
}

static void mcp_cmd_dbg_step(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)ctx;
    if (!s_app.cpu) {
        mcp_err(MCP_ERR_NOCPU, "no cpu");
        return;
    }
    long count = args[0] ? strtol(args, NULL, 10) : 1;
    dbg_step(bus, (int)count);
    mcp_send_stopinfo();
}

static void mcp_cmd_dbg_break(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)bus;
    (void)ctx;
    (void)args;
    dbg_request_break();
    mcp_send_stopinfo();
}

static void mcp_cmd_dbg_mode(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)bus;
    (void)ctx;
    if (strcmp(args, "ON") == 0)
        dbg_set_mode(DBG_MODE_ON);
    else if (strcmp(args, "OFF") == 0)
        dbg_set_mode(DBG_MODE_OFF);
    else if (strcmp(args, "AUTO") == 0)
        dbg_set_mode(DBG_MODE_AUTO);
    else {
        mcp_err(MCP_ERR_BADARG, "mode must be ON, OFF or AUTO");
        return;
    }
    mcp_ok();
}

/* ---- SNAPSHOT --------------------------------------------------------- */

static void mcp_snapshot_error(checkpoint_result_t result) {
    if (result == CHECKPOINT_NOCPU) {
        mcp_err(MCP_ERR_NOCPU, "no cpu");
        return;
    }
    if (result == CHECKPOINT_OOM) {
        mcp_err(MCP_ERR_OOM, "snapshot");
        return;
    }
    const char *reason = checkpoint_result_reason(result);
    const char *detail = checkpoint_result_detail(result);
    if (detail) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%s %s", reason, detail);
        mcp_err(MCP_ERR_NOTREADY, msg);
        return;
    }
    mcp_err(MCP_ERR_NOTREADY, reason);
}

static void mcp_cmd_snapshot_save(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)ctx;
    if (args[0] != '\0') {
        mcp_err(MCP_ERR_BADARG, "SNAPSHOT SAVE");
        return;
    }
    checkpoint_result_t result = checkpoint_save(bus);
    if (result == CHECKPOINT_OK)
        mcp_reply("OK SLOT=VALID\n", 14);
    else
        mcp_snapshot_error(result);
}

static void mcp_cmd_snapshot_load(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    if (args[0] != '\0') {
        mcp_err(MCP_ERR_BADARG, "SNAPSHOT LOAD");
        return;
    }
    checkpoint_result_t result = checkpoint_load(bus);
    if (result != CHECKPOINT_OK) {
        mcp_snapshot_error(result);
        return;
    }
    ctx->output_len = 0;
    ctx->wait_kind = MCP_WAIT_NONE;
    mcp_send_stopinfo();
}

static void mcp_cmd_snapshot_status(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)ctx;
    if (args[0] != '\0') {
        mcp_err(MCP_ERR_BADARG, "SNAPSHOT STATUS");
        return;
    }
    if (!s_app.cpu) {
        mcp_err(MCP_ERR_NOCPU, "no cpu");
        return;
    }
    const char *reason;
    switch (checkpoint_slot_state(bus, &reason)) {
    case CHECKPOINT_SLOT_EMPTY:
        mcp_reply("OK SLOT=EMPTY\n", 14);
        return;
    case CHECKPOINT_SLOT_VALID:
        mcp_reply("OK SLOT=VALID\n", 14);
        return;
    case CHECKPOINT_SLOT_INVALID_STATE:
        mcp_reply("OK SLOT=INVALID REASON=MEDIA_CHANGED\n", 37);
        return;
    }
}

/* ---- 非同期待ち（KEY WAIT / DBG WAIT） --------------------------------- */

static int mcp_wait_keyin_done(void) {
    return s_app.key_input_waiting != 0;
}
static void mcp_wait_keyin_reply(void) {
    mcp_reply("OK 1\n", 5);
}
static int mcp_wait_dbg_done(void) {
    return dbg_is_paused();
}
static void mcp_wait_dbg_reply(void) {
    mcp_send_stopinfo();
}

/* 待ちの種類ごとの「完了判定」と「完了時の応答」。新しい待ちを足すときは
 * mcp_wait_kind_t に1つ増やしてここに2関数を書くだけでよい。 */
static const struct {
    int (*is_done)(void);
    void (*reply)(void);
} s_wait_ops[MCP_WAIT_KIND_COUNT] = {
    [MCP_WAIT_NONE] = {NULL, NULL},
    [MCP_WAIT_KEYIN] = {mcp_wait_keyin_done, mcp_wait_keyin_reply},
    [MCP_WAIT_DBG] = {mcp_wait_dbg_done, mcp_wait_dbg_reply},
};

/* 完了していれば即応答、していなければ後続フレームのpollへ持ち越す。 */
static void mcp_wait_begin(mcp_context_t *ctx, mcp_wait_kind_t kind, const char *args) {
    if (s_wait_ops[kind].is_done()) {
        s_wait_ops[kind].reply();
        return;
    }
    uint32_t timeout_ms = (uint32_t)strtoul(args, NULL, 10);
    ctx->wait_deadline = pal_get_ticks_us() + (uint64_t)timeout_ms * 1000;
    ctx->wait_kind = kind;
}

static void mcp_cmd_key_wait(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)bus;
    mcp_wait_begin(ctx, MCP_WAIT_KEYIN, args);
}

static void mcp_cmd_dbg_wait(x68k_bus_t *bus, mcp_context_t *ctx, const char *args) {
    (void)bus;
    mcp_wait_begin(ctx, MCP_WAIT_DBG, args);
}

/* ---- グループ表 -------------------------------------------------------- */

#define MCP_GROUP(name, table) {name, table, sizeof(table) / sizeof((table)[0])}

static const mcp_action_entry_t s_act_hello[] = {{NULL, mcp_cmd_hello}};
static const mcp_action_entry_t s_act_reset[] = {{NULL, mcp_cmd_reset}};
static const mcp_action_entry_t s_act_interrupt[] = {{NULL, mcp_cmd_interrupt}};
static const mcp_action_entry_t s_act_power[] = {{"OFF", mcp_cmd_power_off},
                                                 {"ON", mcp_cmd_power_on}};
static const mcp_action_entry_t s_act_diag[] = {{NULL, mcp_cmd_diag}};
static const mcp_action_entry_t s_act_ring[] = {{NULL, mcp_cmd_ring}};
static const mcp_action_entry_t s_act_disasm[] = {{NULL, mcp_cmd_disasm}};
static const mcp_action_entry_t s_act_regs[] = {{NULL, mcp_cmd_regs}};
static const mcp_action_entry_t s_act_mmu[] = {{"GET", mcp_cmd_mmu_get}};
static const mcp_action_entry_t s_act_config[] = {{"GET", mcp_cmd_config_get}};
static const mcp_action_entry_t s_act_paste[] = {{NULL, mcp_cmd_paste}};
static const mcp_action_entry_t s_act_screenshot[] = {{NULL, mcp_cmd_screenshot}};

static const mcp_action_entry_t s_act_whiteband[] = {
    {"ON", mcp_cmd_whiteband_on},
    {"OFF", mcp_cmd_whiteband_off},
    {"CONT", mcp_cmd_whiteband_cont},
};
static const mcp_action_entry_t s_act_watch[] = {
    {"ON", mcp_cmd_watch_on},
    {"OFF", mcp_cmd_watch_off},
};
static const mcp_action_entry_t s_act_hostfile[] = {
    {"PUT", mcp_cmd_hostfile_put},
    {"GET", mcp_cmd_hostfile_get},
    {"STATUS", mcp_cmd_hostfile_status},
};
static const mcp_action_entry_t s_act_disk[] = {
    {"INSERT", mcp_cmd_disk_insert},
    {"EJECT", mcp_cmd_disk_eject},
    {"STATUS", mcp_cmd_disk_status},
    {"WP", mcp_cmd_disk_wp},
};
static const mcp_action_entry_t s_act_key[] = {
    {"DOWN", mcp_cmd_key_down},   {"UP", mcp_cmd_key_up},
    {"SCAN", mcp_cmd_key_scan},   {"CLEAR", mcp_cmd_key_clear},
    {"CHECK", mcp_cmd_key_check}, {"WAIT", mcp_cmd_key_wait},
    {"PASTE", mcp_cmd_key_paste}, {"KBDSTATE", mcp_cmd_key_kbdstate},
};
static const mcp_action_entry_t s_act_mouse[] = {
    {"MOVE", mcp_cmd_mouse_move},
    {"BUTTON", mcp_cmd_mouse_button},
};
static const mcp_action_entry_t s_act_joy[] = {
    {"SET", mcp_cmd_joy_set},
    {"RELEASE", mcp_cmd_joy_release},
};
static const mcp_action_entry_t s_act_mem[] = {
    {"READ", mcp_cmd_mem_read},
    {"PEEK", mcp_cmd_mem_peek},
    {"WRITE", mcp_cmd_mem_write},
};
static const mcp_action_entry_t s_act_text[] = {
    {"STDOUT", mcp_cmd_text_stdout},
    {"EVENTS", mcp_cmd_text_events},
    {"PROBE", mcp_cmd_text_probe},
    {"BITMAP", mcp_cmd_text_bitmap},
};
static const mcp_action_entry_t s_act_bp[] = {
    {"ADD", mcp_cmd_bp_add},     {"DEL", mcp_cmd_bp_del},   {"ENABLE", mcp_cmd_bp_enable},
    {"CLEAR", mcp_cmd_bp_clear}, {"LIST", mcp_cmd_bp_list},
};
static const mcp_action_entry_t s_act_wp[] = {
    {"ADD", mcp_cmd_wp_add},     {"DEL", mcp_cmd_wp_del},   {"ENABLE", mcp_cmd_wp_enable},
    {"CLEAR", mcp_cmd_wp_clear}, {"LIST", mcp_cmd_wp_list},
};
static const mcp_action_entry_t s_act_exc[] = {
    {"SET", mcp_cmd_exc_set},
    {"GET", mcp_cmd_exc_get},
};
static const mcp_action_entry_t s_act_perf[] = {
    {"ARM", mcp_cmd_perf_arm},
    {"STATUS", mcp_cmd_perf_status},
    {"CLEAR", mcp_cmd_perf_clear},
};
static const mcp_action_entry_t s_act_dbg[] = {
    {"STATUS", mcp_cmd_dbg_status}, {"CONT", mcp_cmd_dbg_cont}, {"STEP", mcp_cmd_dbg_step},
    {"BREAK", mcp_cmd_dbg_break},   {"MODE", mcp_cmd_dbg_mode}, {"WAIT", mcp_cmd_dbg_wait},
};
static const mcp_action_entry_t s_act_snapshot[] = {
    {"SAVE", mcp_cmd_snapshot_save},
    {"LOAD", mcp_cmd_snapshot_load},
    {"STATUS", mcp_cmd_snapshot_status},
};

static const mcp_group_entry_t s_groups[] = {
    MCP_GROUP("HELLO", s_act_hello),
    MCP_GROUP("RESET", s_act_reset),
    MCP_GROUP("INTERRUPT", s_act_interrupt),
    MCP_GROUP("POWER", s_act_power),
    MCP_GROUP("WHITEBAND", s_act_whiteband),
    MCP_GROUP("DIAG", s_act_diag),
    MCP_GROUP("RING", s_act_ring),
    MCP_GROUP("WATCH", s_act_watch),
    MCP_GROUP("MEM", s_act_mem),
    MCP_GROUP("REGS", s_act_regs),
    MCP_GROUP("MMU", s_act_mmu),
    MCP_GROUP("CONFIG", s_act_config),
    MCP_GROUP("DISASM", s_act_disasm),
    MCP_GROUP("BP", s_act_bp),
    MCP_GROUP("WP", s_act_wp),
    MCP_GROUP("DBG", s_act_dbg),
    MCP_GROUP("SNAPSHOT", s_act_snapshot),
    MCP_GROUP("EXC", s_act_exc),
    MCP_GROUP("DISK", s_act_disk),
    MCP_GROUP("HOSTFILE", s_act_hostfile),
    MCP_GROUP("KEY", s_act_key),
    MCP_GROUP("MOUSE", s_act_mouse),
    MCP_GROUP("JOY", s_act_joy),
    MCP_GROUP("PASTE", s_act_paste),
    MCP_GROUP("TEXT", s_act_text),
    MCP_GROUP("PERF", s_act_perf),
    MCP_GROUP("SCREENSHOT", s_act_screenshot),
};

int mcp_commands_handle(x68k_bus_t *bus, mcp_context_t *context, const char *line) {
    char group[16];
    const char *rest = mcp_next_token(line, group, sizeof(group));
    for (size_t i = 0; i < sizeof(s_groups) / sizeof(s_groups[0]); i++) {
        if (strcmp(group, s_groups[i].group) != 0)
            continue;
        const mcp_group_entry_t *g = &s_groups[i];
        if (g->actions[0].action == NULL) { /* アクションを取らないグループ */
            g->actions[0].handler(bus, context, rest);
            return 1;
        }
        char action[16];
        rest = mcp_next_token(rest, action, sizeof(action));
        for (size_t a = 0; a < g->count; a++) {
            if (strcmp(action, g->actions[a].action) == 0) {
                g->actions[a].handler(bus, context, rest);
                return 1;
            }
        }
        mcp_err(MCP_ERR_UNKNOWN, "unknown action for group");
        return 1;
    }
    return 0; /* 未知のグループ。応答は server.c が返す */
}

void mcp_commands_poll(x68k_bus_t *bus, mcp_context_t *context) {
    iocs_key_wait_poll(bus);
    if (context->wait_kind != MCP_WAIT_NONE) {
        if (s_wait_ops[context->wait_kind].is_done()) {
            s_wait_ops[context->wait_kind].reply();
            context->wait_kind = MCP_WAIT_NONE;
        } else if (pal_get_ticks_us() >= context->wait_deadline) {
            mcp_err(MCP_ERR_TIMEOUT, "wait deadline reached");
            context->wait_kind = MCP_WAIT_NONE;
        }
    }
    if (context->paste_idx < context->paste_len) {
        uint64_t now = pal_get_ticks_us();
        if (now >= context->paste_next_us) {
            pal_key_inject_ascii(context->paste_queue[context->paste_idx++]);
            if (context->paste_idx == context->paste_len)
                s_app.key_input_paste_active = 0;
            context->paste_next_us = now + 20000;
        }
    }
}

void mcp_commands_client_disconnected(mcp_context_t *context) {
    context->wait_kind = MCP_WAIT_NONE;
}
