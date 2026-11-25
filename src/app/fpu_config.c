#include "app/fpu_config.h"

bool x68k_select_fpu_config(mc68k_model_t model, x68k_fpu_mode_t mode,
                            mc68k_fpu_config_t *out) {
    if (!out)
        return false;

    if (model != MC68K_MODEL_68000 && model != MC68K_MODEL_68EC030 &&
        model != MC68K_MODEL_68030 && model != MC68K_MODEL_68040 && model != MC68K_MODEL_68060)
        return false;

    switch (mode) {
    case X68K_FPU_OFF:
        *out = model == MC68K_MODEL_68040 ? MC68K_FPU_NONE_040 : MC68K_FPU_NONE;
        return true;
    case X68K_FPU_ON:
        *out = model == MC68K_MODEL_68040                                   ? MC68K_FPU_68040
               : model == MC68K_MODEL_68060                                 ? MC68K_FPU_68060
               : model == MC68K_MODEL_68EC030 || model == MC68K_MODEL_68030 ? MC68K_FPU_68882
                                                                            : MC68K_FPU_NONE;
        return true;
    case X68K_FPU_68881:
    case X68K_FPU_68881_SECONDARY:
        if (model != MC68K_MODEL_68000)
            return false;
        *out = MC68K_FPU_NONE;
        return true;
    case X68K_FPU_68882:
        if (model != MC68K_MODEL_68EC030 && model != MC68K_MODEL_68030)
            return false;
        *out = MC68K_FPU_68882;
        return true;
    }
    return false;
}

bool x68k_fpu_uses_cir(mc68k_model_t model, x68k_fpu_mode_t mode) {
    return model == MC68K_MODEL_68000 &&
           (mode == X68K_FPU_ON || mode == X68K_FPU_68881 || mode == X68K_FPU_68881_SECONDARY);
}

bool x68k_fpu_uses_cir_secondary(x68k_fpu_mode_t mode) {
    return mode == X68K_FPU_68881_SECONDARY;
}
