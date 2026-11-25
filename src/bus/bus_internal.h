#pragma once
/*
 * bus_internal.h — x68k_bus 実装ファイル間で共有する内部定義。
 * 公開 API は bus/x68k_bus.h。ここには struct x68k_bus 本体・アドレスマップ
 * 定数・ファイルをまたぐ内部関数宣言のみを置く。
 * x68k_bus.c / sram.c（bus/）、fdc.c / mfp.c / dmac.c / crtc.c / rtc.c / scsi.c /
 * sasi.c / scc.c / opm.c / sprite.c（peripheral/）からのみ include すること。
 */
#include "bus/x68k_bus.h"
#include "peripheral/scsi.h"
#include "peripheral/sasi.h"
#include "peripheral/scsi_rom_data.h"
#include "peripheral/scc.h"
#include "peripheral/adpcm.h"
#include "peripheral/opm.h"
#include "peripheral/fdc.h"
#include "peripheral/hostdrv_remote.h"
#include "peripheral/hostfile.h"
#include "peripheral/dmac.h"
#include "peripheral/crtc.h"
#include "peripheral/mfp.h"
#include "peripheral/rtc.h"
#include "bus/guest_clock.h"
#include "mc68000/cpu_types.h"
#include "mc68000/fpu.h"
#include "bus/fpu_cir.h"
#include <stdio.h> /* FILE*（HOSTFILE MMIO裏口のストリーミング用） */

/*
 * X68000 アドレスマップ（24 bit / 16MB）
 *
 *   0x000000–0x0FFFFF : メイン RAM（1MB）
 *   0xC00000–0xDFFFFF : GVRAM（2MB）
 *   0xE00000–0xE7FFFF : TVRAM（512KB）
 *   0xE80000–0xE87FFF : CRTC レジスタ
 *   0xE88001–0xE8802F : MFP (MC68901)（奇数アドレスのみ）
 *   0xE88031–0xE8A000未満 : CRTC 残り
 *   0xE8A001–0xE8A01F : RTC (RP5C15)（奇数アドレスのみ。偶数バイトアクセスは
 *                        バスエラー。ミラー($E8A020-$E8AFFF)は未実装）
 *   0xE8A020–0xE8FFFF : CRTC 残り
 *   0xE90000–0xE91FFF : OPM FM音源（$1B=CT1/CT2はadpcm.cが実装。レジスタ選択/
 *                        データポート・ステータス・タイマはopm.c、波形合成は
 *                        audio/ym2151.cが実装）
 *   0xE92000–0xE93FFF : PCM ADPCM音源(MSM6258)
 *   0xE94000–0xE95FFF : FDC (µPD72065B)
 *   0xE96000–0xE97FFF : SASI HDC / 内蔵SCSI SPC
 *   0xE98000–0xE99FFF : SCC（ch A=RS-232C、ch B=マウス）
 *   0xE9A000–0xE9BFFF : PPI（ポートA/B=ジョイスティック、ポートC=ADPCM/FDC制御）
 *   0xE9C000–0xE9DFFF : IOI I/O割り込みコントローラ
 *   0xE9E000–0xE9FFFF : XB1 拡張ボード領域1（未接続時はバスエラー）
 *   0xF00000–0xFFFFFF : IOCS ROM
 */

#define RAM_SIZE         0x100000u
#define GVRAM_SIZE       0x200000u
#define TVRAM_SIZE       0x080000u
#define TVRAM_PLANE_SIZE 0x20000u /* TVRAM 1プレーン分（4プレーン×このサイズ） */
#define CRTC_SIZE        0x010000u
#define IOCS_ROM_SIZE    0x100000u

#define RAM_BASE   0x000000u
#define GVRAM_BASE 0xC00000u
#define TVRAM_BASE 0xE00000u
#define CRTC_BASE  0xE80000u

/* システムポート#4 ($E8E00B, 機種ID)。IPLROM 1.6 は $DC のときだけ "X68030" と表示する。
 * X68030 系は $DC、それ以外は 10MHz 以下で $FF、超で $FE（XEiJ と同じ規則）。 */
