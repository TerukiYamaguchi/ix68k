#ifndef X68K_FDC_H
#define X68K_FDC_H

#include <stdint.h>
#include <stdbool.h>
#include "bus/x68k_bus.h"
#include "peripheral/media_fp.h"

/* FDC_EXEC: READ DATA/WRITE DATAコマンドの実行データフェーズ(E-Phase)。
 * $E94003の read8/write8がこのフェーズ中はコマンド/結果ではなくセクタ
 * データバイトをやり取りする（XEiJ FDC.javaのfdcIndex/fdcLimitと同型、
 * dma_ptr/dma_remainingで表現）。 */
typedef enum { FDC_IDLE, FDC_CMD, FDC_EXEC, FDC_RESULT } fdc_phase_t;

/* 標準2HD: 77cyl x 2head x 8sec。WRITE/READ DELETED DATA(opcode 0x09/0x0C、
 * IOCS _B_WRITED/_B_READDL用)のDDAM(Deleted Data Address Mark)
 * 状態をセクタ単位のビットマップで保持するためのサイズ定義。 */
#define FDC_2HD_CYLS    77
#define FDC_2HD_HEADS   2
#define FDC_2HD_SECTORS (FDC_2HD_CYLS * FDC_2HD_HEADS * 8) /* 1232 */
#define FDC_DDAM_BYTES  ((FDC_2HD_SECTORS + 7) / 8)        /* 154 */

/* DDAMビットマップの容量。媒体プロファイルの総セクタ数（最大は2HQの80x2x18=2880）
 * を収める固定長。実際に使うビット数はドライブのプロファイルで決まる。 */
#define FDC_DDAM_MAX_SECTORS 2880
#define FDC_DDAM_MAX_BYTES   ((FDC_DDAM_MAX_SECTORS + 7) / 8) /* 360 */

/* fdc_sector_loc_t.index: DDAMの対象外（標準2HDの既存READ/WRITE DATAが範囲外CHRNを
 * 受理した場合）を表す。ddam_*は無操作になる。 */
#define FDC_SECTOR_NO_INDEX 0xFFFFFFFFu

/* 媒体プロファイル。全トラックが同一のセクタ列を持つ媒体を
 * 表す（2HTのような可変長セクタも、トラックごとに同じ2エントリが並ぶ形で表せる）。
 * ファイル内のトラック順はC昇順・物理H昇順。物理ヘッド（FDCコマンドのHD）と
 * IDフィールドのH値は別物で、後者は物理H + id_h_offset（2HDEは128）。 */
#define FDC_PROFILE_MAX_SECTORS 18

/* 媒体の転送速度と回転数。2HD系・2HC・2HQは500kbps、
 * 2DD系は250kbps。500kbps媒体は回転数で区別する: 2HD系・2HCは360rpm、2HQは300rpm。
 * 回転数はREAD/WRITE/SCAN（データ転送系）だけを照合し、READ IDは問わない
 * （実機の2HQは360rpmでもIDが読めてしまう。9SCSET.DOC）。250kbps媒体は回転数を照合しない
 * （docs/ix68k-internals/design/fdc_design.md §8）。 */
#define FDC_RATE_500_KBPS 500
#define FDC_RATE_250_KBPS 250

/* $E94005 bit3-0（Option Select / ドライブ選択ビット）。 */
#define FDC_OPTION_DRIVE_MASK 0x0Fu

typedef struct {
    uint8_t r;            /* IDのR */
    uint8_t n;            /* IDのN（バイト長は128<<N と一致する） */
    uint16_t byte_offset; /* トラック先頭からのバイトオフセット */
    uint16_t byte_length; /* セクタのバイト長 */
} fdc_sector_entry_t;

