#ifndef X68K_HOSTFILE_H
#define X68K_HOSTFILE_H

#include <stdint.h>
#include <stdio.h>

/*
 * HOSTFILE MMIO 裏口: 稼働中のエミュレータと macOS 間でファイルを1本単位で
 * やり取りする「メールボックス」。HOSTDRV（偽の FAT ボリュームを
 * 合成しHuman68kのキャッシュと衝突し得る）とは異なり、HOSTFILE.X（68k側の
 * 実行コマンド）は普通の _dos_create/_dos_write 等の DOS コールで実ドライブに
 * 書くため、Human68k 自身のバッファキャッシュと矛盾しない。
 *
 * 100MB超のファイルもホスト側RAMを圧迫しないよう、ホスト側は全体を
 * malloc せずストリーミングする:
 *   - macOS→68k方向（macOSがPUT、68kがGETで読む）: ホストは macOS の元ファイルを
 *     開いたまま (`get_src`) 保持し、68k の READ 要求ごとに fseek+fread する。
 *     ホスト側メモリは常時O(1)。
 *   - 68k→macOS方向（68kがPUT、macOSがGETで回収）: 受信データはRAMではなく
 *     一時ファイル (`put_tmp_path`) へ都度 fseek+fwrite でスプールする。
 *     macOS側の回収(`hostfile_get`)は rename(2) で完了する
 *     （同一ファイルシステム上ならデータコピー不要、EXDEV ならコピーにフォールバック）。
 *   - 中断（新規転送開始・RESET・プロセス終了）時は開いているハンドル/未回収の
 *     一時ファイルを必ず破棄する（hostfile_reset_transfer_state）。
 *
 * オフセットは HOSTFILE_BASE からのバイトオフセット。
 *
 *   +0x00.w MAGIC        (RO) 常に 0x4846 ('H','F')。裏口の存在検出用
 *                              （HOSTDRV と違い hostdrv_dir 設定に依存せず常時有効）
 *   +0x02.b COMMAND      (WO) 書き込みが同期実行のトリガ。
 *                              0x01=READ(ソースファイル→RAM,GET用)
 *                              0x02=WRITE(RAM→一時ファイル,PUT用)
 *                              0x03=PUT_BEGIN(一時ファイル新規作成) 0x04=PUT_END(確定)
 *   +0x03.b STATUS       (RO) 直前コマンドの結果（0=成功、非0=エラーコード）
 *   +0x04.l DATA_LEN     (RW) GET: host が HOSTFILE_PUT 後にセットする総バイト数（68k は RO）
 *                              PUT: 68k が全 WRITE 後にセットしてから PUT_END を撃つ
 *   +0x08.l PARAM_BUF    (WO) 転送相手の 68k RAM 絶対アドレス
 *   +0x0C.l PARAM_OFFSET (WO) ファイル内のバイトオフセット
 *   +0x10.l PARAM_COUNT  (WO) 転送バイト数
 *   +0x14.b STATE        (RO) state をそのまま返す（0=空/1=GET待ち/2=PUT待ち）
 *
 * 16bit書き込み経路の罠（bus_write_word の DEV ホワイトリスト）に注意:
 * HOSTFILE_BASE を必ずそこへ加えること（x68k_bus.c 参照）。
 */
#define HOSTFILE_BASE       0xEA0100u
#define HOSTFILE_REG_WINDOW 0x18u
#define HOSTFILE_END        (HOSTFILE_BASE + HOSTFILE_REG_WINDOW)
#define HOSTFILE_REGS_SIZE  0x15u /* 0x00-0x14: MAGIC/COMMAND/STATUS/DATA_LEN/PARAM群/STATE */
#define HF_PUT_TMP_TEMPLATE "/tmp/x68k_hostfile_XXXXXX" /* mkstemp用テンプレート(POSIX) */
/* Windowsは%TEMP%（例: C:\Users\<user>\AppData\Local\Temp\）を実行時に
 * GetTempPathW()で解決（win_get_temp_template()）するため、フルパスが収まるよう余裕を持たせる。
 */
#define HF_PUT_TMP_PATH_MAX 300

typedef struct {
    uint8_t regs[HOSTFILE_REGS_SIZE]; /* HOSTFILE MMIO 裏口: COMMAND/STATUS/PARAM_* 等 */
    FILE *get_src;    /* macOS→68k方向: macOS側ソースファイルのFILE*（state==1の間だけ開く。
                       * 68kのREAD要求ごとにfseek+freadでオンデマンド読み出し、RAM全体を
                       * 保持しない） */
    FILE *put_tmp_fp; /* 68k→macOS方向: 受信中の一時ファイルのFILE*
                       * （PUT_BEGIN〜PUT_ENDの間だけ開く。WRITE要求ごとにfseek+fwrite） */
    char put_tmp_path[HF_PUT_TMP_PATH_MAX]; /* 上記一時ファイルの実パス */
    int put_tmp_valid; /* put_tmp_path が未回収の完了済み一時ファイルを指しているか */
    uint32_t len;      /* 転送対象の総バイト数（GET方向: ソースファイルサイズ,
                        * PUT方向: 68kがPUT_ENDで確定させた総量） */
    uint8_t state;     /* 0=空, 1=GET待ち(macOSがPUT済、68kのHOSTFILE GET待ち),
                        * 2=PUT待ち(68kがPUT完了、macOS側のHOSTFILE_GET回収待ち) */
} hostfile_t;

/* hf->get_src / hf->put_tmp_fp を閉じ、未回収の一時ファイルを削除する。
 * 新規転送開始時・RESET時・プロセス終了時のいずれからも呼ぶ（中断からの復帰用）。 */
void hostfile_reset_transfer_state(hostfile_t *hf);

/*
 * HOSTFILE MMIO 裏口レジスタ read/write。x68k_bus実装には依存しない
 * （bus/bus_internal.hをincludeしない、テスト・差し替えを容易にするため）。
 * ram/ram_size は READ/WRITE コマンドの転送先/元アドレスの範囲チェックに
 * 使う 68k RAM 本体（呼び出し側の b->ram / RAM_BUF_SIZE）。
 * off は HOSTFILE_BASE からのバイトオフセット。
 */
uint8_t hostfile_reg_read8(const hostfile_t *hf, uint32_t off);
void hostfile_reg_write8(hostfile_t *hf, uint8_t *ram, uint32_t ram_size, uint32_t off,
                         uint8_t val);

/* macOS の path を開き、68k 側の HOSTFILE GET が読み出せる状態にする。
 * 成功時はファイルサイズ（0以上）を返す。失敗時は -1。詳細は x68k_bus.h の
 * x68k_bus_hostfile_put() 参照（本関数はその実体）。 */
long hostfile_put(hostfile_t *hf, const char *path);

/* 68k 側が HOSTFILE PUT で一時ファイルへ書き込み済みのデータを macOS の path へ
 * 移す。成功時は書き出したバイト数（0以上）、失敗時は負値。詳細は x68k_bus.h の
 * x68k_bus_hostfile_get() 参照（本関数はその実体）。 */
long hostfile_get(hostfile_t *hf, const char *path);

/* 転送状態を取得する（デバッグ・待ち合わせ用）。詳細は x68k_bus.h の
 * x68k_bus_hostfile_status() 参照（本関数はその実体）。 */
void hostfile_status(const hostfile_t *hf, int *state, uint32_t *len);

#endif /* X68K_HOSTFILE_H */