#define SYSPORT4_OFFSET      0x0Bu
#define SYSPORT_MIRROR_MASK  0x0Fu
#define SYSPORT4_ID_X68030   0xDCu
#define SYSPORT4_ID_SLOW     0xFFu
#define SYSPORT4_ID_FAST     0xFEu
#define SYSPORT4_SLOW_MAX_HZ 10000000u

/* システムポート#6 ($E8E00F, Power OFF Control)。前面スイッチOFFのとき
 * $00,$0F,$0F の順にバイト書込みすると本体電源がOFFになる。 */
#define SYSPORT6_ADDR          0xE8E00Fu
#define SYSPORT6_VAL_FIRST     0x00u
#define SYSPORT6_VAL_NEXT      0x0Fu
#define SYSPORT6_POWEROFF_SEQ0 0u /* 待機 */
#define SYSPORT6_POWEROFF_SEQ1 1u /* $00 受理 */
#define SYSPORT6_POWEROFF_SEQ2 2u /* $00,$0F 受理 */
/* RTC (RP5C15): CRTC_BASE-CRTC_SIZE の内側($E8A000-$E8BFFF、奇数アドレスのみ)。
 * MFP同様、read8/write8のCRTC分岐内で専用デコードする。 */
#define RTC_BASE               0xE8A000u
#define RTC_SIZE               0x002000u
/* 0xE90000–0xE9FFFF: 各デバイス 8KB ずつ（XEiJ MMD 配置準拠） */
#define OPM_BASE               0xE90000u /* OPM FM音源 */
#define PCM_BASE               0xE92000u /* PCM ADPCM音源 */
#define FDC_BASE               0xE94000u /* FDC (µPD72065B) */
#define FDC_DRVSEL_2DD_BIT     0x10u     /* $E94007 bit4: 1=2DD/2D(250kbps)、0=2HD(500kbps) */
#define HDC_BASE               0xE96000u /* HDC SASI */
#define SPC_BASE 0xE96020u /* 内蔵SCSI SPC(MB89352)レジスタ, XEiJ SPC_BASE_IN準拠 */
#define SPC_SIZE 0x000020u
#define SCC_BASE 0xE98000u /* SCC */
#define SCC_SIZE 0x000008u /* ch B(コマンド/データ)+ch A(コマンド/データ)、各word幅 */
#define PPI_BASE 0xE9A000u /* PPI */
#define PPI_PC_JOY1_DISABLE     0x10u     /* PPIポートC bit4: ジョイスティック#1操作禁止 */
#define PPI_PC_JOY2_DISABLE     0x20u     /* PPIポートC bit5: ジョイスティック#2操作禁止 */
#define PPI_PC_JOY1_TRG_A_FORCE 0x40u     /* PPIポートC bit6(IOA5): #1トリガA信号をLowにする */
#define PPI_PC_JOY1_TRG_B_FORCE 0x80u     /* PPIポートC bit7(IOA6): #1トリガB信号をLowにする */
#define JOY_BIT_TRG_A           0x20u     /* ポートA/B bit5: トリガA（アクティブロー） */
#define JOY_BIT_TRG_B           0x40u     /* ポートA/B bit6: トリガB（アクティブロー） */
#define JOY_PORT_RELEASED       0xFFu     /* ジョイスティック未接続/無操作（アクティブロー） */
#define IOI_BASE                0xE9C000u /* IOI I/O割り込み */
#define XB1_BASE                0xE9E000u /* XB1 拡張ボード領域1 */
#define FPU_CIR_PRIMARY_BASE    XB1_BASE  /* CZ-6BP1/6BP1A 標準番地 */
#define FPU_CIR_SECONDARY_BASE  0xE9E080u /* ジャンパ切替のもう一つの window */
#define FPU_CIR_SIZE            0x000020u /* CIR window は32 byte */

/*
 * 0xEA0000–0xEAFFFF: XB2 拡張ボード領域。
 * 未接続部分への CPU アクセスはバスエラーにする。HOSTDRV/HOSTFILE の裏口と
 * 0xEAFFEB（RTC テストバイト）はバス層が個別に処理する。
 */
#define DEV_BASE 0xEA0000u

