#pragma once
#include <stdint.h>

/* HOSTDRVリモートデバイス。macOSの任意ディレクトリをHuman68kの
 * 「リモートデバイス」（属性ワードbit13）
 * として公開する。FAT を合成せず、_OPEN/_READ 等のファイル系 DOS コールを comcod
 * $41〜$58 としてそのまま受け取り、ホスト側の open/read/readdir 等へ直接マッピングする。
 * bus 実装には依存しない（テスト・差し替えを容易にするため）。
 *
 * ゲスト側は \SYS\HOSTDRV.SYS（tools-x68k/hostdrv/）が担う。ドライバの割り込みルーチンは
 * リクエストヘッダのアドレスを REG_A5 へ書き、COMMAND=EXEC を撃つだけの薄いブリッジで、
 * このモジュールがリクエストヘッダをバス経由で直接読み書きして結果を書き戻す。
 * 初期化 comcod $40 のみドライバ側で完結させる（ドライバ末尾アドレスはリンク時に
 * 決まる自シンボルを使う必要があり、このモジュールはデバイスヘッダのアドレスを
 * 知らないため）。
 */

void hostdrv_remote_configure(const char *host_dir);
int hostdrv_remote_is_configured(void);

/* RESET命令・MCPソフトリセット時に呼ぶ。開いているホストfd・$47/$48の検索
 * セッションを破棄する（中断された操作をリークさせないため）。 */
void hostdrv_remote_reset(void);

/*
 * MMIOレジスタウィンドウ: $EA0200-$EA020F。
 *
 *   +0x00.w MAGIC   (RO) 未設定なら 0x0000、ホストディレクトリ設定済みなら 0x4844 ('H','D')
 *   +0x02.b COMMAND (WO) 書き込みが同期実行のトリガ。0x02 = EXEC（リクエストヘッダ処理）
 *   +0x03.b STATUS  (RO) 直前コマンドの結果 (0=成功)
 *   +0x04.l REG_A5  (WO) 割り込みルーチンが書くリクエストヘッダアドレス
 *   +0x08.w REG_D0  (RO) 割り込みルーチンが d0 に返すエラーワード
 */
#define HOSTDRV_RD_BASE     0xEA0200u
#define HOSTDRV_RD_WINDOW   0x10u
#define HOSTDRV_RD_END      (HOSTDRV_RD_BASE + HOSTDRV_RD_WINDOW)
#define HOSTDRV_RD_CMD_EXEC 0x02u

typedef struct {
    uint32_t reg_a5;
    uint16_t reg_d0;
    uint8_t status;
} hostdrv_remote_t;

/* ゲストメモリアクセサ。リクエストヘッダはスーパーバイザスタック上だが、$4c _READ /
 * $4d _WRITE の転送バッファは GVRAM 等を指しうるため、RAM 直参照ではなくバス経由にする
 * （呼び出し側が x68k_bus_read8/write8 を束ねて渡す）。 */
typedef struct {
    void *ctx;
    uint8_t (*read8)(void *ctx, uint32_t addr);
    void (*write8)(void *ctx, uint32_t addr, uint8_t val);
} hostdrv_rd_mem_t;

uint8_t hostdrv_remote_reg_read8(const hostdrv_remote_t *st, uint32_t off);
void hostdrv_remote_reg_write8(hostdrv_remote_t *st, const hostdrv_rd_mem_t *mem, uint32_t off,
                               uint8_t val);
