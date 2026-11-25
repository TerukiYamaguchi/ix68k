#include <stdint.h>
#include <x68k/dos.h>
#include "hostdrv_drv.h"

/* HOSTDRV.SYS: Human68k「リモートデバイス」（属性ワード bit13）としてホスト macOS の
 * ディレクトリを公開する薄いブリッジドライバ。業務ロジックは一切持たず、リクエスト
 * ヘッダのアドレスをエミュレータの MMIO 裏口（0xEA0200-、src/peripheral/hostdrv_remote.c
 * 参照）へ渡すだけ。ファイル系 DOS コールの実処理（open/read/readdir 等への変換）は
 * すべてホスト側で行う。
 *
 * 初期化 comcod $40 だけはここで完結させる。ドライバ末尾アドレス（+14 出力）はリンク時
 * に決まる自シンボル &_end が必要で、エミュレータ側はデバイスヘッダのアドレスを知らない
 * ため。 */

struct hostdrv_req_header *reqheader;

/* Shift_JIS: "\r\nHOSTDRV Ver.1.0: F: をホスト共有ドライブに割り当てました。\r\n"
 * 添字 19 の F を、初期化時に Human68k が割り当てたドライブ文字へ置き換える。 */
static uint8_t hostdrv_init_message[] = {
    '\r', '\n', 'H', 'O', 'S', 'T', 'D', 'R', 'V', ' ', 'V', 'e', 'r', '.', '1', '.', '0', ':', ' ', 'F', ':', ' ',
    0x82, 0xf0, 0x83, 0x7a, 0x83, 0x58, 0x83, 0x67, 0x8b, 0xa4,
    0x97, 0x4c, 0x83, 0x68, 0x83, 0x89, 0x83, 0x43, 0x83, 0x75,
    0x82, 0xc9, 0x8a, 0x84, 0x82, 0xe8, 0x93, 0x96, 0x82, 0xc4,
    0x82, 0xdc, 0x82, 0xb5, 0x82, 0xbd, 0x81, 0x42, '\r', '\n', '\0'
};

#define HOSTDRV_INIT_MESSAGE_DRIVE_INDEX 19u

/* Shift_JIS: "\r\nHOSTDRV: ix68k 以外では組み込めません。\r\n" */
static const uint8_t hostdrv_not_ix68k_message[] = {
    '\r', '\n', 'H', 'O', 'S', 'T', 'D', 'R', 'V', ':', ' ', 'i', 'x', '6', '8', 'k', ' ',
    0x88, 0xc8, 0x8a, 0x4f, 0x82, 0xc5, 0x82, 0xcd, 0x91,
    0x67, 0x82, 0xdd, 0x8d, 0x9e, 0x82, 0xdf, 0x82, 0xdc, 0x82, 0xb9, 0x82,
    0xf1, 0x81, 0x42, '\r', '\n', '\0'
};

/* Shift_JIS: "\r\nHOSTDRV: ホストディレクトリが未設定のため組み込みません。\r\n" */
static const uint8_t hostdrv_no_dir_message[] = {
    '\r', '\n', 'H', 'O', 'S', 'T', 'D', 'R', 'V', ':', ' ',
    0x83, 0x7a, 0x83, 0x58, 0x83, 0x67, 0x83, 0x66, 0x83, 0x42, 0x83, 0x8c, 0x83, 0x4e, 0x83,
    0x67, 0x83, 0x8a, 0x82, 0xaa, 0x96, 0xa2, 0x90, 0xdd, 0x92, 0xe8, 0x82,
    0xcc, 0x82, 0xbd, 0x82, 0xdf, 0x91, 0x67, 0x82, 0xdd, 0x8d, 0x9e, 0x82,
    0xdd, 0x82, 0xdc, 0x82, 0xb9, 0x82, 0xf1, 0x81, 0x42, '\r', '\n', '\0'
};

extern void hostdrv_b_print(const uint8_t *message);
extern uint32_t hostdrv_probe_magic(void);

#define HOSTDRV_RD_BASE 0xEA0200u

#define HOSTDRV_RD_MAGIC   (*(volatile uint16_t *)(HOSTDRV_RD_BASE + 0x00))
#define HOSTDRV_RD_COMMAND (*(volatile uint8_t *)(HOSTDRV_RD_BASE + 0x02))
#define HOSTDRV_RD_REG_A5  (*(volatile uint32_t *)(HOSTDRV_RD_BASE + 0x04))
#define HOSTDRV_RD_REG_D0  (*(volatile uint16_t *)(HOSTDRV_RD_BASE + 0x08))