#define DEV_END 0xEB0000u

/*
 * 0xEB0000–0xEBFFFF: スプライトコントローラ（スプライトスクロールレジスタ・
 * BGスクロール/コントロール・画面モードレジスタ・PCGエリア・BGデータエリア）。
 * 実機のレジスタ配置は『Inside X68000』図10/図18/図19を参照。
 */
#define SPRITE_BASE 0xEB0000u
#define SPRITE_SIZE 0x010000u                   /* 64KB: 0xEB0000–0xEBFFFF */
#define SPRITE_END  (SPRITE_BASE + SPRITE_SIZE) /* 0xEC0000 */
#define SP_PCG_OFF                                                              \
    0x8000u /* PCGエリア（8x8共用/16x16用+16x16専用）先頭のsp_ram内オフセット \
             */
#define SP_BGDATA0_OFF 0xC000u /* BGデータエリア0 先頭オフセット */
#define SP_BGDATA1_OFF 0xE000u /* BGデータエリア1 先頭オフセット */

/*
 * 0xED0000–0xED3FFF: SRAM（実機仕様通り 16KB）。
 *
 * 0xEB0000–0xEBFFFFのスプライト/PCG/BG領域とは別のバッキングを持つ。
 * 0xEC0000–0xECFFFF は XB3 拡張ボード領域で、未接続時はバスエラーになる。
 */
#define SRAM_BASE     0xED0000u
#define SRAM_SIZE     0x004000u /* 16KB: 0xED0000–0xED3FFF */
#define IOCS_ROM_BASE 0xF00000u

/* iocs_rom バッファ内の各領域オフセット（IOCS_ROM_BASE 起点）。
 * CGROM は 768KB（$F00000-$FBFFFF）、128KB IPLROM は $FE0000-$FFFFFF を
 * バッファ末尾側（IPLROM_BUF_OFFSET）へ置く。$FC0000-$FDFFFF は SCSI ROM 窓。 */
#define CGROM_SIZE         0x0C0000u
#define IPLROM_ADDR_OFFSET 0x0E0000u /* $FE0000 - IOCS_ROM_BASE */
#define IPLROM_BUF_OFFSET  0x0C0000u
#define IPLROM_SIZE        0x020000u

/* バスアドレス(>= IOCS_ROM_BASE)を iocs_rom バッファ内オフセットへ変換する。
 * CGROM 領域はそのまま、$FE0000 以降の IPLROM は IPLROM_BUF_OFFSET へ写す。 */
static inline uint32_t iocs_rom_offset(uint32_t addr) {
    uint32_t off = addr - IOCS_ROM_BASE;
    if (off >= IPLROM_ADDR_OFFSET)
        off = IPLROM_BUF_OFFSET + (off & (IPLROM_SIZE - 1u));
    return off;
}

/* 実機 SRAM 実体（0xED0000-0xED3FFF, 16KB）の b->sram 内オフセット。
 * b->sram は SRAM_BASE (0xED0000) 起点で確保しているためオフセット 0。 */
#define SRAM_REAL_OFFSET 0x000000u

/*
 * MC68030内蔵キャッシュ性能近似（システムポート#5 $E8E009）。
 * ここの定数はすべて「命令の公称サイクルへ上乗せする有効ペナルティ」の
 * 較正値であり、実機の生ウェイト数そのものではない
 * （docs/ix68k-internals/design/cpu_design.md §2 参照）。
 */
#define X68K_WAIT_SCALE_NUM         1 /* 有効ペナルティ = 生ウェイト * NUM / DEN */
#define X68K_WAIT_SCALE_DEN         1
#define X68K_WAIT_MAX               31 /* 上限クランプ（暴走的な減速を防ぐ） */
/* DRAM 設定 $0（スタティックカラム）の1アクセスあたり平均ペナルティ。
 * 実機はページ内 0 / ページ跨ぎ 4 だが、ページ状態は追跡しない。
 * X68030 の通常起動経路（IPLROM30 $FF0144）はこの設定 $0 で動くため、
 * ここを大きくすると既定構成の全 RAM アクセスへ恒常的に最悪値を課すことに
 * なる。実機ベンチで有意な比が確認できるまでは 0 のまま動かさない。 */
