#include "app/fpu_config.h"
#include "mc68000/cpu.h"
#include "mc68000/fpu.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t mem[0x10000];
    int cpu_space_reads;
    int cpu_space_writes;
} test_bus_t;

static bool read8(void *ctx, uint32_t addr, uint8_t *out) {
    test_bus_t *bus = ctx;
    if (addr >= sizeof(bus->mem))
        return false;
    *out = bus->mem[addr];
    return true;
}

static bool read16(void *ctx, uint32_t addr, uint16_t *out) {
    uint8_t hi, lo;
    return read8(ctx, addr, &hi) && read8(ctx, addr + 1u, &lo) &&
           ((*out = ((uint16_t)hi << 8) | lo), true);
}

static bool read32(void *ctx, uint32_t addr, uint32_t *out) {
    uint16_t hi, lo;
    return read16(ctx, addr, &hi) && read16(ctx, addr + 2u, &lo) &&
           ((*out = ((uint32_t)hi << 16) | lo), true);
}

static bool write8(void *ctx, uint32_t addr, uint8_t value) {
    test_bus_t *bus = ctx;
    if (addr >= sizeof(bus->mem))
        return false;
    bus->mem[addr] = value;
    return true;
}

static bool write16(void *ctx, uint32_t addr, uint16_t value) {
    return write8(ctx, addr, (uint8_t)(value >> 8)) && write8(ctx, addr + 1u, (uint8_t)value);
}

static bool write32(void *ctx, uint32_t addr, uint32_t value) {
    return write16(ctx, addr, (uint16_t)(value >> 16)) &&
           write16(ctx, addr + 2u, (uint16_t)value);
}

static bool cpu_space_read(void *ctx, uint8_t fc, uint32_t addr, uint8_t size, uint32_t *out) {
    test_bus_t *bus = ctx;
    (void)fc;
    (void)addr;
    (void)size;
    (void)out;
    bus->cpu_space_reads++;
    return false;
}

static bool cpu_space_write(void *ctx, uint8_t fc, uint32_t addr, uint8_t size,
                            uint32_t value) {
    test_bus_t *bus = ctx;
    (void)fc;
    (void)addr;
    (void)size;
    (void)value;
    bus->cpu_space_writes++;
    return false;
}

static mc68k_bus_t hooks_with_cpu_space_probe(test_bus_t *bus) {
    mc68k_bus_t hooks;
    memset(&hooks, 0, sizeof(hooks));
    hooks.read_byte = read8;
    hooks.read_word = read16;
    hooks.read_long = read32;
    hooks.write_byte = write8;
    hooks.write_word = write16;
    hooks.write_long = write32;
    hooks.cpu_space_read = cpu_space_read;
    hooks.cpu_space_write = cpu_space_write;
    hooks.ctx = bus;
    return hooks;
}

/* mc68k_get_fpu_const() は外付け68881/68882用。040内蔵FPUはCPU専有のため、
 * ここではFPCPオブジェクトがある構成だけを検査する。 */
static void check_cpu(mc68k_model_t model, x68k_fpu_mode_t mode, mc68k_fpu_config_t expected,
                      int has_fpcp) {
    mc68k_fpu_config_t config = MC68K_FPU_NONE;
    assert(x68k_select_fpu_config(model, mode, &config));
    assert(config == expected);
    test_bus_t bus = {0};
    mc68k_bus_t hooks = hooks_with_cpu_space_probe(&bus);
    mc68k_state_t *cpu = mc68k_create_model_with_fpu(&hooks, model, config);
    assert(cpu);
    const mc68k_fpu_t *fpu = mc68k_get_fpu_const(cpu);
    assert((fpu != NULL) == has_fpcp);
    if (has_fpcp)
        assert(mc68k_fpu_get_config(fpu) == expected);
    mc68k_destroy(cpu);
}

static void put16(test_bus_t *bus, uint32_t addr, uint16_t value) {
    assert(write16(bus, addr, value));
}

static void put32(test_bus_t *bus, uint32_t addr, uint32_t value) {
    assert(write32(bus, addr, value));
}