#define HOSTDRV_RD_CMD_EXEC 0x02u
#define HOSTDRV_RD_MAGIC_OK 0x4844u /* 'H','D': ホストディレクトリ設定済み */
#define HOSTDRV_PROBE_BUS_ERROR 0xFFFFFFFFu /* hostdrv_probe_magic(): $EA0200 がバスエラー */
/* 初期化の拒否に使うデバイスエラー（errh: 中止＆再実行＆無視、errl: その他のエラー） */
#define HOSTDRV_INIT_ERR_HIGH 0x70u
#define HOSTDRV_INIT_ERR_LOW  0x0Cu
#define HOSTDRV_DOS_ERR_INVALID_DRIVE (-15) /* Human68k DOS エラー: 無効なドライブ指定 */

/* 初期化で ix68k + ホストディレクトリ設定済みと確認できたとき 1。0 の間は MMIO に触れない。 */
static uint8_t hostdrv_active __attribute__((section(".data"))); /* .bss はロード時に 0 保証がないため .data に置く */

int interrupt(void) {
    struct hostdrv_req_header *req = reqheader;
    uint8_t comcod = req->comcod & 0x7f;

    if (comcod == 0x40) {
        extern char _end;
        int result = 0;

        uint32_t magic = hostdrv_probe_magic();

        if (magic != HOSTDRV_RD_MAGIC_OK) {
            /* ix68k 以外（バスエラー）またはホストディレクトリ未設定: メッセージを出し、
             * 初期化エラーで組み込みを拒否する。ユニット数 0 の「正常終了」を返してはならない:
             * Human68k は devend 以降へ 65536 個（約 768KB）の
             * ドライブ管理ノードを作る（実測。DBRA に 0-1 が渡る挙動と推定）。以後ドライブリストを辿る処理
             * （DRIVE.X など）が 65536 回ドライバを呼び、数秒かかる。
             * デバイスヘッダは Human68k がチェーンへ繋ぎ済みのため、末尾アドレスは通常時と
             * 同じ &_end にして解放しない（先頭にするとチェーンが壊れる）。 */
            hostdrv_b_print(magic == HOSTDRV_PROBE_BUS_ERROR ? hostdrv_not_ix68k_message
                                                             : hostdrv_no_dir_message);
            req->p13 = 0;
            req->p14 = (uint32_t)&_end;
            req->p18 = 0;
            req->errl = HOSTDRV_INIT_ERR_LOW;
            req->errh = HOSTDRV_INIT_ERR_HIGH;
            result = (HOSTDRV_INIT_ERR_HIGH << 8) | HOSTDRV_INIT_ERR_LOW;
        } else {
            /* 初期化時の +0x16 は Human68k が割り当てたドライブ番号（A:=0）。 */
            uint8_t drive_no = req->drive_no;

            req->p13 = 1;               /* ユニット数 */
            req->p14 = (uint32_t)&_end; /* ドライバ末尾アドレス */
            req->p18 = 0;                /* BPBテーブルポインタ（リモートでは無視される） */
            req->errl = 0;
            req->errh = 0;
            hostdrv_active = 1;
            hostdrv_init_message[HOSTDRV_INIT_MESSAGE_DRIVE_INDEX] = (uint8_t)('A' + drive_no);
            hostdrv_b_print(hostdrv_init_message);
        }
        /* DISK2HD 暴走対策: ブート経路では初期化リクエストヘッダが直後に再利用される
         * ため、comcod を明示的に 0 へ書き戻す（CONFIG.SYS 経路では不要だがコストゼロ
         * なので常に入れる）。 */
        req->comcod = 0x00;
        return result;
    }

    if (!hostdrv_active) {
        /* 組み込み拒否後も Human68k は他コマンドを発行しうる。$EA0200- は ix68k 以外では
         * バスエラーになるため MMIO に触れず、DOS エラー「無効なドライブ」を返す。 */
        req->p18 = (uint32_t)HOSTDRV_DOS_ERR_INVALID_DRIVE;
        req->errl = 0;
        req->errh = 0;
        return 0;
    }

    /* 初期化以外は MMIO 経由でホスト側(hostdrv_remote.c)へ丸投げする。
     * errl/errh/p18 等の結果はホスト側がリクエストヘッダへ直接書き込む。 */
    HOSTDRV_RD_REG_A5 = (uint32_t)req;
    HOSTDRV_RD_COMMAND = HOSTDRV_RD_CMD_EXEC;
    return HOSTDRV_RD_REG_D0;
}

void _start(void) {}
