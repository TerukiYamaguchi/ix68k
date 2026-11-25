#pragma once

#include "app/config.h"
#include "mc68000/cpu_types.h"

#include <stdbool.h>

/* ix68k が許可する CPU/FPU 組合せだけを選ぶ。68881 は外付けCIRなので out は NONE。 */
bool x68k_select_fpu_config(mc68k_model_t model, x68k_fpu_mode_t mode, mc68k_fpu_config_t *out);

/* 選択したCPU/FPU構成がCZ-6BP1のCIR windowを有効化するか。 */
bool x68k_fpu_uses_cir(mc68k_model_t model, x68k_fpu_mode_t mode);
bool x68k_fpu_uses_cir_secondary(x68k_fpu_mode_t mode);
