/* バスやCPUに依存しないYM2151合成コアの単体テスト。 */
#include "audio/ym2151.h"
#include <assert.h>
#include <stdio.h>

static uint32_t sample_hash(const int32_t *buf, int count) {
    uint32_t hash = 2166136261u;
    for (int i = 0; i < count; i++) {
        uint32_t value = (uint32_t)buf[i];
        for (int shift = 0; shift < 32; shift += 8) {
            hash ^= (value >> shift) & 0xffu;
            hash *= 16777619u;
        }
    }
    return hash;
}

int main(void) {
    ym2151_t core;

    /* A. 無音: リセット直後はKEYONされていないため全チャンネル無音 */
    ym2151_reset(&core);
    {
        int32_t buf[64 * 2];
        ym2151_generate(&core, buf, 64);
        for (int i = 0; i < 64 * 2; i++)
            assert(buf[i] == 0);
    }

    /* B. 単音: チャンネル0、CON=7(全オペレータがoutに直接加算されるため
     * op1のみキーオンすれば単一オペレータのキャリアになる)、op1(絶対
     * オペレータ番号0、s_fixed_map[0]の第1バイト)にTL=0(最大音量)・
     * AR=31(瞬時アタック)を設定し、KEYONする。 */
    ym2151_reset(&core);
    ym2151_write(&core, 0x20, 0xC7); /* ch0: pan L+R on, feedback=0, algorithm(CON)=7 */
    ym2151_write(&core, 0x60, 0x00); /* op1(abs#0): TL=0(最大音量) */
    ym2151_write(&core, 0x80, 0x1F); /* op1(abs#0): AR=31(瞬時アタック), KSR=0 */
    ym2151_write(&core, 0x08, 0x08); /* KEYON: ch=0, opmask=bit0(op1)のみ */

    {
        int32_t buf[64 * 2];
        ym2151_generate(&core, buf, 64);

        int nonzero = 0;
        for (int i = 0; i < 64; i++) {
            int32_t l = buf[i * 2 + 0];
            int32_t r = buf[i * 2 + 1];
            /* パン両ON・単一チャンネルのみアクティブなためL=Rが常に成立する */
            assert(l == r);
            if (l != 0)
                nonzero = 1;
        }
        assert(nonzero); /* 少なくとも1サンプルは非0(発音していること) */
        /* 波形全体を固定し、個別サンプルでは見つけにくい変化を検出する。 */
        assert(sample_hash(buf, 64 * 2) == 0x152B1701u);
    }

    /* タイマを始動していないためTimerA/Bフラグは立たない。 */
    assert(ym2151_status(&core) == 0);

    printf("ym2151_gen_test: OK\n");
    return 0;
}
