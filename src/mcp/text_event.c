#include "mcp/text_event.h"

#include "mc68000/debug.h"
#include <string.h>
#include <time.h>

typedef struct {
    mcp_text_event_t meta;
    uint32_t payload_offset;
} text_event_slot_t;

static text_event_slot_t s_events[MCP_TEXT_EVENT_MAX];
static uint8_t s_bytes[MCP_TEXT_EVENT_BYTES];
static uint32_t s_event_first;
static uint32_t s_event_count;
static uint32_t s_byte_head;
static uint32_t s_byte_used;
static uint32_t s_dropped;
static uint64_t s_epoch;
static uint64_t s_next_seq;

/* 別プロセスで起動し直した場合に epoch が前回と一致して再起動を見逃さないよう、
 * 初回利用時に起動時刻由来の値で epoch を初期化する。 */
static void ensure_epoch(void) {
    if (s_epoch != 0)
        return;
    struct timespec now;
#ifdef _WIN32
    if (timespec_get(&now, TIME_UTC) != TIME_UTC)
#else
    /* CMAKE_C_STANDARD 99 では glibc が TIME_UTC/timespec_get(C11)を公開しない。 */
    if (clock_gettime(CLOCK_REALTIME, &now) != 0)
#endif
        now.tv_sec = now.tv_nsec = 0;
    s_epoch = ((uint64_t)now.tv_sec * 1000000007ull) ^ (uint64_t)now.tv_nsec;
    if (s_epoch == 0)
        s_epoch = 1;
}

static void copy_name(char *dst, size_t dst_size, const char *src) {
    if (!src)
        src = "";
    size_t n = strlen(src);
    if (n >= dst_size)
        n = dst_size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void remove_oldest(int dropped) {
    if (s_event_count == 0)
        return;
    text_event_slot_t *slot = &s_events[s_event_first];
    s_byte_used -= slot->meta.payload_len;
    s_event_first = (s_event_first + 1u) % MCP_TEXT_EVENT_MAX;
    s_event_count--;
    if (dropped)
        s_dropped++;
}

static void write_payload(uint32_t offset, const uint8_t *payload, size_t len) {
    for (size_t i = 0; i < len; ++i)
        s_bytes[(offset + (uint32_t)i) % MCP_TEXT_EVENT_BYTES] = payload[i];
}

int mcp_text_event_emit(uint32_t pc, const char *source, const char *probe,
                        const char *encoding, int32_t x, int32_t y, int32_t attr,
                        uint16_t flags, const uint8_t *payload, size_t payload_len) {
    if ((!payload && payload_len) || payload_len > MCP_TEXT_EVENT_MAX_PAYLOAD)
        return -1;
    ensure_epoch();
    while (s_event_count == MCP_TEXT_EVENT_MAX ||
           MCP_TEXT_EVENT_BYTES - s_byte_used < payload_len)
        remove_oldest(1);

    uint32_t index = (s_event_first + s_event_count) % MCP_TEXT_EVENT_MAX;
    text_event_slot_t *slot = &s_events[index];
    *slot = (text_event_slot_t){
        .meta = {.seq = s_next_seq++,
                 .epoch = s_epoch,
                 .pc = pc,
                 .x = x,
                 .y = y,
                 .attr = attr,
                 .flags = flags,
                 .payload_len = (uint32_t)payload_len},
        .payload_offset = s_byte_head,
    };
    copy_name(slot->meta.source, sizeof(slot->meta.source), source);
    copy_name(slot->meta.probe, sizeof(slot->meta.probe), probe);
    copy_name(slot->meta.encoding, sizeof(slot->meta.encoding), encoding);
    if (payload_len)
        write_payload(s_byte_head, payload, payload_len);
    s_byte_head = (s_byte_head + (uint32_t)payload_len) % MCP_TEXT_EVENT_BYTES;
    s_byte_used += (uint32_t)payload_len;
    s_event_count++;
    return 0;
}

int mcp_text_event_capture_cstring(mc68k_state_t *cpu, uint32_t pc, const char *source,
                                   const char *probe, const char *encoding, int32_t x,
                                   int32_t y, int32_t attr, uint32_t address, size_t max_len) {
    if (!cpu || max_len == 0)
        return -1;
    if (max_len > MCP_TEXT_EVENT_MAX_PAYLOAD)
        max_len = MCP_TEXT_EVENT_MAX_PAYLOAD;
    uint8_t bytes[MCP_TEXT_EVENT_MAX_PAYLOAD];
    size_t len = 0;
    uint16_t flags = 0;
    for (; len < max_len; ++len) {
        uint8_t ch;
        if (mc68k_debug_read_bytes(cpu, address + (uint32_t)len, &ch, 1) != 0) {
            flags |= MCP_TEXT_EVENT_READ_ERROR;
            break;
        }
        if (ch == 0)
            break;
        bytes[len] = ch;
    }
    if (len == max_len)
        flags |= MCP_TEXT_EVENT_TRUNCATED;
    return mcp_text_event_emit(pc, source, probe, encoding, x, y, attr, flags, bytes, len);
}

void mcp_text_events_reset(void) {
    s_event_first = 0;
    s_event_count = 0;
    s_byte_head = 0;
    s_byte_used = 0;
    s_dropped = 0;
    ensure_epoch();
    s_epoch++;
    if (s_epoch == 0)
        s_epoch = 1;
}

mcp_text_event_status_t mcp_text_events_status(void) {
    ensure_epoch();
    return (mcp_text_event_status_t){
        .count = s_event_count, .dropped = s_dropped, .epoch = s_epoch, .next_seq = s_next_seq};
}

int mcp_text_event_get(size_t oldest, mcp_text_event_t *out, uint8_t *payload,
                       size_t payload_cap) {
    if (!out || oldest >= s_event_count)
        return -1;
    const text_event_slot_t *slot =
        &s_events[(s_event_first + (uint32_t)oldest) % MCP_TEXT_EVENT_MAX];
    if (slot->meta.payload_len > payload_cap || (slot->meta.payload_len && !payload))
        return -1;
    *out = slot->meta;
    for (uint32_t i = 0; i < slot->meta.payload_len; ++i)
        payload[i] = s_bytes[(slot->payload_offset + i) % MCP_TEXT_EVENT_BYTES];
    return 0;
}

void mcp_text_events_discard(size_t count) {
    while (count-- && s_event_count)
        remove_oldest(0);
    s_dropped = 0;
}
