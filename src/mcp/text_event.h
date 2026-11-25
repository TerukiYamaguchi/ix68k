#pragma once

#include <stddef.h>
#include <stdint.h>

#include "mc68000/cpu_types.h"

/* TEXT EVENTS の内部保持形式。payload は固定長メタデータとは別のバイトリングに置く。 */
#define MCP_TEXT_EVENT_MAX                 16384u
#define MCP_TEXT_EVENT_BYTES               (64u * 1024u)
#define MCP_TEXT_EVENT_MAX_PAYLOAD         4096u
#define MCP_TEXT_EVENT_SOURCE_MAX          32u
#define MCP_TEXT_EVENT_PROBE_MAX           32u
#define MCP_TEXT_EVENT_ENCODING_MAX        16u
#define MCP_TEXT_EVENT_UNSPECIFIED         INT32_MIN
/* TEXT EVENTSのワイヤ形式: TEV1/count/dropped/epochとイベント固定メタデータ。 */
#define MCP_TEXT_EVENTS_WIRE_HEADER_BYTES  20u
#define MCP_TEXT_EVENT_WIRE_METADATA_BYTES 118u

enum {
    MCP_TEXT_EVENT_TRUNCATED = 1u << 0,  /* 指定上限でNUL終端前に打ち切った */
    MCP_TEXT_EVENT_READ_ERROR = 1u << 1, /* 論理メモリ読出しに失敗した */
};

typedef struct {
    uint64_t seq;
    uint64_t epoch;
    uint32_t pc;
    int32_t x, y, attr;
    uint16_t flags;
    char source[MCP_TEXT_EVENT_SOURCE_MAX];
    char probe[MCP_TEXT_EVENT_PROBE_MAX];
    char encoding[MCP_TEXT_EVENT_ENCODING_MAX];
    uint32_t payload_len;
} mcp_text_event_t;

typedef struct {
    uint32_t count;
    uint32_t dropped;
    uint64_t epoch;
    uint64_t next_seq;
} mcp_text_event_status_t;

/* 生バイトを1イベントとして記録する。payload_lenはMCP_TEXT_EVENT_MAX_PAYLOAD以下。 */
int mcp_text_event_emit(uint32_t pc, const char *source, const char *probe,
                        const char *encoding, int32_t x, int32_t y, int32_t attr,
                        uint16_t flags, const uint8_t *payload, size_t payload_len);

/* 現行SRに対応する論理アドレスからNUL終端文字列を安全に読み、イベント化する。
 * 不正ポインタ・上限超過は停止ではなくflagsへ反映する。 */
int mcp_text_event_capture_cstring(mc68k_state_t *cpu, uint32_t pc, const char *source,
                                   const char *probe, const char *encoding, int32_t x,
                                   int32_t y, int32_t attr, uint32_t address, size_t max_len);

void mcp_text_events_reset(void);
mcp_text_event_status_t mcp_text_events_status(void);

/* oldest=0としてイベントとpayloadをコピーする。payload_cap不足時は-1。 */
int mcp_text_event_get(size_t oldest, mcp_text_event_t *out, uint8_t *payload,
                       size_t payload_cap);

/* oldest側からcount件を破棄する。MCP送信成功後にだけ呼ぶ。
 * 成功取得までにリング容量超過で失われた件数も、ここで次回取得向けにリセットする。 */
void mcp_text_events_discard(size_t count);
