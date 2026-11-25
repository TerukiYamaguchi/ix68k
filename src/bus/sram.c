#include "bus/bus_internal.h"
#include <stdio.h>
#include <string.h>

/* ---- SRAM 永続化 ---- */

/*
 * 実機 SRAM 領域: 物理 0xED0000–0xED3FFF (16KB)
 * SRAM_REAL_OFFSET は上部の #define 群を参照（read8/write8 の ram_limit() からも使うため）。
 */
#define SRAM_REAL_SIZE 0x004000u /* 16KB */

/* SRAM[$ED005A]: ROM が参照する FDD 内蔵台数設定。
 * 0 のままだと ROM が FDD アクセスを全てエラーにする。
 * 実機では工場出荷時 or システムメニューで設定される値。
 * X68000 XVI 標準は 2 基内蔵。 */
#define SRAM_FDD_COUNT_OFF 0x5Au

static void sram_ensure_fdd_count(x68k_bus_t *bus) {
    if (bus->sram[SRAM_REAL_OFFSET + SRAM_FDD_COUNT_OFF] == 0)
        bus->sram[SRAM_REAL_OFFSET + SRAM_FDD_COUNT_OFF] = 2;
}

int x68k_bus_load_sram(x68k_bus_t *bus, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    size_t n = fread(bus->sram + SRAM_REAL_OFFSET, 1, SRAM_REAL_SIZE, f);
    fclose(f);
    sram_ensure_fdd_count(bus);
    x68k_bus_page_map_refresh(bus); /* $ED0008 の ram_limit が変わりうる */
    return (n == SRAM_REAL_SIZE) ? 0 : -1;
}

int x68k_bus_save_sram(const x68k_bus_t *bus, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    size_t n = fwrite(bus->sram + SRAM_REAL_OFFSET, 1, SRAM_REAL_SIZE, f);
    fclose(f);
    return (n == SRAM_REAL_SIZE) ? 0 : -1;
}

void x68k_bus_init_sram_defaults(x68k_bus_t *bus) {
    /*
     * ROM 内蔵デフォルト SRAM データを ROM から検索してコピーする。
     * ROMバリアントによって格納オフセットが異なるため、固定アドレスに頼らず
     * マジックバイト列で IPLROM 全域 (iocs_rom[0x0C0000..0x0DFFFF]) を走査して探す。
     *
     * SRAM マジック: {0x82, 0x77, '6', '8', '0', '0', '0', 'W'}
     * CGROM のみの場合はマジックが存在しないため何もしない。
     */
    static const uint8_t magic[8] = {0x82, 0x77, 0x36, 0x38, 0x30, 0x30, 0x30, 0x57};
    /* iocs_rom[0x0C0000..0x0DFFFF] が 128KB IPLROM 領域 */
    const uint8_t *rom_start = bus->iocs_rom + 0x0C0000;
    const uint8_t *src = NULL;
    for (int i = 0; i <= 0x20000 - 91; i++) {
        if (memcmp(rom_start + i, magic, 8) == 0) {
            src = rom_start + i;
            break;
        }
    }
    if (!src)
        return; /* ROM 未ロード or SRAM デフォルトなし */
    memcpy(bus->sram + SRAM_REAL_OFFSET, src, 91);
    /* SRAM[0x26]: オプション機器フラグ。ROM デフォルト 0x07 はオプション機器あり想定だが、
     * ix68k はオプション機器なし構成のため 0 にクリアする。
     * これにより BIOS が FDD ブートパスへ進む。 */
    bus->sram[SRAM_REAL_OFFSET + 0x26] = 0x00;
    x68k_bus_page_map_refresh(bus); /* $ED0008 の ram_limit が変わりうる */
}

int x68k_bus_load_cgrom(x68k_bus_t *bus, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    /*
     * CGROM: 0xF00000–0xFBFFFF (768KB = CGROM_SIZE)
     * IPLROM(iocs_rom[IPLROM_BUF_OFFSET..]) を壊さないよう CGROM_SIZE で打ち切る。
     */
    size_t n = fread(bus->iocs_rom, 1, CGROM_SIZE, f);
    fclose(f);
    return (n > 0) ? 0 : -1;
}

