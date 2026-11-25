#include "mcp/text_probe.h"

#include "mc68000/cpu.h"
#include "mc68000/debug.h"
#include <string.h>

#define MC68K_SR_SUPERVISOR 0x2000u /* SR の S ビット */

typedef struct {
    mc68k_state_t *cpu;
    mcp_text_probe_def_t defs[MC68K_MAX_ENTRY_PROBES];
    size_t count;
    int enabled;
} text_probe_state_t;

static text_probe_state_t s_state;

static int read_value(mc68k_state_t *cpu, const mcp_text_probe_def_t *def, uint32_t *out) {
    mc68k_regs_t regs;
    mc68k_get_regs(cpu, &regs);
    if (def->value_source == MCP_TEXT_PROBE_DREG) {
        *out = regs.d[def->value_index];
        return 0;
    }
    if (def->value_source == MCP_TEXT_PROBE_AREG) {
        *out = regs.a[def->value_index];
        return 0;
    }
    uint8_t bytes[4];
    uint32_t addr = regs.a[7] + (uint32_t)def->stack_offset;
    if (mc68k_debug_read_bytes(cpu, addr, bytes, sizeof(bytes)) != 0)
        return -1;
    *out = ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) | ((uint32_t)bytes[2] << 8) |
           bytes[3];
    return 0;
}

static void text_probe_hit(mc68k_state_t *cpu, unsigned id, uint32_t pc, void *ctx) {
    text_probe_state_t *state = ctx;
    if (!state || id >= state->count)
        return;
    const mcp_text_probe_def_t *def = &state->defs[id];
    if (def->supervisor_only) {
        mc68k_regs_t regs;
        mc68k_get_regs(cpu, &regs);
        if (!(regs.sr & MC68K_SR_SUPERVISOR))
            return;
    }
    uint32_t value;
    if (read_value(cpu, def, &value) != 0) {
        (void)mcp_text_event_emit(pc, def->source, def->probe, def->encoding, def->x, def->y,
                                  def->attr, MCP_TEXT_EVENT_READ_ERROR, NULL, 0);
        return;
    }
    if (def->action == MCP_TEXT_PROBE_EMIT_STRING) {
        (void)mcp_text_event_capture_cstring(cpu, pc, def->source, def->probe, def->encoding,
                                             def->x, def->y, def->attr, value, def->max_length);
    } else if (def->action == MCP_TEXT_PROBE_EMIT_CHAR) {
        uint8_t bytes[4];
        for (unsigned i = 0; i < def->value_width; ++i)
            bytes[i] = (uint8_t)(value >> ((def->value_width - 1u - i) * 8u));
        (void)mcp_text_event_emit(pc, def->source, def->probe, def->encoding, def->x, def->y,
                                  def->attr, 0, bytes, def->value_width);
    } else {
        (void)mcp_text_event_emit(pc, def->source, def->probe, def->encoding, def->x, def->y,
                                  def->attr, 0, NULL, 0);
    }
}

int mcp_text_probe_enable(mc68k_state_t *cpu, const mcp_text_probe_def_t *defs, size_t count) {
    if (!cpu || !defs || count == 0 || count > MC68K_MAX_ENTRY_PROBES)
        return -1;
    uint32_t pcs[MC68K_MAX_ENTRY_PROBES];
    for (size_t i = 0; i < count; ++i) {
        const mcp_text_probe_def_t *def = &defs[i];
        if ((def->pc & 1u) || def->value_source > MCP_TEXT_PROBE_STACK ||
            (def->value_source != MCP_TEXT_PROBE_STACK && def->value_index > 7) ||
            def->action > MCP_TEXT_PROBE_SET_CURSOR ||
            (def->action == MCP_TEXT_PROBE_EMIT_STRING &&
             (def->max_length == 0 || def->max_length > MCP_TEXT_EVENT_MAX_PAYLOAD)) ||
            (def->action == MCP_TEXT_PROBE_EMIT_CHAR && def->value_width != 1 &&
             def->value_width != 2 && def->value_width != 4))
            return -1;
        pcs[i] = def->pc;
    }
    text_probe_state_t next = {.cpu = cpu, .count = count, .enabled = 1};
    memcpy(next.defs, defs, count * sizeof(*defs));
    if (mc68k_set_entry_probes(cpu, pcs, count, text_probe_hit, &s_state) != 0)
        return -1;
    s_state = next;
    return 0;
}

void mcp_text_probe_disable(mc68k_state_t *cpu) {
    if (s_state.enabled && (!cpu || cpu == s_state.cpu))
        mc68k_clear_entry_probes(s_state.cpu);
    memset(&s_state, 0, sizeof(s_state));
}

int mcp_text_probe_is_enabled(void) {
    return s_state.enabled;
}
