#ifndef X68K_HOSTDRV_REMOTE_FD_H
#define X68K_HOSTDRV_REMOTE_FD_H

#include <stdint.h>

/* fdの所有権をテーブルへ移す。空きがなければfdを閉じる。 */
void hostdrv_fd_table_put(uint32_t fcb, int fd);
int hostdrv_fd_table_get(uint32_t fcb);
/* 対応だけを削除する。fdは呼び出し側が閉じる。 */
void hostdrv_fd_table_remove(uint32_t fcb);
/* 登録中のfdをすべて閉じ、対応を削除する。 */
void hostdrv_fd_table_reset(void);

#endif /* X68K_HOSTDRV_REMOTE_FD_H */