/*
 * ベクタ 2–63 を RTE スタブ (RAM[0x0100]) に初期化する。
 *
 * BIOS は割り込みマスクを解除した直後にベクタテーブルを設定するが、
 * それより前に任意の例外・割り込み・TRAP が発生しても
 * RTE スタブに飛んで安全に戻れるようにする。
 *
 * vec 2-9   : バスエラー, アドレスエラー, 不正命令, ゼロ除算, CHK, TRAPV,
 *             特権違反, トレース
 * vec 10-23 : 未使用/未実装
 * vec 24    : スプリアス割り込み
 * vec 25-31 : オートベクタ (レベル 1–7 割り込み)
 * vec 32-47 : TRAP #0–#15（TRAP #15 は mc68k_set_trap_handler で補足）
 * vec 48-63 : FP/MMU 予約
 *
 * vec 32-47 (TRAP #0-#15) も 0x000100 にしておかないと、未初期化のベクタが
 * 0x000000 (リセットベクタ領域) を指し、その内容が命令として実行されて
 * 連鎖例外を起こす。
 * vec 64-255 は初期化しない（0x000100 を指す場合、その書き込みが
 * 0x000100 のRTEスタブを上書きするため）。ユーザ定義ベクタは BIOS が設定する。
 *
 * RTE スタブは 0x000100 に置く。
 * vec 63 の最終バイトは 0xFC + 3 = 0xFF なので、
 * ループ後に 0x0100 を書いても上書きされない。
 */
static void install_rte_stubs(x68k_bus_t *bus) {
    for (int v = 2; v <= 63; v++) {
        uint32_t va = (uint32_t)v * 4;
        bus->ram[va] = 0x00;
        bus->ram[va + 1] = 0x00;
        bus->ram[va + 2] = 0x01;
        bus->ram[va + 3] = 0x00; /* → 0x00000100 (RTE スタブ) */
    }
    bus->ram[0x0100] = 0x4E; /* RTE: 0x4E73 — 例外ハンドラ用 */
    bus->ram[0x0101] = 0x73;
    bus->ram[0x0102] = 0x4E; /* RTS: 0x4E75 — JSR 戻りスタブ用 */
    bus->ram[0x0103] = 0x75;
}

/*
 * MCP/ソフトリセット用: 電源投入時ROMリセットベクタ（poweron_vec、
 * x68k_bus_load_iocs_rom が起動時に確定させる）を RAM $0-$7 へ書き戻し、
 * RTE スタブも再設置する。mc68k_reset() の直前に呼ぶこと。
 *
 * mc68k_reset() は単純に $0/$4 をバス経由で読むだけなので、Human68k 起動後に
 * この領域が独自ベクタで上書きされたままだと SSP/PC にゴミを読み込み CPU が
 * 暴走する。
 */
void x68k_bus_reset_vectors(x68k_bus_t *bus) {
    memcpy(bus->ram, bus->poweron_vec, 8);
    install_rte_stubs(bus);
}

/* ベクタ24（スプリアス割り込み）の例外ベクタ番地と、そこへ固定する IPLROM 内の
 * 単独 rte 命令。IPLROM_BASE は IPLROM 本体（$FE0000-$FFFFFF）の先頭で、
 * 「現在の値が ROM を指しているか」の判定に使う。 */
#define SPURIOUS_VEC_NUM  0x18u     /* ベクタ番号 24 */
#define SPURIOUS_VEC_ADDR 0x000060u /* = SPURIOUS_VEC_NUM * 4 */
#define SPURIOUS_VEC_RTE  0xFE006Cu /* IPLROM nmi_handler 末尾の単独 rte */
#define IPLROM_BASE       0xFE0000u

/*
 * CPU への IRQ 線更新は x68k_cycle_catchup()（MFP_TICK_CYCLES 粒度）
 * でしか行われない一方、MFP の IMRA/IMRB 書き込みはマスクしたチャンネルの
 * mfp_ch_pending をその場でクリアする。このため「線はレベル6のまま、pending は
 * 空」という古い線が生じ、ゲストが直後に IPL を下げるとベクタの無いレベル6
 * 割り込み（IACK で応答デバイスなし＝スプリアス割り込み、ベクタ24）を稀に引く。
 *
 * 実機でも MFP まわりの不都合でスプリアス割り込みは起こりうるとされ、
 * 060turbo の ROM は起動時にベクタ24のハンドラを RTE だけのルーチンに
 * 差し替えて無視する対策を取っている（発生源を問わず安全側に倒せる）。
 *
 * RTE スタブの参照先は install_rte_stubs() の RAM[0x0100] ではなく、
 * IPLROM 内に元々ある単独 rte 命令（$0000FE006C、nmi_handler 末尾。
 * dis/disasm/iplromxv/IPLROMXV_addr.S:52 で確認済み）を使う。RAM[0x0100] は
 * BIOS 実行前だけの一時的な安全地帯で、Human68k 起動後はワークエリアとして
 * 再利用され得る（動作確認で実際に上書きされ「おかしな命令」例外に化けた）。
 * ROM は read-only で常時 $FE0000-$FFFFFF にマップされ続けるため安全。
 *
 * ベクタ24自体もIPLROM起動時のベクタ一括フィルにすぐ上書きされてしまう
 * （実測でベクタ24が IPLROM の共有例外ハンドラを指すことを確認済み）ため、
 * x68k_cycle_catchup() から毎 tick 冪等に呼び、上書きされても即座に固定し直す。
 */
