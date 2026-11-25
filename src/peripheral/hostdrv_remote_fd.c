#include "hostdrv_remote_fd.h"

#include <stdint.h>

#ifdef _WIN32
#include "platform/win_compat.h"
#else
#include <unistd.h>
#endif

/* $4a _OPENが開いたホストfdをFCBアドレスに対応付ける。 */
#define HOSTDRV_MAX_FD 32

typedef struct {
    uint32_t fcb;
    int fd;
    int in_use;
} hostdrv_fd_entry_t;

static hostdrv_fd_entry_t s_fd_table[HOSTDRV_MAX_FD];

void hostdrv_fd_table_put(uint32_t fcb, int fd) {
    for (int i = 0; i < HOSTDRV_MAX_FD; i++) {
        if (!s_fd_table[i].in_use) {
            s_fd_table[i].fcb = fcb;
            s_fd_table[i].fd = fd;
            s_fd_table[i].in_use = 1;
            return;
        }
    }
    close(fd); /* 所有権を受け取ったため、登録できない場合も閉じる。 */
}

int hostdrv_fd_table_get(uint32_t fcb) {
    for (int i = 0; i < HOSTDRV_MAX_FD; i++)
        if (s_fd_table[i].in_use && s_fd_table[i].fcb == fcb)
            return s_fd_table[i].fd;
    return -1;
}

void hostdrv_fd_table_remove(uint32_t fcb) {
    for (int i = 0; i < HOSTDRV_MAX_FD; i++)
        if (s_fd_table[i].in_use && s_fd_table[i].fcb == fcb)
            s_fd_table[i].in_use = 0;
}

void hostdrv_fd_table_reset(void) {
    for (int i = 0; i < HOSTDRV_MAX_FD; i++) {
        if (s_fd_table[i].in_use) {
            close(s_fd_table[i].fd);
            s_fd_table[i].in_use = 0;
        }
    }
}
