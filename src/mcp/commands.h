#pragma once

#include "bus/x68k_bus.h"
#include "mcp/internal.h"

/* コマンドを処理する。認識した場合は非0を返す。
 * WAIT_* / DBG_WAIT / PASTE の後続処理は context に保持され、poll で完了する。 */
int mcp_commands_handle(x68k_bus_t *bus, mcp_context_t *context, const char *line);
void mcp_commands_poll(x68k_bus_t *bus, mcp_context_t *context);
void mcp_commands_client_disconnected(mcp_context_t *context);