void x68k_bus_pin_spurious_vector(x68k_bus_t *bus) {
    /* ゲストがベクタ24を自前の値で使っているなら触らない。NetBSD/x68k は
     * カーネルを物理 $0 へロードするため $60 はベクタではなくカーネルコードで、
     * 無条件に書き込むと locore.s の CPU 判別部（$5E の movc %d0,%cacr）を
     * 壊して不当命令例外になる。書き戻す対象は
     *   - IPLROM のベクタ一括フィル値（上位バイト＝ベクタ番号、下位24bitは
     *     ROM 内の共有例外ハンドラ。例: $18FF0770）
     *   - 本関数が以前書いた値そのもの
     * の2つだけに限定する。どちらも下位24bitが IPLROM 領域を指す。 */
    uint32_t cur = x68k_bus_read32(bus, SPURIOUS_VEC_ADDR);
    uint8_t tag = (uint8_t)(cur >> 24);
    if ((cur & 0x00FFFFFFu) < IPLROM_BASE)
        return;
    if (tag != SPURIOUS_VEC_NUM && tag != 0u)
        return;
    x68k_bus_write32(bus, SPURIOUS_VEC_ADDR, SPURIOUS_VEC_RTE);
}

int x68k_bus_load_iocs_rom(x68k_bus_t *bus, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    rewind(f);

    size_t n;
    if (fsize == 0x20000) {
        /*
         * 128KB IPLROM (IPLROM.DAT / IPLROMXV.DAT):
         * X68000 の IOCS ROM は 0xFC0000–0xFDFFFF に配置される。
         * iocs_rom バッファ内オフセット = 0xFC0000 - 0xF00000 = 0x0C0000。
         *
         * リセットベクタ (SSP/PC) は ROM 後半 64KB (A16=1) の先頭 8 バイトから読む。
         * 電源投入時、ROM A16=1 の 64KB が $000000–$00FFFF にもマップされるため
         * CPU は $000000/$000004 からリセットベクタを読み取る。
         * ROM ファイルオフセット $10000 = 物理 $FD0000 = パワーオン時 $000000。
         */
        n = fread(bus->iocs_rom + 0x0C0000, 1, 0x20000, f);
        fclose(f);
        if ((long)n < 0x20000)
            return -1;

        /*
         * パワーオン時のリセットベクタは ROM 後半 64KB (A16=1) から提供される。
         * ROM ファイルオフセット $10000 (= 物理 $FD0000) の先頭 8 バイトが
         * SSP (4B) と PC (4B) として $000000/$000004 に見える。
         *
         * IPLROMXV: SSP=$00002000 (RAM 先頭付近), PC=$00FF0010 (ROM ミラー経由)
         * $FF0010 の最初の命令: MOVE #$2700,SR / LEA $2000,SP — 確認済み
         */
        uint8_t *rom_hi = bus->iocs_rom + 0x0C0000 + 0x10000; /* ROM 後半 ($FD0000) */
        uint32_t ssp = ((uint32_t)rom_hi[0] << 24) | ((uint32_t)rom_hi[1] << 16) |
                       ((uint32_t)rom_hi[2] << 8) | (uint32_t)rom_hi[3];
        uint32_t pc = ((uint32_t)rom_hi[4] << 24) | ((uint32_t)rom_hi[5] << 16) |
                      ((uint32_t)rom_hi[6] << 8) | (uint32_t)rom_hi[7];

        uint8_t reset_vec[8] = {(uint8_t)(ssp >> 24), (uint8_t)(ssp >> 16), (uint8_t)(ssp >> 8),
                                (uint8_t)ssp,         (uint8_t)(pc >> 24),  (uint8_t)(pc >> 16),
                                (uint8_t)(pc >> 8),   (uint8_t)pc};
        memcpy(bus->ram, reset_vec, 8);
        memcpy(bus->poweron_vec, reset_vec, 8); /* soft reset 用スナップショット */

        install_rte_stubs(bus);
    } else {
        /* 1MB フルイメージ: 0xF00000 からそのまま配置。先頭 8B をベクタとして使用 */
        n = fread(bus->iocs_rom, 1, IOCS_ROM_SIZE, f);
        fclose(f);
        if (n < 8)
            return -1;
        memcpy(bus->ram, bus->iocs_rom, 8);
        memcpy(bus->poweron_vec, bus->iocs_rom, 8); /* soft reset 用スナップショット */
    }

    return 0;
}

const uint8_t *x68k_bus_get_cgrom_ank(const x68k_bus_t *bus) {
    return bus->iocs_rom + 0x3A000;
}