#define X68K_RAM_WAIT_STATIC_COLUMN 0

/* システムポート#5 ($E8E009) から導出した、RAM/ROM 1バスサイクルあたりの
 * 有効ペナルティ。x68k_bus_apply_wait_port() が書込みのたびに再計算する。 */
typedef struct {
    uint8_t rom;  /* ROM 1バスサイクルあたりの有効ペナルティ */
    uint8_t ram;  /* DRAM 1バスサイクルあたりの有効ペナルティ */
    uint8_t mmio; /* 常に 0（MMIO は課金しない） */
} x68k_wait_profile_t;

typedef enum {
    X68K_WAIT_REGION_NONE,
    X68K_WAIT_REGION_RAM,
    X68K_WAIT_REGION_ROM,
    X68K_WAIT_REGION_SRAM,
    X68K_WAIT_REGION_GVRAM,
    X68K_WAIT_REGION_TVRAM,
    X68K_WAIT_REGION_CRTC,
    X68K_WAIT_REGION_VC_PALETTE,
    X68K_WAIT_REGION_VC_MODE,
    X68K_WAIT_REGION_DMAC,
    X68K_WAIT_REGION_MFP,
    X68K_WAIT_REGION_RTC,
    X68K_WAIT_REGION_PRN,
    X68K_WAIT_REGION_SYS,
    X68K_WAIT_REGION_OPM,
    X68K_WAIT_REGION_ADPCM,
    X68K_WAIT_REGION_FDC,
    X68K_WAIT_REGION_HDC,
    X68K_WAIT_REGION_SCC,
    X68K_WAIT_REGION_PPI,
    X68K_WAIT_REGION_IOI,
    X68K_WAIT_REGION_SPRITE_REG,
    X68K_WAIT_REGION_SPRITE_VRAM,
    X68K_WAIT_REGION_COUNT,
} x68k_wait_region_t;

typedef struct {
    uint16_t read;
    uint16_t write;
} x68k_wait_pair_t;

typedef struct {
    x68k_wait_pair_t region[X68K_WAIT_REGION_COUNT];
    uint8_t long_factor;
    bool ram_rom_from_port5;
} x68k_wait_table_t;

/* RAM バッキング配列の実サイズ。実機は最大12MB（本体標準+拡張、$C00000
 * = GVRAM直前まで）搭載可能なため、その最大値ぶん確保しておき、実際に
 * 「搭載済み」とみなす範囲は ram_limit() が SRAM $ED0008（SWITCH.X MEMORY、
 * 「C compiler PRO-68K ver2.0 プログラマーズマニュアル」付録）を見て動的に決める。 */
#define RAM_BUF_SIZE 0xC00000u

/* IOI (I/O Interrupt Controller) 定数 — XEiJ IOInterrupt.java 準拠 */
#define IOI_FDC_SIGNAL  0x80u /* デバイス割り込み信号ビット (status reg 上位4bit) */
#define IOI_FDD_SIGNAL  0x40u
#define IOI_PRN_SIGNAL  0x20u /* プリンタ割り込み信号 (RESET後にアサートされる) */
#define IOI_FDC_REQUEST 0x04u /* CPU 割り込み要求ビット */
#define IOI_FDD_REQUEST 0x02u
#define IOI_FDC_VECTOR  0x00u /* ベクタオフセット */
#define IOI_FDD_VECTOR  0x01u
/* SPC (内蔵SCSI) 割り込み: FDC/FDD と違いベクタ$6C固定・常時許可・最優先。
 * ioi_signal/enable/request の8bit
 * ビットパック方式とは別に、独立フラグ(spc_irq_pending)で管理する
 * （実機でも別チップ・別優先度経路のため設計上の単純化として妥当）。 */
#define SPC_IRQ_VECTOR  0x6Cu

/* _B_PUTMES キャプチャリング（詳細は struct x68k_bus 内コメント参照）。
 * col/row の最大値はIOCS引数仕様(0-127/0-63)に合わせる。 */
