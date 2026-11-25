#pragma once

#include <stddef.h>
#include <stdint.h>

#include "mc68000/cpu_types.h"
#include "mcp/text_event.h"

typedef enum {
    MCP_TEXT_PROBE_DREG,
    MCP_TEXT_PROBE_AREG,
    MCP_TEXT_PROBE_STACK,
} mcp_text_probe_value_source_t;

typedef enum {
    MCP_TEXT_PROBE_EMIT_STRING,
    MCP_TEXT_PROBE_EMIT_CHAR,
    MCP_TEXT_PROBE_SET_CURSOR,
} mcp_text_probe_action_t;

/* Pythonプロファイル側で検証・解決した後にだけ渡す低レベル定義。 */
typedef struct {
    uint32_t pc;
    const char *source;
    const char *probe;
    const char *encoding;
    mcp_text_probe_action_t action;
    mcp_text_probe_value_source_t value_source;
    uint8_t value_index; /* Dn/An番号。STACKでは未使用 */
    int32_t stack_offset;
    uint8_t value_width; /* CHARは1/2/4、STRINGのポインタは4 */
    uint32_t max_length; /* STRINGの必須上限 */
    int32_t x, y, attr;  /* 未指定はMCP_TEXT_EVENT_UNSPECIFIED */
    int supervisor_only; /* 非0ならSR.S=1のヒットだけ採取（同じ論理アドレスを実行するユーザープロセスの誤発火を除く）
                          */
} mcp_text_probe_def_t;

int mcp_text_probe_enable(mc68k_state_t *cpu, const mcp_text_probe_def_t *defs, size_t count);
void mcp_text_probe_disable(mc68k_state_t *cpu);
int mcp_text_probe_is_enabled(void);