typedef struct {
    const char *name;
    uint16_t rate_kbps; /* 転送速度。ドライブの実効転送速度と一致しなければアクセスは失敗する */
    uint8_t
        rpm300; /* 1: 300rpmの媒体（2HQ）、0: 360rpm。500kbps媒体のデータ転送だけで照合する */
    uint8_t cyls;
    uint8_t heads;
    uint8_t id_h_offset;
    uint8_t
        format_boot_r; /* 0以外: FORMAT TRACKのみ、(C0,H0)の先頭セクタをこのR(H=物理ヘッド)でも
                        * 受理する（2HDEZ。9SCFMT.Xは全トラック後に(0,0)を通常配置へ
                        * 書き直すため、途中はboot_r相当の配置で書く）。読み出しには効かない */
    uint8_t boot_r;    /* 0以外: シリンダ0・物理ヘッド0の先頭セクタ（ブートIPL）のIDのR。
                        * このセクタのIDのHはオフセット無し（物理H）。9SCFMT.Xの実FORMAT TRACKで
                        * 確認（2HS: R=1、2HDE: H=0,R=1）。仕様書には記載無し */
    uint8_t sectors;   /* 1トラックのセクタ数（sec[]の有効数） */
    /* 0以外: シリンダ0・物理ヘッド0だけが別のセクタ配置を持つ（2HT。9SCFMT.Xは
     * (0,0)を2HS同様のR=1,11..18のN=3 9セクタで書く）。配置はトラックの先頭から
     * 詰め、イメージ上のトラック枠(track_bytes)の残りは未使用。DDAMはsectors個を
     * 超えるセクタには付かない（通し番号が隣のトラックと衝突するため）。 */
    uint8_t boot_track_sectors;
    fdc_sector_entry_t boot_track_sec[FDC_PROFILE_MAX_SECTORS];
    uint32_t track_bytes;
    uint32_t total_bytes; /* 受理するイメージの厳密なバイト数 */
    fdc_sector_entry_t sec[FDC_PROFILE_MAX_SECTORS];
} fdc_media_profile_t;

/* fdc_profile_locate()の結果。indexはDDAM等で使うディスク全体通しのセクタ番号。 */
typedef struct {
    uint32_t offset; /* イメージ先頭からのバイトオフセット */
    uint32_t length;
    uint32_t index;
} fdc_sector_loc_t;

/* トラック(cyl,phys_hd)の実際のセクタ配置。boot_track_sec[]の特例を反映し、
 * セクタ数を返す。*secには配列の先頭を返す。 */
int fdc_profile_track_sectors(const fdc_media_profile_t *p, int cyl, int phys_hd,
                              const fdc_sector_entry_t **sec);
/* トラック(cyl,phys_hd)のi番目のセクタのIDのR/H。boot_r・boot_track_sec[]の特例を反映する。 */
uint8_t fdc_profile_id_r(const fdc_media_profile_t *p, int cyl, int phys_hd, int i);
uint8_t fdc_profile_id_h(const fdc_media_profile_t *p, int cyl, int phys_hd, int i);

extern const fdc_media_profile_t fdc_profile_2hd;
extern const fdc_media_profile_t fdc_profile_2hs;
extern const fdc_media_profile_t fdc_profile_2hde;
extern const fdc_media_profile_t fdc_profile_2ht;
extern const fdc_media_profile_t fdc_profile_2hc;
extern const fdc_media_profile_t fdc_profile_2dd9;
extern const fdc_media_profile_t fdc_profile_2dd8;
extern const fdc_media_profile_t fdc_profile_2hq;
extern const fdc_media_profile_t fdc_profile_2hdez;

