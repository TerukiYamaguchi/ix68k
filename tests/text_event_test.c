#include "mcp/text_event.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "mc68000/cpu.h"
#include "mc68000/debug.h"
#include "memory/bus.h"
#include "memory/memory.h"

#define RAM_SIZE 0x10000u

static uint8_t peek8(void *ctx, uint32_t addr) {
    return ((const uint8_t *)ctx)[addr & 0xFFFFu];
}

static uint16_t peek16(void *ctx, uint32_t addr) {
    return (uint16_t)((peek8(ctx, addr) << 8) | peek8(ctx, addr + 1));
}

static uint32_t peek32(void *ctx, uint32_t addr) {
    return ((uint32_t)peek16(ctx, addr) << 16) | peek16(ctx, addr + 2);
}

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

static void test_order_and_reset_epoch(void) {
    mcp_text_events_reset();
    mcp_text_event_status_t before = mcp_text_events_status();
    static const uint8_t first[] = {0x82, 0xA0};
    static const uint8_t second[] = {'o', 'k'};
    assert(mcp_text_event_emit(0x1234, "iocs", "B_PRINT", "shift-jis", 1, 2, 3, 0, first,
                               sizeof(first)) == 0);
    assert(mcp_text_event_emit(0x1236, "title", "puts", "raw", MCP_TEXT_EVENT_UNSPECIFIED,
                               MCP_TEXT_EVENT_UNSPECIFIED, MCP_TEXT_EVENT_UNSPECIFIED,
                               MCP_TEXT_EVENT_TRUNCATED, second, sizeof(second)) == 0);
    mcp_text_event_status_t status = mcp_text_events_status();
    assert(status.count == 2 && status.epoch == before.epoch);

    mcp_text_event_t event;
    uint8_t payload[8];
    assert(mcp_text_event_get(0, &event, payload, sizeof(payload)) == 0);
    assert(event.pc == 0x1234 && event.payload_len == sizeof(first));
    assert(strcmp(event.source, "iocs") == 0 && memcmp(payload, first, sizeof(first)) == 0);
    assert(mcp_text_event_get(1, &event, payload, sizeof(payload)) == 0);
    assert(event.flags == MCP_TEXT_EVENT_TRUNCATED && memcmp(payload, second, sizeof(second)) == 0);

    mcp_text_events_discard(1);
    status = mcp_text_events_status();
    assert(status.count == 1 && status.dropped == 0);

    mcp_text_events_reset();
    status = mcp_text_events_status();
    assert(status.count == 0 && status.epoch == before.epoch + 1);
}

static void test_overflow_discards_complete_events(void) {
    mcp_text_events_reset();
    uint8_t payload[MCP_TEXT_EVENT_MAX_PAYLOAD];
    memset(payload, 0x5A, sizeof(payload));
    for (unsigned i = 0; i < MCP_TEXT_EVENT_MAX; ++i)
        assert(mcp_text_event_emit(i, "test", "event", "raw", 0, 0, 0, 0, payload,
                                   sizeof(payload)) == 0);
    assert(mcp_text_event_emit(0xFFFF, "test", "last", "raw", 0, 0, 0, 0, payload,
                               sizeof(payload)) == 0);
    mcp_text_event_status_t status = mcp_text_events_status();
    assert(status.count == MCP_TEXT_EVENT_BYTES / MCP_TEXT_EVENT_MAX_PAYLOAD);
    assert(status.dropped >= 1);
    mcp_text_event_t event;
    assert(mcp_text_event_get(status.count - 1, &event, payload, sizeof(payload)) == 0);
    assert(event.pc == 0xFFFF);
}

static void test_overflow_reports_seq_gap_and_resets_dropped_after_discard(void) {
    mcp_text_events_reset();
    mcp_text_event_status_t before = mcp_text_events_status();
    static const uint8_t payload[] = {'x'};
    const unsigned overflow = 3;
    for (unsigned i = 0; i < MCP_TEXT_EVENT_MAX + overflow; ++i)
        assert(mcp_text_event_emit(i, "test", "overflow", "raw", 0, 0, 0, 0, payload,
                                   sizeof(payload)) == 0);

    mcp_text_event_status_t status = mcp_text_events_status();
    assert(status.count == MCP_TEXT_EVENT_MAX && status.dropped == overflow);
    mcp_text_event_t event;
    uint8_t first_payload[sizeof(payload)];
    assert(mcp_text_event_get(0, &event, first_payload, sizeof(first_payload)) == 0);
    assert(event.seq == before.next_seq + overflow);

    mcp_text_events_discard(status.count);
    status = mcp_text_events_status();
    assert(status.count == 0 && status.dropped == 0);
}

static void test_capture_uses_logical_debug_read(void) {
    uint8_t ram[RAM_SIZE] = {0};
    ram[0x200] = 0x82;
    ram[0x201] = 0xA0;
    ram[0x202] = '\0';
    mc68k_state_t *cpu = make_cpu(ram);
    mcp_text_events_reset();
    assert(mcp_text_event_capture_cstring(cpu, 0x1234, "title", "puts", "shift-jis", 3, 4,
                                          MCP_TEXT_EVENT_UNSPECIFIED, 0x200, 32) == 0);
    mcp_text_event_t event;
    uint8_t payload[4];
    assert(mcp_text_event_get(0, &event, payload, sizeof(payload)) == 0);
    assert(event.pc == 0x1234 && event.payload_len == 2 && event.flags == 0);
    assert(memcmp(payload, ram + 0x200, 2) == 0);
    mc68k_destroy(cpu);
}

int main(void) {
    test_order_and_reset_epoch();
    test_overflow_discards_complete_events();
    test_overflow_reports_seq_gap_and_resets_dropped_after_discard();
    test_capture_uses_logical_debug_read();
    return 0;
}
