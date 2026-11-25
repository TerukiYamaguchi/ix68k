#include "bus/bus_internal.h"

/*
 * $EB0810 画面モードレジスタ「解像度設定」のビット配置は CRTC R20 の下位バイトと
 * 同一である（『Inside X68000』5-3: 「$EB0810番地のレジスタにはCRTCのレジスタR20の
 * 下位8ビットと同じ値を設定してください」、図56の生図でも L/H・未定義・V-RES・未定義・
 * H-RES の並び）:
 *   bit0 = H-RES (0=水平256ドット / 1=水平512ドット)
 *   bit1 = R20のH-RESフィールド上位ビット。1=768ドット/VGA系＝スプライト非対応
 *   bit2 = V-RES (0=垂直256ライン / 1=垂直512ライン)
 *   bit3 = R20のV-RESフィールド上位ビット。1=1024ライン＝スプライト非対応
 *   bit4 = L/H (水平同期周波数 15.98kHz=0 / 31.5kHz=1)
 * IPLROMの画面モード設定ルーチン($FF64E6付近)は、モードテーブル($FF65E0起点、
 * 32バイト/モード)の+0x17バイトを$EB0810へ書く。実測値は mode0(512x512)=$15、
 * mode2(256x256)=$10、mode3(256x256標準)=$00 でいずれもR20下位バイトと一致し、
 * mode16(768x512)のみ$FF(=bit1,bit3が立つ)である。
 *
 * 判定にCRTC側のレジスタは一切使わない。$EB0810はスプライトコントローラ自身の
 * レジスタであり、CRTC R20と一致するのは「ソフトがそう書く」運用規約にすぎない。
 * CRTCを直接叩く俗称「正方形ドットモード」(OTOKO.X等)はR20だけを書き換えて
 * $EB0810を256x256のまま残すため、R20で判定するとスプライトが誤って消える。
 *768x512等でSP/BG面が出ないのは、_CRTMODが$EB0810に$FFを書く
 * ことで自然に成立する。XEiJ SpriteScreen.java の sprActive/(&3)分岐と同じ設計。
 */
int x68k_bus_get_sprite_scene(const x68k_bus_t *bus, sprite_scene_t *sc) {
    const uint8_t *s = bus->sp_ram;
#define RDW(off) (((uint16_t)s[(off)] << 8) | s[(off) + 1])
    /* 本判定で使うのは下位5ビットのみ（bit5-7は未使用）。ハードが上位ビットを実際に
     * どう保持するかは未検証のため、SP RAM書込み側のマスクは変更しない
     * （判定ロジックのみに閉じたマスク）。 */
    int res = RDW(0x0810) & 0x1F;
    /* bit1/bit3 が立つのは 768ドット/VGA系・1024ライン等、スプライトコントローラが
     * 同期・生成できないモード。SP/BG面全体を出力しない
     * （XEiJ: sprActive = (sprReg8ResoCurr & 10) == 0）。 */
    if (res & 0x0A) {
        sc->pat_size = 8;
        sc->real_wh = 512;
        sc->two_bg_pages = 0;
        sc->disp_on = 0;
        sc->bg0_on = 0;
        sc->bg1_on = 0;
        return -1;
    }
    int hres = res & 1; /* bit0: 0=水平256 / 1=水平512 */
    /* V-RES(bit2)・L/H(bit4)はBGパタンサイズ・実画面サイズ・BG面数に影響しない。
     * 512x256混在モードは512x512と同じ16x16パタン/実画面1024/BG0のみである
     * （『Inside X68000』本文「表示画面の水平512ドットモード…のときにはBG0画面のみが
     *   表示され、BG1画面は水平256ドットモードのときにだけ表示されます」、
     *   XEiJ SpriteScreen.java sprStep2 の (sprReg8ResoCurr & 3) 分岐）。 */
    if (hres) {
        sc->pat_size = 16;
        sc->real_wh = 1024;
        sc->two_bg_pages = 0;
    } else {
        sc->pat_size = 8;
        sc->real_wh = 512;
        sc->two_bg_pages = 1;
    }
    int ctrl = RDW(0x0808);
    sc->disp_on = (ctrl & 0x0200) ? 1 : 0;                      /* bit9 DISP/CPU */
    sc->bg0_on = (ctrl & 0x0001) ? 1 : 0;                       /* bit0 BG0 ON */
    sc->bg1_on = ((ctrl & 0x0008) && sc->two_bg_pages) ? 1 : 0; /* bit3 BG1 ON */
    sc->bg0_base_off =
        (ctrl & 0x0002) ? SP_BGDATA1_OFF : SP_BGDATA0_OFF; /* bit2-1 BG0 TXSEL下位 */
    sc->bg1_base_off =
        (ctrl & 0x0010) ? SP_BGDATA1_OFF : SP_BGDATA0_OFF; /* bit5-4 BG1 TXSEL下位 */
    sc->bg0_sx = RDW(0x0800) & 0x3FF;
    sc->bg0_sy = RDW(0x0802) & 0x3FF;
    sc->bg1_sx = RDW(0x0804) & 0x1FF;
    sc->bg1_sy = RDW(0x0806) & 0x1FF;
#undef RDW
    return 0;
}
