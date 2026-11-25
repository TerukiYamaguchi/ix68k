#pragma once

#include <stdint.h>

#include "bus/x68k_bus.h"

/* 現在のフレームを PPM(P6) 形式で生成する。呼び出し側が free() する。
 * mask==0 は表示済みフレーム、非0 は指定レイヤーだけの再合成を返す。
 * 失敗時は NULL を返し、out_len は変更しない。 */
uint8_t *mcp_screenshot_make_ppm(x68k_bus_t *bus, int mask, int *out_len);