/* FNOP は F-line cpID=1。68882 が CPU 所有で処理し、CPU 空間フックを通らないことを確認する。 */
static void check_fnop(mc68k_model_t model, x68k_fpu_mode_t mode, int has_fpu) {
    test_bus_t bus = {0};
    put32(&bus, 0, 0x0000f000u);        /* initial SSP */
    put32(&bus, 4, 0x00000100u);        /* reset PC */
    put32(&bus, 11u * 4u, 0x00000200u); /* vector $2c (F-line) */
    put16(&bus, 0x100, 0xf280u);        /* FNOP */
    put16(&bus, 0x102, 0x0000u);
    put16(&bus, 0x104, 0x4e71u); /* NOP */
    put16(&bus, 0x200, 0x4e71u);

    mc68k_fpu_config_t config;
    assert(x68k_select_fpu_config(model, mode, &config));
    mc68k_bus_t hooks = hooks_with_cpu_space_probe(&bus);
    mc68k_state_t *cpu = mc68k_create_model_with_fpu(&hooks, model, config);
    assert(cpu);
    mc68k_reset(cpu);
    (void)mc68k_execute(cpu, 1);
    mc68k_regs_t regs;
    mc68k_get_regs(cpu, &regs);
    if (has_fpu) {
        assert(regs.pc == 0x104u);
        assert(bus.cpu_space_reads == 0 && bus.cpu_space_writes == 0);
    } else {
        assert(regs.pc == 0x200u);
        assert(regs.a[7] == 0x0000eff8u); /* 68030 format 0: SR/PC/format-vector の8 bytes */
        assert(((uint16_t)bus.mem[0xeff8] << 8 | bus.mem[0xeff9]) == 0x2700u);
        assert(((uint32_t)bus.mem[0xeffa] << 24 | (uint32_t)bus.mem[0xeffb] << 16 |
                (uint32_t)bus.mem[0xeffc] << 8 | bus.mem[0xeffd]) == 0x00000100u);
        assert(((uint16_t)bus.mem[0xeffe] << 8 | bus.mem[0xefff]) == 0x002cu);
    }
    mc68k_destroy(cpu);
}

int main(void) {
    check_cpu(MC68K_MODEL_68EC030, X68K_FPU_ON, MC68K_FPU_68882, 1);
    check_cpu(MC68K_MODEL_68030, X68K_FPU_ON, MC68K_FPU_68882, 1);
    check_cpu(MC68K_MODEL_68000, X68K_FPU_ON, MC68K_FPU_NONE, 0);
    check_cpu(MC68K_MODEL_68040, X68K_FPU_ON, MC68K_FPU_68040, 0);
    check_cpu(MC68K_MODEL_68040, X68K_FPU_OFF, MC68K_FPU_NONE_040, 0);
    check_cpu(MC68K_MODEL_68060, X68K_FPU_ON, MC68K_FPU_68060, 0);
    check_cpu(MC68K_MODEL_68060, X68K_FPU_OFF, MC68K_FPU_NONE, 0);

    check_cpu(MC68K_MODEL_68EC030, X68K_FPU_68882, MC68K_FPU_68882, 1);
    check_cpu(MC68K_MODEL_68030, X68K_FPU_68882, MC68K_FPU_68882, 1);
    check_cpu(MC68K_MODEL_68EC030, X68K_FPU_OFF, MC68K_FPU_NONE, 0);
    check_cpu(MC68K_MODEL_68030, X68K_FPU_OFF, MC68K_FPU_NONE, 0);
    check_cpu(MC68K_MODEL_68000, X68K_FPU_OFF, MC68K_FPU_NONE, 0);

    check_fnop(MC68K_MODEL_68EC030, X68K_FPU_ON, 1);
    check_fnop(MC68K_MODEL_68030, X68K_FPU_ON, 1);
    check_fnop(MC68K_MODEL_68EC030, X68K_FPU_OFF, 0);
    check_fnop(MC68K_MODEL_68030, X68K_FPU_OFF, 0);

    mc68k_fpu_config_t config = MC68K_FPU_NONE;
    assert(!x68k_select_fpu_config(MC68K_MODEL_68000, X68K_FPU_68882, &config));
    assert(x68k_select_fpu_config(MC68K_MODEL_68000, X68K_FPU_68881, &config));
    assert(x68k_select_fpu_config(MC68K_MODEL_68000, X68K_FPU_68881_SECONDARY, &config));
    assert(config == MC68K_FPU_NONE);
    assert(x68k_fpu_uses_cir(MC68K_MODEL_68000, X68K_FPU_ON));
    assert(x68k_fpu_uses_cir(MC68K_MODEL_68000, X68K_FPU_68881));
    assert(!x68k_fpu_uses_cir(MC68K_MODEL_68EC030, X68K_FPU_ON));
    assert(x68k_fpu_uses_cir_secondary(X68K_FPU_68881_SECONDARY));
    assert(!x68k_fpu_uses_cir(MC68K_MODEL_68000, X68K_FPU_OFF));
    assert(!x68k_select_fpu_config(MC68K_MODEL_68EC030, X68K_FPU_68881, &config));
    assert(!x68k_select_fpu_config(MC68K_MODEL_68030, X68K_FPU_68881_SECONDARY, &config));
    assert(!x68k_select_fpu_config(MC68K_MODEL_68040, X68K_FPU_68882, &config));
    assert(!x68k_select_fpu_config(MC68K_MODEL_68010, X68K_FPU_68882, &config));
    assert(!x68k_select_fpu_config(MC68K_MODEL_68020, X68K_FPU_68882, &config));
    assert(!x68k_select_fpu_config(MC68K_MODEL_68010, X68K_FPU_OFF, &config));
    assert(!x68k_select_fpu_config(MC68K_MODEL_68020, X68K_FPU_ON, &config));
    assert(!x68k_select_fpu_config(MC68K_MODEL_68030, (x68k_fpu_mode_t)99, &config));
    puts("fpu_fpcp_x68030_test: OK");
    return 0;
}
