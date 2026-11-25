#ifndef X68K_CRTC_H
#define X68K_CRTC_H

#include <stdint.h>

/*
 * CRTC ラスターモデル。走査線位置をCPUサイクル基準でドリフト無く追跡する。
 * 旧mfp_scanline/mfp_scanline_frac（擬似走査線カウンタ）を置き換え、MFPは
 * 判定のみに専念する。
 */
typedef struct {
    int line;             /* 現在の絶対ラスタ番号 0..lines_per_frame-1（0 = 垂直同期開始） */
    int lines_per_frame;  /* R04+1 由来。異常時は567（31.5kHz標準相当） */
    int vdisp_start;      /* 垂直映像期間開始ラスタ = R06+1 */
    int vdisp_lines;      /* 映像期間のラスタ数 = R07 - R06 */
    int dup;              /* ラスタ2度読み係数 1 or 2（vdisp_lines / 表示高さ） */
    int display_h;        /* このフレームの表示高さ（frame_start で確定） */
    int valid;            /* 1 = ラスタ同期描画に使える。0 = 従来の一括合成へフォールバック */
    int cycles_per_frame; /* このフレームのCPUサイクル数（cpu_hz/60） */
    int cycle_in_frame;   /* フレーム先頭からの累積CPUサイクル */

    /* 垂直映像期間開始ラスタで一度だけラッチする縦スクロール値。 */
    int y_latched;    /* 1 = このフレームでラッチ済み */
    int tx_y_zero;    /* R11 テキストYスクロール */
    int gr_y_zero[4]; /* R13/R15/R17/R19 の順 */
} x68k_raster_t;

typedef struct {
    x68k_raster_t
        raster;          /* CRTCラスターモデル。走査線位置はここで一元管理し、
                          * MFPはmfp_raster_line_event()経由で通知を受けて割り込み判定のみ行う。 */
    int raster_y_live;   /* 1 = 縦スクロール(R11/R13等)をラッチせず常にライブ参照する
                          * （-raster-y live起動オプションの逃げ道。既定0=ラッチ）。 */
    int raster_pal_live; /* 1 = パレットを各ラスタでライブ参照する（既定0=V-DISPラッチ）。 */
    int pal_latched;     /* 現フレームのV-DISP開始でパレットをsnapshot済み。 */
    int pal_dirty;       /* 前回snapshot後にlive palette RAMが更新された。 */
    int pal_cache_ready; /* bus write経由のlive cacheが初期化済み。 */
    unsigned pal_vdisp_gfx_writes; /* 表示期間中のpalette byte write数（互換性監視用）。 */
    unsigned pal_vdisp_text_writes;
    /* CRTC動作ポート bit1（高速クリア）の完了フラグ読み出し用の保留状態。
     * トリガ書き込み直後は crtc_regs 上の bit1 を 1 のまま残し、このフラグを立てる。
     * ポートが次に読まれた瞬間だけ bit1=1 を返しつつ crtc_regs 側の bit1 とこの
     * フラグを両方 0 に落とす（「トリガ後まずbit1が1になったことを確認してから
     * 0を待つ」という2段階ポーリングを行うソフト対策）。 */
    int rapid_clear_pending;
    /* パレットのARGB8888変換キャッシュ。CRTC書き込みパス
     * （x68k_bus_write8のCRTC分岐）で該当エントリのみeagerに更新する。
     * x68k_bus_create()でGRB555=0相当の0xFF000000で初期化しておくこと。 */
    uint32_t gfx_pal_argb[256]; /* $E82000-$E821FF（256エントリ、GVRAM用） */
    uint16_t gfx_pal_raw[256];
    uint32_t gfx_pal_argb_latched[256];
    uint16_t gfx_pal_raw_latched[256];
    /* $E82200-$E823FF（256エントリ、512バイト）。『Inside X68000』 図33(p.218)の通り
     * テキスト画面とスプライト+BG画面が共用する1本のパレットRAM（従来16エントリ分
     * しか確保しておらず、スプライト/BG用のブロック1-15
     * （エントリ16-255、_SPALETが書き込む範囲）を保持できていなかった）。
     * テキストは常にエントリ0-15(ブロック0)だけを参照する。 */
    uint32_t text_pal_argb[256];
    uint16_t text_pal_raw[256];
    uint32_t text_pal_argb_latched[256];
    uint16_t text_pal_raw_latched[256];
    /* システムポート#1 ($E8E001) コントラストの描画状態。attenuation は
     * 0..45（0=最明、45=最暗）の1/3段階単位で、ゼロ初期化したテスト用busも
     * 最明になるよう減衰量として保持する。LUTは描画値(45-attenuation)が
     * 変わったときだけ再構築する。contrast_lut_scaled=-1は未構築を表す。 */
    uint8_t contrast_attenuation;
    uint8_t contrast_lut[256];
    int contrast_lut_scaled;
} crtc_t;

#endif /* X68K_CRTC_H */
