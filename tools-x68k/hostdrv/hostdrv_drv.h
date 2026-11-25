#ifndef _HOSTDRV_DRV_H_
#define _HOSTDRV_DRV_H_

#include <stdint.h>

/* Human68k「リモートデバイス」共通リクエストヘッダ（26バイト。初期化 comcod $40 のみ
 * reqlen=22 で末尾4バイトは未使用）。
 * docs/ix68k-internals/design/hostdrv_design.md §2 で確定したレイアウト。 */
struct hostdrv_req_header {
    uint8_t reqlen;  /* +0x00.b */
    uint8_t unit;    /* +0x01.b */
    uint8_t comcod;  /* +0x02.b bit7 = VERIFY フラグ。分岐前に & 0x7f すること */
    uint8_t errl;    /* +0x03.b デバイスドライバエラー下位（非0で中断/再実行ダイアログ） */
    uint8_t errh;    /* +0x04.b デバイスドライバエラー上位（対処方法ビット） */
    uint8_t reserved[8]; /* +0x05    未使用 */
    uint8_t p13;     /* +0x0d.b コマンド依存 */
    uint32_t p14;    /* +0x0e.l コマンド依存 */
    uint32_t p18;    /* +0x12.l コマンド依存の入力、かつリザルトステータス（DOS戻り値）出力 */
    uint8_t drive_no; /* +0x16.b 初期化時にHuman68kが割り当てたドライブ番号（A:=0） */
} __attribute__((packed, aligned(2)));

#endif /* _HOSTDRV_DRV_H_ */
