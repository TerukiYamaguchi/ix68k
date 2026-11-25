#include "mcp/text_probe.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "mc68000/cpu.h"
#include "mc68000/debug.h"
#include "memory/bus.h"
#include "memory/memory.h"

#define RAM_SIZE 0x10000u
#define PC       0x1000u
#define SSP      0x8000u

static void put16(uint8_t *ram, uint32_t addr, uint16_t value) {
    ram[addr] = (uint8_t)(value >> 8);
    ram[addr + 1] = (uint8_t)value;
}
static void put32(uint8_t *ram, uint32_t addr, uint32_t value) {
    ram[addr] = (uint8_t)(value >> 24);
    ram[addr + 1] = (uint8_t)(value >> 16);
    ram[addr + 2] = (uint8_t)(value >> 8);
    ram[addr + 3] = (uint8_t)value;
}
static uint8_t peek8(void *ctx, uint32_t addr) { return ((uint8_t *)ctx)[addr & 0xFFFFu]; }
static uint16_t peek16(void *ctx, uint32_t addr) { return (uint16_t)((peek8(ctx, addr) << 8) | peek8(ctx, addr + 1)); }
static uint32_t peek32(void *ctx, uint32_t addr) { return ((uint32_t)peek16(ctx, addr) << 16) | peek16(ctx, addr + 2); }

static mc68k_state_t *make_cpu(uint8_t *ram) {
    mem_ctx_t *mem = mem_create();
    mem_region_t *region = calloc(1, sizeof(*region));
    assert(region);
    region->data = ram;
    region->size = RAM_SIZE;
    mem_add_region(mem, region);
    mc68k_bus_t bus = bus_make_hooks(mem);
    mc68k_state_t *cpu = mc68k_create(&bus);
    assert(cpu);
    mc68k_set_peek_hooks(cpu, peek8, peek16, peek32, ram);
    return cpu;
}

static void test_dreg_string_and_stack_char(void) {
    uint8_t ram[RAM_SIZE] = {0};
    put32(ram, 0, SSP);
    put32(ram, 4, PC);
    put16(ram, PC, 0x4E71); /* NOP */
    put16(ram, PC + 2, 0x4E71);
    memcpy(ram + 0x200, "text", 5);
    put32(ram, SSP, 0x00000041u);
    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_set_dreg(cpu, 0, 0x200);
    const mcp_text_probe_def_t defs[] = {
        {.pc = PC, .source = "title", .probe = "puts", .encoding = "shift-jis",
         .action = MCP_TEXT_PROBE_EMIT_STRING, .value_source = MCP_TEXT_PROBE_DREG,
         .value_index = 0, .max_length = 32, .x = 1, .y = 2, .attr = 3},
        {.pc = PC + 2, .source = "kernel", .probe = "putc", .encoding = "raw",
         .action = MCP_TEXT_PROBE_EMIT_CHAR, .value_source = MCP_TEXT_PROBE_STACK,
         .value_width = 1, .x = MCP_TEXT_EVENT_UNSPECIFIED,
         .y = MCP_TEXT_EVENT_UNSPECIFIED, .attr = MCP_TEXT_EVENT_UNSPECIFIED},
    };
    mcp_text_events_reset();
    assert(mcp_text_probe_enable(cpu, defs, 2) == 0);
    mc68k_execute(cpu, 8);
    mcp_text_event_t event;
    uint8_t payload[32];
    assert(mcp_text_event_get(0, &event, payload, sizeof(payload)) == 0);
    assert(event.payload_len == 4 && memcmp(payload, "text", 4) == 0 && event.x == 1);
    assert(mcp_text_event_get(1, &event, payload, sizeof(payload)) == 0);
    assert(event.payload_len == 1 && payload[0] == 'A');
    mcp_text_probe_disable(cpu);
    assert(!mcp_text_probe_is_enabled());
    mc68k_destroy(cpu);
}

/* supervisor_only のプローブは、ユーザーモード中に同じPCを実行しても採取しない。 */
static void test_supervisor_only_filter(void) {
    uint8_t ram[RAM_SIZE] = {0};
    put32(ram, 0, SSP);
    put32(ram, 4, PC);
    put16(ram, PC, 0x46FC);     /* MOVE #0,SR（ユーザーモードへ） */
    put16(ram, PC + 2, 0x0000);
    put16(ram, PC + 4, 0x4E71); /* NOP: supervisor_only */
    put16(ram, PC + 6, 0x4E71); /* NOP: モードを問わない */
    put16(ram, PC + 8, 0x4E71);
    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mc68k_set_dreg(cpu, 0, 0x41);
    const mcp_text_probe_def_t defs[] = {
        {.pc = PC + 4, .source = "kernel", .probe = "super", .encoding = "raw",
         .action = MCP_TEXT_PROBE_EMIT_CHAR, .value_source = MCP_TEXT_PROBE_DREG,
         .value_width = 1, .x = MCP_TEXT_EVENT_UNSPECIFIED, .y = MCP_TEXT_EVENT_UNSPECIFIED,
         .attr = MCP_TEXT_EVENT_UNSPECIFIED, .supervisor_only = 1},
        {.pc = PC + 6, .source = "user", .probe = "any", .encoding = "raw",
         .action = MCP_TEXT_PROBE_EMIT_CHAR, .value_source = MCP_TEXT_PROBE_DREG,
         .value_width = 1, .x = MCP_TEXT_EVENT_UNSPECIFIED, .y = MCP_TEXT_EVENT_UNSPECIFIED,
         .attr = MCP_TEXT_EVENT_UNSPECIFIED},
    };
    mcp_text_events_reset();
    assert(mcp_text_probe_enable(cpu, defs, 2) == 0);
    mc68k_execute(cpu, 40);
    mcp_text_event_t event;
    uint8_t payload[4];
    assert(mcp_text_event_get(0, &event, payload, sizeof(payload)) == 0);
    assert(event.payload_len == 1 && strcmp(event.probe, "any") == 0);
    assert(mcp_text_event_get(1, &event, payload, sizeof(payload)) != 0);
    mcp_text_probe_disable(cpu);

    /* スーパーバイザ実行中は採取する。 */
    mc68k_reset(cpu);
    mc68k_set_dreg(cpu, 0, 0x42);
    put16(ram, PC, 0x4E71);
    put16(ram, PC + 2, 0x4E71);
    mcp_text_events_reset();
    assert(mcp_text_probe_enable(cpu, defs, 2) == 0);
    mc68k_execute(cpu, 40);
    assert(mcp_text_event_get(0, &event, payload, sizeof(payload)) == 0);
    assert(strcmp(event.probe, "super") == 0 && payload[0] == 0x42);
    mcp_text_probe_disable(cpu);
    mc68k_destroy(cpu);
}

int main(void) {
    test_dreg_string_and_stack_char();
    test_supervisor_only_filter();
}