#define PUTMES_RING_SIZE      32
#define PUTMES_TEXT_MAX       128
/* 1文字セルのTVRAM生バイト数: 4プレーン × 16スキャンライン。 */
#define PUTMES_CELL_RAW_BYTES 64
typedef struct {
    uint8_t valid;
    /* 0 = 描画後スナップショットをまだ取っていない（記録が起きたフレームの
     * 次のフレーム境界にまだ到達していない）。この状態のエントリは
     * 内容検証をスキップして無条件に採用する。 */
    uint8_t snapped;
    uint8_t row, col, len;
    /* 記録時点の putmes_frame_counter 値。putmes_frame_counter との符号無し
     * 差が2以上になった最初のフレーム境界でスナップショットを取る
     * （＝記録フレームの残りサイクル＋次フレーム全体が経過し、ROMの
     * _B_PUTMES ハンドラの描画完了が保証される）。符号無し差分で判定するため
     * カウンタが32bitでラップしても正しく動く。 */
    uint32_t record_frame;
    char text[PUTMES_TEXT_MAX];
    /* 描画完了後に取った、各桁のTVRAMセル生バイト。参照時に現在のTVRAM内容と
     * memcmp で突き合わせ、完全一致したセルだけキャプチャを採用する。
     * 4プレーン全てを対象にするのは、_B_PUTMES の描画がIOCS表示属性(attr)と
     * CRTC R21のSAビットのトグルの組み合わせでプレーン0のみ/1のみ/両方/
     * 両方ゼロと分岐しており（ROM $ff78ce のジャンプテーブル16エントリ）、
     * どのプレーンが変化するかを事前に決められないため。 */
    uint8_t raw[PUTMES_TEXT_MAX][PUTMES_CELL_RAW_BYTES];
} putmes_entry_t;

/* 生成済みの FPU モデルをボードとして接続する（所有権は bus へ移る）。NULL は生成失敗として
 * false を返し、無効のまま。x68k_bus_set_fpu_cir() の本体で、失敗経路のテスト用に分離。 */
bool x68k_bus_attach_fpu_cir(x68k_bus_t *bus, mc68k_fpu_t *fpu, x68k_fpu_cir_address_t address);

/* 物理ページ→配列ポインタ表（mc68k_page_map_t の ix68k 側の実体）。x68k_bus_t には埋め込まず
 * 別確保する。スナップショット復元が構造体全体を memcpy で上書きするため。
 * 作り直しは x68k_bus_page_map_refresh()（全体）と
 * x68k_bus_page_map_update_mask()（無効化ビットだけ）。
 * 作り直しの契機: ram_limit・エリアセット・拡張エリア・ウェイトポート・timing profile・
 * ユーザモード保護・ram_watch（範囲と重なるメイン RAM のページは WRITE_DIRECT を外す）・
 * スナップショット復元。 */
#define X68K_PAGE_MAP_LOCAL_SLOT_BYTES (1u << MC68K_PAGE_L1_SHIFT)
#define X68K_PAGE_MAP_DISABLE_DMAC     0x1u /* DMAC ch2 動作中は CPU アクセスごとの同期が要る */
typedef struct x68k_page_map {
    mc68k_page_map_t map;
    mc68k_page_entry_t main[MC68K_PAGE_L2_COUNT]; /* 24bit 空間（上位アドレスのミラーも共有） */
    mc68k_page_entry_t *local; /* ローカルメモリを含む 16MB スロットごとの表 */
    unsigned local_slots;
} x68k_page_map_t;