typedef struct {
    fdc_phase_t phase;
    uint8_t cmd[9];
    int cmd_idx, cmd_total;
    uint8_t res[7];
    int res_idx, res_len;
    uint8_t cur_cyl[4]; /* ドライブごとの現在シリンダ */
    uint8_t st0;        /* SENSE INTERRUPT STATUS 用 */
    uint8_t sense_cyl;
    int irq;                               /* 1 = FDC 完了割り込み pending */
    uint8_t *disk[4];                      /* XDF イメージ (NULL = 未挿入) */
    const fdc_media_profile_t *profile[4]; /* ドライブごとの媒体プロファイル (NULL = 標準2HD) */
    int disk_bytes[4];                     /* 各ドライブのイメージサイズ */
    char *disk_path[4];                    /* XDF ファイルパス (WRITE_DATA ライトバック用) */
    /* $E94007 bit4（1=2DD/2D=250kbps、0=2HD=500kbps）。X68030 Compact相当の、2DDも
     * 扱えるドライブを常に仮定する。 */
    uint8_t density_2dd;
    /* $E94005 bit3-0（Option Select）の最終書込値。ドライブNがNとN+1 mod 4の同時アサートの
     * ときだけ300rpm、それ以外は360rpm（9SCHRD.DOC）。 */
    uint8_t option_bits;
    /* READ IDが次に返すセクタの、トラック内の位置（ドライブごと）。READ IDのたびに進み、
     * トラックのセクタ数で先頭へ戻る。実機は回転に応じて次のIDを返し、9SCDRV.Xの
     * メディア判定（MedChk R-ID）は21回のREAD IDで得たRの最大値でセクタ数を知る */
    uint8_t id_cursor[4];
    int num_drives; /* 搭載ドライブ数（デフォルト 2、fd2/fd3 指定時 4） */

    /* 実行データフェーズ(FDC_EXEC)の状態。DMAC ch0 が $E94003 経由で消費する。 */
    uint8_t *dma_ptr;       /* 現在の転送位置(disk[drv]バッファ内) */
    uint32_t dma_remaining; /* 残バイト数 */
    int dma_dir;            /* OCR.DIRと同義: 1=READ(disk→host)/0=WRITE(host→disk) */
    int dma_drv;            /* 対象ドライブindex(WRITE完了時のwrite-back用) */
    uint32_t dma_offset;    /* セクタ先頭オフセット(disk[drv]内、write-back用) */
    uint32_t dma_size;      /* セクタサイズ(write-back用) */

    /* SCAN EQUAL(opcode 0x11)の比較モード。dma_dirとは直交する。
     * SCANのDMA方向はWRITE DATAと同じOCR.DIR=0（ホストメモリ→FDC、
     * IPLROMXV $FF92A0が ch0 OCR に $32 を書く）なので dma_dir は 0 のままにし、
     * 「受け取ったバイトをディスクへ書くのか、ディスクの内容と比較するだけか」
     * をこのフラグで区別する。dma_dir自体を 2 等に拡張すると
     * fdc_msr() の DIO ビット導出（dma_dir ? 0xD0 : 0x90）が壊れる。 */
    int dma_cmp;       /* 1 = SCAN比較モード（ディスクへは書き込まない） */
    int scan_mismatch; /* 比較中に1バイトでも条件不成立があった */

    /* DDAM(Deleted Data Address Mark)状態。ドライブ×セクタのビットマップ
     * (1=WRITE DELETED DATAで書かれた削除データ、0=通常データ)。XDFイメージへは
     * 書き戻さず、挿入・排出でクリアされる（永続化しない）。 */
    uint8_t ddam[4][FDC_DDAM_MAX_BYTES];

    /* ライトプロテクト(WP)状態。ドライブ単位のフラグ。挿入時にホストの
     * XDFファイルの書き込み権限(access(W_OK))から自動判定してセットし、
     * 排出でクリアする。ddamと同じく非永続化。イメージには
     * WP記録場所が無く、ホストのファイル属性が単一の真実であるため。 */
    uint8_t write_protect[4];
    uint64_t media_gen;
    bool media_gen_overflow;
    media_fp_t media_fp[4];

    /* FORMAT TRACK(opcode 0x0D)の実行データフェーズ状態。ホストがDMAで送る
     * IDタプル列(SC個 x C,H,R,N)をfmt_idsへ受け、全て揃った時点で
     * fdc_exec_complete()が検証・フィル・書き戻しを行う。 */
    int fmt_active;   /* 1 = FORMAT TRACK実行中（dma_ptrはfmt_idsを指す） */
    int fmt_unit;     /* 対象ユニット番号 */
    int fmt_hd;       /* 対象の物理ヘッド */
    int fmt_cyl;      /* 対象シリンダ（コマンド受理時のcur_cyl） */
    uint8_t fmt_fill; /* フィルバイト(D) */
    int fmt_sc;       /* コマンドのSC（IDタプル数） */
    uint8_t fmt_ids[FDC_PROFILE_MAX_SECTORS * 4];

    x68k_bus_t *bus; /* DMAC ch0汎用エンジン呼び出し用の自己参照。
                      * scsi_t.busと同じ方式、x68k_bus_create()で設定。 */
} fdc_t;

void fdc_media_changed(fdc_t *fdc);

/* プロファイルの総セクタ数（シリンダ数 x ヘッド数 x トラックあたりセクタ数）。 */
uint32_t fdc_profile_sector_count(const fdc_media_profile_t *p);
/* 要求CHRN（Cは物理シリンダと同値、Hは物理ヘッドphys_hdとID値id_h）に一致する
 * セクタを探す。見つかれば0を返しlocへ書く。範囲外・不一致は-1（locは不変）。 */
int fdc_profile_locate(const fdc_media_profile_t *p, int cyl, int phys_hd, int id_h, int r,
                       int n, fdc_sector_loc_t *loc);
void fdc_reset(fdc_t *f);
void fdc_set_density(fdc_t *f, int is_2dd);
void fdc_set_option(fdc_t *f, uint8_t val);
uint8_t fdc_msr(const fdc_t *f);
uint8_t fdc_read_data(fdc_t *f);
void fdc_write_data(fdc_t *f, uint8_t *ram, uint8_t *crtc_regs, uint8_t val);
void fdc_ddam_clear_track(fdc_t *f, int drv, int cyl, int hd);
void fdc_ddam_clear_drive(fdc_t *f, int drv);

#endif /* X68K_FDC_H */