struct x68k_bus {
    /* CZ-6BP1/6BP1A の MC68881。NULL ならボード無効（XB1全域がバスエラー）。
     * fpu_cir_base は有効時の window 先頭（FPU_CIR_*_BASE）。 */
    mc68k_fpu_t *fpu_cir;
    fpu_cir_t fpu_cir_state;
    uint32_t fpu_cir_base;
    mc68k_state_t *cpu; /* x68k_bus_set_cpu() で mc68k_create() 後に配線。
                         * bus_iack() のスプリアス割り込み通知
                         * (mc68k_signal_bus_error()) に使う。 */
    uint32_t cpu_hz;    /* 実行中CPUクロック(Hz)。既定10,000,000(実機標準)。
                         * x68k_bus_set_cpu_hz()で変更する（起動オプション/メニュー由来、
                         * CPUクロック可変対応）。MFP(4MHz固定)・OPM(4MHz固定)・ADPCM DMA
                         * の実時間ペーシングはこの値とのCPUサイクル比で計算するため、
                         * mfp.c/opm.c/dmac.cがtick毎に参照する。 */
    uint8_t *ram;
    uint8_t *local_memory;
    x68k_page_map_t *pmap; /* 物理ページ表。NULL なら全アクセスがコールバック経由 */
    uint32_t local_memory_size;
    uint8_t *gvram;
    uint8_t *tvram;
    uint8_t *crtc_regs;
    crtc_t crtc;        /* CRTCラスターモデル・パレットARGB変換キャッシュ */
    uint8_t dev_rtc_eb; /* 0xEAFFEB: RTC テストバイト（書き込み保持・読み返し用） */
    hostdrv_remote_t
        hostdrv_rd;      /* HOSTDRV リモートデバイス MMIO 裏口: COMMAND/STATUS/REG_A5/REG_D0 */
    hostfile_t hostfile; /* HOSTFILE MMIO 裏口: レジスタ・転送ハンドル・状態 */
    uint8_t *sram;       /* 0xED0000–0xED3FFF: SRAM領域（実機仕様通り16KB） */
    uint8_t *sp_ram;     /* 0xEB0000–0xEBFFFF: スプライトスクロールレジスタ+BGコントロール
                          * +画面モードレジスタ+PCGエリア+BGデータエリア（64KB） */
    uint8_t *iocs_rom;
    x68k_wait_profile_t wait; /* システムポート#5 ($E8E009) 由来のRAM/ROMウェイト
                                  プロファイル。x68k_bus_apply_wait_port() が
                                  更新する。既定はポート値$00相当。 */
    x68k_timing_profile_t timing_profile;
    uint8_t poweron_vec[8];    /* 電源投入時ROMリセットベクタ(SSP/PC)。x68k_bus_load_iocs_rom が
                                   確定させ、x68k_bus_reset_vectors() が soft reset 時に $0-$7
                                   へ書き戻す（mc68k_reset() は $0/$4 を読むだけで、
                                   Human68k起動後に上書きされたゴミを読んで暴走するため） */
    uint32_t super_area_limit; /* $E86001 エリアセット。[0, super_area_limit) はユーザモードから
                                   アクセスするとバスエラー（『Inside X68000』 5-5節）。
                                   境界式: ((val & 0xFF) + 1) << 13。既定
                                  $2000（XEiJ初期値と一致） */
    uint8_t extended_area_set[5]; /* X68030 $EAFF81-$EAFF89: 256KB blocks $200000-$BFFFFF */
    int super_protect_enabled;    /* 1 でユーザ/スーパーバイザ メモリ保護を有効化（既定0=OFF）。
                                      X68K_SUPER_PROTECT 環境変数で main() が設定する。既存ソフトが
                                      ユーザモードからVRAM等に直接触れて動作している場合の回帰を
                                      避けるため既定オフ */
    uint32_t ram_watch_addr;      /* RAM書き込みウォッチ: 監視範囲の先頭アドレス */
    uint32_t ram_watch_len;       /* 監視バイト長。0で無効（既定OFF）。
                                      x68k_bus_set_ram_watch() / MCP WATCH_ON,WATCH_OFF で設定 */
    int boot_dev_watch_armed;     /* $ED0018(SRAM起動デバイス設定)読み出し検出の
                                      有効/無効。armed中のみread8()内でチェックする（通常時は
                                      分岐にすら入らずコストゼロ）。 */
    int boot_dev_read_seen;       /* armed中に$ED0018が読まれたら1が立つ。
                                      x68k_bus_take_boot_dev_read()で読んだら即座に消費される
                                      （pal_nmi_requested()と同型）。 */
    int nmi_pending; /* NMIボタン相当の要求（sticky・edge-consumed）。x68k_bus_set_nmi_pending()
                         で立てる。bus_irq_level()（CPUのpull型IRQ受付）が最優先で
                         読み、真なら消費してレベル7を返す。x68k_cycle_catchup()がpal_nmi_requested()
                         (ホストのNMIボタン)をここへ橋渡しし、mcp_cmd_interrupt()もこれ経由で
                         デバッグ用NMIを注入する（pull型ではCPUがpending_irqを見なくなるため、
                         直接mc68k_set_irq()を呼ぶ経路は使えない）。 */
    uint8_t poweroff_seq;        /* システムポート#6 ($E8E00F) 電源断シーケンスの進行
                                      状態（SYSPORT6_POWEROFF_SEQ0/1/2）。 */
    uint8_t power_off_requested; /* 電源断要求（ラッチ。取得後もクリアしない）。 */
    fdc_t fdc;
    scsi_t spc_in;                   /* 内蔵SCSI SPC(MB89352)レジスタ, $E96021-$E9603F */
    sasi_t hdc_in;                   /* 内蔵HDC(SASI互換)レジスタ, $E96001-$E96007。
                                            spc_in.unitsを共有参照、ブート時IPLROMが直接叩く経路 */
    uint8_t scsi_rom[SCSI_ROM_SIZE]; /* $FC0000-$FC1FFF: 実SCSI BIOS ROM(SCSI16IN) */
    int spc_irq_pending;    /* SPC割り込み要求pending（レベル1・ベクタ$6C固定、常時許可） */
    dmac_t dmac;            /* DMAC (HD63450) ch0/ch2/ch3の割り込み・転送状態 */
    int fdc_drive_sel;      /* 選択中のドライブ番号 (0-3) */
    int fdc_motor_on;       /* $E94007 bit7(MOTOR ON)の最後の書き込み値。1のとき
                             * fdc_drive_selのドライブのアクセスランプが赤
                             * （実ROMの説明通りbit7を単純追跡するだけでよい、
                             * モータ停止タイマー等は不要）。 */
    uint8_t drv_ctrl_latch; /* $E94005 書き込み時の前回値（イジェクトの1→0エッジ検出用） */
    uint8_t fdc_eject_masked_bits; /* ドライブ毎の物理イジェクトボタン無効化状態
                                    * (bit N=1でドライブNのボタン無効、$E94005 bit6
                                    * EJECT MASK、ドライブ選択ビット1→0エッジで確定。
                                    * 『Inside X68000』 図5「1=イジェクトボタンを
                                    * 無効化する」）。MCP eject_disk・GUIイジェクト
                                    * ボタン（x68k_bus_eject_fdc()）はこれを見て拒否
                                    * するが、IOCS _B_EJECT等のレジスタ書き込み経由
                                    * (bit5 EJECT ON/OFF)は実機同様マスクの対象外
                                    * （ソフトウェア自身の制御経路のため）。 */
    uint8_t fdc_ejected_flag
        [4]; /* ドライブ毎「前回 _B_DRVCHK($4E) mode=8 検査後にイジェクトしたか」
              * のラッチ（読み出しでクリア）。物理/MCP どちらの eject 経路でも立つ */
    /* IOI (I/O Interrupt Controller): 0xE9C001 / 0xE9C003 */
    uint8_t ioi_signal;     /* デバイス割り込み信号 (FDC=0x80, FDD=0x40) */
    uint8_t ioi_vector;     /* 割り込みベクタベース (bits 7-2, 下位2bit=0) */
    uint8_t ioi_enable;     /* 割り込み許可ビット (bit2=FDC, bit1=FDD) */
    uint8_t ioi_request;    /* 割り込み要求 pending (bit2=FDC, bit1=FDD) */
    uint8_t ioi_in_service; /* 処理中デバイス */
    mfp_t mfp;              /* MFP (MC68901): 0xE88001-0xE8802F 奇数アドレス。
                                GPIP bit7/bit4のフォールバック位相もここに含む */
    guest_clock_t guest_clock;
    rp5c15_t rtc; /* RTC (RP5C15): 0xE8A001-0xE8A01F 奇数アドレス。
                      x68k_bus_create()でのみ初期化し、CPU RESET命令では
                      触らない（実チップは電池保持でCPU RESET線と無関係） */
    /* SCC (Z8530): 0xE98001-0xE98007 奇数アドレス */
    scc_t scc;
    /* ADPCM(MSM6258) + PPI ポートC + OPM $1B */
    adpcm_t adpcm;
    /* OPM(YM2151) FM音源。$1B(CT1/CT2)は引き続きadpcmが独立追跡する（二重管理）。 */
    opm_t opm;
    /* PPIポートA/B（ジョイスティック#1/#2、$E9A001/$E9A003）。
     * アクティブロー: 未接続/未押下=1。ホスト入力未接続時は常に0xFF。 */
    uint8_t joy_port[2];
    /* ホスト→キーボード方向コマンド（USART TX）の保持状態。RESETでは消さない。 */
    kbd_state_t kbd;
    /* IOCS _B_PUTMES($2F、白帯等の座標指定文字列表示)呼び出し引数の
     * キャプチャリング。TVRAM上は反転表示でビットパターンがCGROMグリフと
     * 一致しないため（漢字も含むためピクセル解析では原理的に読めない）、
     * read_bitmap_text はキャプチャがあるセルではピクセル解析より先に
     * キャプチャ文字を採用する（iocs.c が x68k_bus_record_putmes() で
     * 書き込み、crtc.c の x68k_bus_tvram_to_text() が読み出す）。
     * ただし採用するのは、描画完了後に取ったセル内容(putmes_entry_t.raw)が
     * 現在のTVRAM内容と完全一致する間だけ。画面から消えた（スクロールで
     * 押し出された・上書きされた）キャプチャを返し続けないための賞味期限で、
     * 消去経路（IOCS/ラスタコピー/直接書き込み/DMA）を列挙せずに済む。 */
    putmes_entry_t putmes_ring[PUTMES_RING_SIZE];
    int putmes_ring_pos;
    int putmes_ring_cnt;
    /* フレーム境界ごとに x68k_bus_putmes_snapshot_tick() が加算するカウンタ。
     * putmes_entry_t.record_frame との差でスナップショット契機を判定する。 */
    uint32_t putmes_frame_counter;
    /* ANKフォント差し替え報告。x68k_bus_tvram_to_text() は本来
     * CGROM ANKパターンをROM固定オフセットで参照するが、HIOCS PLUS等の常駐
     * ドライバがIOCSベクタ$16(_FNTADR)を差し替えてANKフォントを実行時変更
     * できることが判明した。差し替え後の実アドレスは常駐ドライバ次第で
     * エミュレータ側からは分からないため、ゲスト側で_FNTADRを実際に呼んで
     * 結果をB_PRINT経由で報告してもらう方式を取る（tools-x68k/fntadr/fntadr.s）。
     * iocs.c の _B_PRINT($21) ハンドラが "FNTADR=XXXXXXXX;" マーカーを検出すると
     * x68k_bus_set_ank_font_override() 経由でここに書く。 */
    uint32_t ank_font_override_addr;
    int ank_font_override_valid;
};

/* ---- OPMレジスタ書き込みの公開ラッパー（実体は opm.c）。write8 から呼ぶ。
 *      opm_reg_write8()を呼んだ後、FMIRQ線の変化を検知しx68k_bus_mfp_opmirq()
 *      で通知する ---- */
void x68k_bus_opm_reg_write8(x68k_bus_t *b, uint32_t off, uint8_t val);

/* ---- 物理ページ表（x68k_bus.c） ---- */
/* 表の全体を状態から作り直し、無効化ビットも更新する。表の入力（ram_limit・エリアセット・
 * 拡張エリア・ウェイト・タイミング profile・CPU 接続・ローカルメモリ・ユーザモード保護・
 * SRAM の内容）が変わるたびに呼ぶ。 */
void x68k_bus_page_map_refresh(x68k_bus_t *b);
/* 無効化ビット（DMAC ch2 動作中）だけを更新する。ch2_active が変わるたびに呼ぶ。 */
void x68k_bus_page_map_update_mask(x68k_bus_t *b);
/* 表が現在の状態から作り直した結果と一致するか（試験用）。 */
bool x68k_bus_page_map_verify(x68k_bus_t *b);
