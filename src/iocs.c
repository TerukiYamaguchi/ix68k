#include "app_internal.h"
#include "mcp/debugger.h"
#include "mcp/text_event.h"
#include "platform/platform.h"
#include "bus/x68k_bus.h"
#include "mc68000/cpu.h"
#include "mc68000/cpu_types.h"
#include "mc68000/debug.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

/* --- IOCSRING: IOCSコール履歴（暴走時の事後解析用） --- */
#define IOCS_RING_SIZE 128
typedef struct {
    uint16_t fn;
    uint32_t d1, d2, a1, pc;
} iocs_log_t;
static iocs_log_t s_iocs_ring[IOCS_RING_SIZE];

/* MCP WAIT_KEYIN 用ヒューリスティック:
 * COMMAND.X のプロンプト入力待ちは B_KEYINP(fn=0)でブロックせず、
 * B_KEYSNS(0x01)/B_SFTSNS(0x02)を高頻度でノンブロッキングポーリングする
 * （1フレーム=CYCLES_PER_FRAME=166666サイクルの間に数千回呼ばれる）。
 * 1フレーム内でこれ以外のIOCS呼び出しが無く、かつポーリング回数が
 * 閾値を超えたフレームがKEYWAIT_DEBOUNCE_FRAMES連続したら「入力待ち」と
 * 判定する（デバウンスは単発の巡回チェックによる誤検出を防ぐため）。 */
#define KEYWAIT_POLL_THRESHOLD  50
#define KEYWAIT_DEBOUNCE_FRAMES 2
static int s_kw_poll_count = 0;
static int s_kw_other_count = 0;
static int s_kw_idle_streak = 0;
/* B_KEYINP入口で確認できた待機は、同フレーム中のTimer C等の無関係な
 * IOCS呼び出しで消してはならない。MCPが次の入力を予約するまで保持する。 */
static int s_kw_direct_waiting = 0;

static void iocs_key_wait_mark_if_rom_loop(x68k_bus_t *bus) {
    if (!bus || s_app.key_input_paste_active || x68k_bus_mfp_usart_rx_pending(bus) ||
        !s_app.cpu)
        return;
    uint32_t pc = mc68k_get_pc(s_app.cpu);
    /* Human68kはTRAP #15でなく、$400/$404のIOCSベクタ先を直接callすることがある。
     * B_KEYINP/B_KEYSNSのROMループ本体（入口から256バイト）にPCがあれば入力待ち。 */
    for (uint32_t vector = 0x400; vector <= 0x404; vector += 4) {
        uint32_t entry = x68k_bus_peek32(bus, vector);
        if (entry >= 0xF80000u && entry <= 0xFFFF00u && pc >= entry && pc < entry + 0x100u) {
            s_kw_direct_waiting = s_app.key_input_waiting = 1;
            return;
        }
    }
}

void iocs_key_wait_poll(x68k_bus_t *bus) {
    iocs_key_wait_mark_if_rom_loop(bus);
}

/* main.c のフレームループから1フレームに1回呼ぶ。 */
void iocs_key_wait_frame_tick(void) {
    /* frame tick単独ではバスを持たない。MCP経路はiocs_key_wait_poll()が即時観測する。 */
    if (s_kw_direct_waiting) {
        s_app.key_input_waiting = 1;
        s_kw_poll_count = 0;
        s_kw_other_count = 0;
        return;
    }
    if (s_kw_other_count == 0 && s_kw_poll_count >= KEYWAIT_POLL_THRESHOLD) {
        if (s_kw_idle_streak < KEYWAIT_DEBOUNCE_FRAMES)
            s_kw_idle_streak++;
        if (s_kw_idle_streak >= KEYWAIT_DEBOUNCE_FRAMES)
            s_app.key_input_waiting = 1;
    } else {
        s_kw_idle_streak = 0;
        s_app.key_input_waiting = 0;
    }
    s_kw_poll_count = 0;
    s_kw_other_count = 0;
}

/* MCP RESET から呼ぶ。RESET前の状態（例: 直前まで真の入力待ちだった）が残ると、
 * リセット直後の一時的な誤検出窓になり得るため明示的にクリアする。 */
void iocs_key_wait_reset(void) {
    s_kw_poll_count = 0;
    s_kw_other_count = 0;
    s_kw_idle_streak = 0;
    s_kw_direct_waiting = 0;
    s_app.key_input_waiting = 0;
}

void iocs_key_wait_input_queued(void) {
    /* PASTE開始時など、CPUがまだ次の命令を実行していない時点でも古い
     * B_KEYINP待ちを解除する。さもないと直後のKEY WAITが前回の待ちを見て
     * 即完了し、投入したコマンドの実行完了を待てない。 */
    s_kw_poll_count = 0;
    s_kw_other_count = 0;
    s_kw_idle_streak = 0;
    s_kw_direct_waiting = 0;
    s_app.key_input_waiting = 0;
}

/* IOCSRINGを整形。buf=NULL なら pal_log に出力。戻り値=buf 書き込み長 */
static int iocs_ring_format(char *buf, int bufsz) {
    int n = s_app.iocs_ring_cnt;
    int start = (s_app.iocs_ring_pos - n + IOCS_RING_SIZE) % IOCS_RING_SIZE;
    int off = 0;
    for (int i = 0; i < n; i++) {
        int idx = (start + i) % IOCS_RING_SIZE;
        iocs_log_t *e = &s_iocs_ring[idx];
        if (buf) {
            if (off < bufsz)
                off += snprintf(buf + off, (size_t)(bufsz - off),
                                "[%4d] fn=0x%02X D1=%08X D2=%08X A1=%08X PC=%06X\n", i - n + 1,
                                e->fn, e->d1, e->d2, e->a1, e->pc);
        } else {
            pal_log("[%4d] fn=0x%02X D1=%08X D2=%08X A1=%08X PC=%06X\n", i - n + 1, e->fn,
                    e->d1, e->d2, e->a1, e->pc);
        }
    }
    return off;
}

int iocs_handler(mc68k_state_t *cpu, int trap_num, void *ctx) {
    x68k_bus_t *bus = (x68k_bus_t *)ctx;

    mc68k_regs_t regs;
    mc68k_get_regs(cpu, &regs);

    if (trap_num != 15) {
        /* TRAP #15（IOCS）以外のTRAP監視。DOSコールはLine-F（$Fxxx）経由でTRAP命令を
         * 使わないため、ここで捕捉されるのは通常IOCS/DOSコール以外のTRAP。 */
        if (s_app.trace_iocs) {
            /* トレース中: 非IOCS TRAPもログ */
            pal_log("TRACE TRAP#%d D0=%08X D1=%08X A1=%08X PC=%06X\n", trap_num, regs.d[0],
                    regs.d[1], regs.a[1], mc68k_get_pc(cpu));
        }
        return 0; /* 通常の例外処理へ */
    }

    uint8_t fn = (uint8_t)(regs.d[0]);

    /* IOCSRINGに記録（暴走時の事後解析用） */
    {
        iocs_log_t *e = &s_iocs_ring[s_app.iocs_ring_pos];
        e->fn = fn;
        e->d1 = regs.d[1];
        e->d2 = regs.d[2];
        e->a1 = regs.a[1];
        e->pc = mc68k_get_pc(cpu);
        s_app.iocs_ring_pos = (s_app.iocs_ring_pos + 1) % IOCS_RING_SIZE;
        if (s_app.iocs_ring_cnt < IOCS_RING_SIZE)
            s_app.iocs_ring_cnt++;
    }
    /* _B_PUTMES（白帯等の座標指定文字列表示）を実行前にフックし、表示文字列
     * そのものを横取りして記録する。TVRAM上の反転表示はビットパターンが
     * CGROMグリフと一致せず read_bitmap_text のピクセル解析では読めない
     * （漢字も含む）ため、引数の文字列と座標を直接使う。
     * 実行自体は ROM ハンドラへ委譲する。 */
    if (fn == 0x2Fu && bus) {
        int col = (int)(regs.d[2] & 0xFFFFu);
        int row = (int)(regs.d[3] & 0xFFFFu);
        int width = (int)((regs.d[4] & 0xFFFFu) + 1u);
        if (col >= 0 && col < 128 && row >= 0 && row < 64) {
            if (width > 128 - col)
                width = 128 - col;
            char text[128];
            int i = 0;
            for (; i < width; i++) {
                uint8_t c = x68k_bus_peek8(bus, regs.a[1] + (uint32_t)i);
                if (c == 0x00u)
                    break;
                text[i] = (c == 0xFEu) ? ' ' : (char)c; /* $fe は空白表示 */
            }
            for (; i < width; i++)
                text[i] = ' '; /* 桁数未満は空白パディング（IOCS仕様） */
            x68k_bus_record_putmes(bus, row, col, width, text);
        }
    }
    /* 白帯フック: _ABORTRST ($FD) は Human68k がエラー(白帯)時に発行する。
     * 検出したらIOCSRINGをダンプし CPU を停止して状態を保全する。
     * 停止はデバッガの停止（STOP=WHITEBAND）として行うので、MCP からは
     * whiteband(action="continue") でも debug_control(action="continue") でも
     * 再開でき、DBG_STATUS も paused を返す。"RESET" でリセット。
     * ただし _ABORTRST は通常のディスクエラー処理でも発行されるため、
     * 既定では停止せず、クラッシュ調査時のみ X68K_WHITEBAND=1 /
     * MCP WHITEBAND ON で有効化する（オプトイン）。 */
    if (s_app.whiteband_halt_on && fn == 0xFDu && !dbg_is_paused()) {
        pal_log("\n=== 白帯検出 (_ABORTRST $FD) - IOCSRING (oldest->newest) ===\n");
        iocs_ring_format(NULL, 0);
        if (mc68k_get_pc_ring(cpu))
            mc68k_dump_pc_ring(cpu, 256); /* 白帯に至る実行パス（直近256命令）*/
        else
            pal_log("PCRING disabled; enable DIAG PCRING ON\n");
        pal_log("=== DEBUG HALT: CPU 停止。MCP continue で再開 / reset でリセット ===\n\n");
        dbg_halt_whiteband();
    }

    /* B:書き込みトレース中: 全IOCS呼び出し（ROM発行含む）をログ */
    if (s_app.trace_iocs) {
        pal_log("TRACE IOCS fn=0x%02X D1=%08X D2=%08X D3=%08X A1=%08X PC=%06X\n", fn, regs.d[1],
                regs.d[2], regs.d[3], regs.a[1], mc68k_get_pc(cpu));
        if (fn == 0xFDu) {
            pal_log("TRACE END (fn=0xFD _ABORTRST D1=%08X D2=%08X)\n", regs.d[1], regs.d[2]);
            s_app.trace_iocs = 0;
        }
    }

    /* ディスク系 IOCS の一部 (0x4E mode=1/8, 0x4F) は ROM ハンドラに渡すと実害が
     * あるため常にスタブで処理する。0x40/0x43/0x44/0x47/0x4A および 0x4E の
     * mode=0/2-9 はROM委譲に切り替え済み（実HDC/FDCハードウェアシーケンスを
     * ROMに踏ませる）。 */
    {
        uint8_t pda = (uint8_t)((regs.d[1] >> 8) & 0xFFu);
        int ext = (pda >= 0x80u && pda <= 0x8Fu); /* SCSI/SASI: $80-$8F */
        int drv = (int)(pda & 0x0Fu);
        switch (fn) {
        case 0x40: /* _B_SEEK: ROM委譲。実ROMハンドラ($FF8B00)はFDD側で
                    * fdc.cに実装済みのSEEK(opcode 0x0F)を、SCSI/SASI側では
                    * Seek(6)(CDB opcode 0x0B、ROMバイナリ$FF9D16実測)を発行
                    * する。SCSI/SASI側の発行先はSPC($E96020、scsi.c)では
                    * なく内蔵HDC($E96000、sasi.c)であることを実機トレース
                    * （$E96001/$E96003アクセス）で確認済み。sasi.cにSeek(6)を
                    * 追加実装済みのため両経路ともROM委譲で正しく動作する */
        case 0x43: /* _B_DSKINI: ROM委譲。ROMハンドラ($FF8CCC)が期待する
                    * $E94001/$E94003シーケンスをFDCエミュレーションが正しく
                    * 踏めるため、実ハードウェアシーケンスをROMに委譲する。 */
        case 0x44: /* _B_DRVSNS: ROM委譲。ST3のビット位置がROM側の期待する
                    * 応答形式と一致しているため、ROMハンドラ($FF8D18)へ委譲する。 */
        case 0x47: /* _B_RECALI: SCSI/FDDともROMハンドラへ委譲する。
                    * SCSI側を固定でD0=-1にしていると、ブートディスパッチャが
                    * ここで即失敗しSCSIブートに到達できない。ROM委譲することで
                    * 実HDC($E96000系)ハードウェアシーケンスを踏ませる。 */
        case 0x4A: /* _B_READID: ROM委譲。名称が似た_B_ASSIGN(fn=0x48、無関係の
                    * 論理名=物理名アサインテーブル機能)と混同しないこと。
                    * ROMハンドラ($FF9002)はfdc.cに実装済みのREAD IDコマンド
                    * (0x0A)を使う実ハードウェアシーケンスのため、委譲する。 */
        case 0x41: /* _B_VERIFY: ROM委譲。実ROMハンドラ($FF8B48)はFDD側で
                    * FDCのSCAN EQUAL(opcode 0x11、コマンド9バイト)を発行し、
                    * DMAC ch0でホストのバッファをFDCへ送って照合させる
                    * ($FF92A0がOCR=$32=DIR=0を書くのでWRITE DATAと同じ方向)。
                    * fdc.cにSCAN EQUALを実装済みのため委譲できる。
                    * SCSI/SASI側($FF94C0)はSCANを使わずREAD(6)+CPUのcmpm.b
                    * 比較で完結するため、sasi.cの既存READ実装で足りる。 */
            return 0;
        case 0x49: /* _B_WRITED */
        case 0x4C: /* _B_READDL */
            /* ROMハンドラ($FF8FD0/$FF9068)は先頭のbtst #12,d1でPDAが$9xでないと
             * $FF1E52 → $FF0626 のtrap #14（白帯）へ抜ける。SASI-HDD側は本
             * フェーズの対象外なので、非FDD PDAと未搭載ドライブはスタブで
             * -1を返してから委譲判定に入る（0x4Eの既存スタブと同じ形）。
             * fdc.cにWRITE/READ DELETED DATA(0x09/0x0C)を
             * 実装済みのため、FDD PDAはROM委譲できる。 */
            if (ext || (pda & 0xF0u) != 0x90u || !bus || drv >= x68k_bus_fdc_num_drives(bus)) {
                mc68k_set_dreg(cpu, 0, (uint32_t)-1);
                return 1;
            }
            return 0;
        case 0x4F: /* _B_EJECT: ROM委譲（FDD側のみ）。
                    * ROMハンドラ($FF9280)は自前実装を持たず、_B_DRVCHKの
                    * ディスパッチャ($FF9148)をmode=7→3→1の順で呼ぶだけの
                    * 薄いラッパーであることを実測で確認。実際に$E94005へ
                    * 書き込むmode=1本体($FF9256)は`btst.l #$06,(a1)`で
                    * ROM自身のRAMシャドウ（ワークエリア$09E2+drv、mode=2/6
                    * で立つ"ロック"ビットの合成値。$E94005ハードウェアの
                    * EJECT MASK(bit6、物理イジェクトボタン無効化)とは別概念、
                    * 混同しないこと）を見てブロックする。ROM委譲後もmode=2/6
                    * でロックしたドライブはmode=1/_B_EJECTから正しく保護
                    * される（実機検証済み）ため、_B_EJECTは「排出禁止時にも
                    * 排出できる」わけではない（過去の想定は誤りだった）。
                    * ブートローダのnot-ready/署名不一致プローブでの実排出
                    * （IPLROMXV FF039E→FF042C）は、ロックされていないドライブ
                    * に限り実機どおり物理排出が起きる想定内の挙動になる。
                    * SASI/SCSI側($FF98B6)は本フェーズの対象外（HDD側の
                    * イジェクトコマンド実装が前提）
                    * のため、非FDD PDAと未搭載ドライブは0x4E/0x49/0x4Cと
                    * 同じ形でスタブのままD0=-1を返す。 */
            if (ext || drv < 0 || (pda & 0xF0u) != 0x90u || !bus ||
                drv >= x68k_bus_fdc_num_drives(bus)) {
                mc68k_set_dreg(cpu, 0, (uint32_t)-1);
                return 1;
            }
            return 0;
        case 0x4E: /* _B_DRVCHK / _DRVCTRL: 2HD ドライブの状態設定・取得 */
        {
            /* ROM ハンドラ($FF9140)は先頭の btst #12,d1 で PDA が $9x でないと
             * $FF1E52 → $FF0626 の trap #14（白帯）へ抜けるため、非FDD PDA と
             * 未搭載ドライブは従来どおりスタブで -1 を返してから委譲判定に入る
             * （num_drives未満かで判定しないと、存在しないドライブに「メディアなし」
             * を返してしまいDRIVEコマンドの表示台数がずれる）。 */
            if (ext || drv < 0 || (pda & 0xF0u) != 0x90u || !bus ||
                drv >= x68k_bus_fdc_num_drives(bus)) {
                mc68k_set_dreg(cpu, 0, (uint32_t)-1);
                return 1;
            }
            uint16_t mode = (uint16_t)(regs.d[2] & 0xFFFFu);
            switch (mode) {
            /* mode=1(イジェクト)はROM委譲。ROMハンドラ
             * ($FF9256、_B_EJECTと共有)はROM自身のロックシャドウを尊重する
             * ため、下のdefault:へ委譲して差し支えない（詳細は0x4Fの
             * コメント参照）。 */
            case 8: { /* 前回検査後のイジェクト有無。ROM は $09E7+drv*2 を返すが、
                       * これは $E94005 の物理 DISK IN 変化を割り込み($FF1468)で
                       * 拾う仕組みで、MCP eject_disk 等のホスト側イジェクトを
                       * 取りこぼす。実機検証済みのホスト側ラッチを使い続ける。 */
                int ejected = x68k_bus_fdc_take_ejected_flag(bus, drv);
                mc68k_set_dreg(cpu, 0, ejected ? (uint32_t)-1 : 1u);
                return 1;
            }
            default:
                /* mode=0/2..7/9 は ROM ハンドラ($FF9140)へ委譲する。IOCS
                 * ディスパッチャ($FF1E10)は $0400+fn*4 の RAM ベクタを jsr
                 * するため、2HDSIM 等が $4E に仕込んだ TSR フックへ到達できる。
                 *ROM の状態取得($FF91C4)は $E94005 の
                 * DISK IN(bit7) を lsr #6 で bit1 に落とす実装で、
                 * bus_mmio_fdc_read8() の $E94005 実装と一致する。lock/LED
                 * ビットは ROM ワークエリア $09E2+drv が保持し、mode=2..7 は
                 * それを $E94005 bit7/bit6 へ反映する（bit5 は必ず 0 なので
                 * 誤排出しない）。これによりエミュレータ独自の lockbits[] は
                 * 不要になり、排出禁止が x68k_bus.c の fdc_eject_masked_bits に
                 * 正しく繋がる。 */
                return 0;
            }
        }
        default:
            break;
        }
    }

    /* B_WRITE (fn=0x45) は ROM/RAM 問わず全呼び出しをログ */
    if (fn == 0x45u) {
        pal_log("B_WRITE_CALL fn=0x45 D1=%08X D2=%08X D3=%08X A1=%08X PC=%06X\n", regs.d[1],
                regs.d[2], regs.d[3], regs.a[1], mc68k_get_pc(cpu));
    }

    /* B_KEYINPはROM内のキーボード処理から再発行されることがある。ROMを委譲する
     * 下の早期returnより先に観測だけ行い、実際のIOCS処理は従来どおりROMへ渡す。
     * PASTE中は各文字の間に空FIFOとなるため、完了待ちの誤検出を避けて抑制する。 */
    if (fn == 0x00u && !s_app.key_input_paste_active && !x68k_bus_mfp_usart_rx_pending(bus))
        s_kw_direct_waiting = s_app.key_input_waiting = 1;

    /* ROM 内コードからの TRAP #15 は ROM 自身の IOCS ハンドラに処理させる。
     * mc68k_get_pc() は TRAP の次のアドレス（スタックに積まれた復帰アドレス）を返す。 */
    if (mc68k_get_pc(cpu) >= 0xF80000u) {
        return 0; /* ベクタテーブル経由で ROM IOCS ハンドラへ */
    }

    /* WAIT_KEYIN ヒューリスティック用カウント（fn=0 は下のcaseで直接管理するため除外） */
    if (fn == 0x01u || fn == 0x02u)
        s_kw_poll_count++;
    else if (fn != 0x00u)
        s_kw_other_count++;

    switch (fn) {
    /* ---- キーボード系 IOCS (#0x00-#0x09): ROM純正ドライバへ委譲。
     * MFP USART RX FIFO 経由の実プロトコルバイトを ROM が解釈する。
     * _B_KEYINPはROM内でビジーウェイトするため、入口で受信FIFO/UDRが空なら
     * その時点で入力待ちを通知できる。これはフレーム単位のB_KEYSNS/B_SFTSNS
     * ヒューリスティックより早く、直接B_KEYINPを呼ぶプログラムにも効く。
     * ROM内のソフトウェアキーバッファに既に文字があれば短い偽陽性になり得るが、
     * 次の入力投入で解除される。 */
    case 0x00: /* B_KEYINP */
        return 0;
    case 0x01: /* B_KEYSNS */
    case 0x02: /* B_SFTSNS */
    case 0x03: /* _KEY_INIT */
    case 0x04: /* _BITSNS */
    case 0x05: /* _SKEYSET */
    case 0x06: /* _LEDCTRL */
    case 0x07: /* _LEDSET */
    case 0x08: /* _KEYDLY */
    case 0x09: /* _KEYREP */
        return 0;
    /* ---- 未接続デバイス: 正しい戻り値でダミー応答 ---- */
    case 0x0C: /* _TVCTRL: Timer C 再起動（BIOS がタイマを再スタートさせる） */
        x68k_bus_mfp_set_tcdcr(bus, 0x70); /* TC mode=7 (div/200), TD mode=0 */
        mc68k_set_dreg(cpu, 0, 0);
        break;
    case 0x30: /* _SET232C: SCC ch A の通信条件設定を ROM へ委譲する。 */
        return 0;
    case 0x3A: /* (未公開) プリンタパラメータ設定。D0=0x7F を返す。 */
        mc68k_set_dreg(cpu, 0, 0x7F);
        break;
    /* _JOYGET は PPI ポート A/B を読む ROM ハンドラへ委譲する。 */
    case 0x3B: /* _JOYGET */
        return 0;
    case 0x3C: /* _INIT_PRN: プリンタ初期化（未接続） */
        mc68k_set_dreg(cpu, 0, 0);
        break;
    /* ---- ディスク系 IOCS (#0x40-#0x4F) ---- */
    /* 0x40, 0x41, 0x43, 0x44, 0x47, 0x49, 0x4A, 0x4C, 0x4E, 0x4F は
     * バイパス前の pre-handler で処理済み */
    case 0x42:
    case 0x48:
    case 0x4B:
        mc68k_set_dreg(cpu, 0, 0); /* 成功(0) — bit30=0 でエラーフラグなし */
        break;
    case 0x4D: /* _B_FORMAT: ディスクの物理フォーマット */
    {
        /* D1.hb=PDA（$80-$8F=SASI-HD、それ以外の下位4bit=FDDドライブ番号）。 */
        uint8_t pda = (uint8_t)((regs.d[1] >> 8) & 0xFFu);
        int ext = (pda >= 0x80u && pda <= 0x8Fu);
        if (ext) {
            mc68k_set_dreg(cpu, 0, (uint32_t)-1); /* SASI-HD 未実装 */
            break;
        }
        /* FDDは媒体を問わず実機ROMの_B_FORMAT($FF90AA)へ委譲する。ROMはN(D2上位)と
         * MFMビットでテーブル($FF9488)を引いてSC/GPLを決め、A1のID配列をDMAで送って
         * FDCのFORMAT TRACKを発行する。FDC側が媒体プロファイルのIDと照合する
         * （標準2HDの予備シリンダcyl=77の黙殺成功もFDC側で処理する）。 */
        return 0;
    }
    case 0x45: /* _B_WRITE: ディスクへの書き込み */
    case 0x46: /* _B_READ: ディスクからの読み込み */
    {
        /* PDA $80-$8F = SCSI/SASI、$90-$93/$00 = FDD。
         * SCSI側もROMハンドラへ委譲する。実SCSI16IN ROMは
         * SPCハードウェアレジスタを直接操作してブートセクタを読み込む設計で、
         * scsi.c/x68k_bus.cで実レジスタ挙動を実装済みのため、FDD同様に
         * ROM委譲で正しく動作する。 */
        /* FDD: ROM の _B_READ($FF8E20)/_B_WRITE($FF8D58) ハンドラへ委譲する。
         * 両ハンドラはセクタごとに READ/WRITE DATA (opcode 0x06/0x05) を
         * 発行して実FDC ($E94001/$E94003/$E94005) を直接叩く設計で、SASI
         * は経由しない（IPLROMXV_addr.S で確認済み）。ROM は全ハンドラで
         * 妥当なオペコード・バイト数しか送らないため、FDCがRESULTフェーズへ
         * 誤遷移するような不正シーケンスは発生しない。 */
        return 0;
    } break;
    /* ---- 画面制御・コールバック設定: ROM ハンドラに任せる ---- */
    case 0x0D: /* _LEDMOD: LED モード設定 */
    case 0x0E: /* _TGUSEMD: 画面の使用状態の設定/取得 (旧状態を D0 で返す) */
    case 0x0F: /* _DEFCHR: 外字定義 */
    case 0x10: /* _CRTMOD: 画面モード設定 (TVRAM クリア・パレット設定は ROM が行う) */
    case 0x16: /* _FNTADR: フォントアドレス取得 (A1/D0 を返す) */
    case 0x1E: /* _B_CURON: カーソル表示再開 */
    case 0x1F: /* _B_CUROFF: カーソル表示停止 */
    case 0x2E: /* _B_CONSOL: テキスト画面表示範囲設定 */
    case 0x36: /* (未公開) マウスデータ受信時の処理アドレス設定。ここで横取りして
                * D0=0 を返すとROM側マウスドライバのコールバックアドレスが
                * 未初期化のまま残り、後続のSCC ch B実マウス割り込み(mouse_move
                * MCPツール等)がそのアドレスへジャンプして暴走する(BADADDR)。
                * 0x70-0x7C の他マウスIOCSと同じ理由でROMに委譲する。 */
    case 0x37: /* B_WRTMOD: テキスト出力コールバック設定 (RAM[0x97E]) */
    case 0x39: /* (未公開) BEEP 音用 PCM データ設定: 旧設定アドレスを返す */
    case 0x72: /* _MS_CUROF: マウスカーソル消去 */
    case 0x7D: /* _SKEY_MOD: ソフトキーボード制御 */
        /* これらを D0=0 で握りつぶすと SWITCH.X が旧状態/ベクタ/フォントアドレスを
         * 使って暴走する (PC=$0A 不正命令例外 / PC=0 アドレスエラー)。
         * ROM ディスパッチャ経由で本来のハンドラ (_TGUSEMD=$FF6668,
         * _FNTADR=$FF6D76, _DEFCHR=$FF669A 等) に渡す。
         * $7D を委譲すると _SKEY_MOD ROM ハンドラが
         *   スタックフレームを破壊し、呼び出し元ラッパー (PC=$077E82) の RTS が
         *   不正戻りアドレス $44BA0005 を拾って白帯になる (BADJUMP/BADMOVEA で特定)。
         *   スタブ化すると今度は割り込みハンドラ ($E372) の RTE が暴走する。
         *   この制約があるため、ソフトキーボードのワークエリア初期化が必要。 */
        return 0; /* ベクタテーブル経由で ROM IOCS ハンドラへ */
    /* ---- テキスト出力: ROM/IOCS.X に委ねる（MCP 用にキャプチャのみ行う） ---- */
    case 0x20: /* _B_PUTC: D1.W = Shift-JIS 文字コード */
    {
        uint16_t ch = (uint16_t)(regs.d[1] & 0xFFFFu);
        uint8_t bytes[2];
        size_t len;
        if (ch > 0xFFu) {
            bytes[0] = (uint8_t)(ch >> 8);
            bytes[1] = (uint8_t)ch;
            len = 2;
        } else {
            bytes[0] = (uint8_t)ch;
            len = 1;
        }
        for (size_t i = 0; i < len; ++i)
            mcp_output_append(bytes[i]);
        (void)mcp_text_event_emit(regs.pc, "iocs", "B_PUTC", "shift-jis",
                                  MCP_TEXT_EVENT_UNSPECIFIED, MCP_TEXT_EVENT_UNSPECIFIED,
                                  MCP_TEXT_EVENT_UNSPECIFIED, 0, bytes, len);
    }
        return 0;
    case 0x21: /* _B_PRINT: A1 = Shift-JIS 文字列アドレス */
    {
        uint32_t addr = regs.a[1];
        uint8_t line[1024];
        int line_len = 0;
        for (int i = 0; i < 1024; i++) {
            uint8_t c = x68k_bus_read_ram_byte(bus, addr + (uint32_t)i);
            if (c == 0)
                break;
            mcp_output_append(c);
            line[line_len++] = c;
        }
        (void)mcp_text_event_emit(regs.pc, "iocs", "B_PRINT", "shift-jis",
                                  MCP_TEXT_EVENT_UNSPECIFIED, MCP_TEXT_EVENT_UNSPECIFIED,
                                  MCP_TEXT_EVENT_UNSPECIFIED, 0, line, (size_t)line_len);
        x68k_bus_scan_fntadr_report(bus, line, line_len);
    }
        return 0;
    /* ---- テキスト画面制御: ROM ハンドラに委ねる ---- */
    case 0x22:    /* _B_COLOR  */
    case 0x23:    /* _B_LOCATE */
    case 0x24:    /* _B_DOWN_S */
    case 0x25:    /* _B_UP_S   */
    case 0x26:    /* _B_UP     */
    case 0x27:    /* _B_DOWN   */
    case 0x28:    /* _B_RIGHT  */
    case 0x29:    /* _B_LEFT   */
    case 0x2A:    /* _B_CLR_ST */
    case 0x2B:    /* _B_ERA_ST */
    case 0x2C:    /* _B_INS    */
    case 0x2D:    /* _B_DEL    */
    case 0x2F:    /* _B_PUTMES */
        return 0; /* ROM ハンドラへ */

    /* ---- RS-232C: SCC ch A を操作する ROM ハンドラへ委譲する ---- */
    case 0x31: /* _LOF232C: 受信バッファ残数取得/クリア */
    case 0x32: /* _INP232C: 1バイト受信（ブロッキング） */
    case 0x33: /* _ISNS232C: 受信ステータス（受信データ有無） */
    case 0x34: /* _OSNS232C: 送信ステータス（送信可否） */
    case 0x35: /* _OUT232C: 1バイト送信 */
        return 0;
    /* ---- 日付・時刻 ---- */
    case 0x51: /* _DATESET: 日付設定 → D1 = BCD日付($00_yy_mm_dd、DATEBINと同形式)。
                * 実RTCはSET後も自走し続けるため、「現在時刻の年月日だけを
                * D1の値に差し替えた時刻」を x68k_bus_rtc_set() でオフセットへ
                * 記録する方式で模倣する（時刻(時分秒)側は現在値を維持する）。
                * MMIO側($E8A000)と同じ時計を共有する。
                * 返値は仕様上常に0。 */
    {
        uint32_t d1 = regs.d[1];
        uint8_t yb = (uint8_t)(d1 >> 16);
        uint8_t mb = (uint8_t)(d1 >> 8);
        uint8_t db = (uint8_t)d1;
        int yr2 = ((yb >> 4) & 0xF) * 10 + (yb & 0xF); /* 1980起点 */
        int mon = ((mb >> 4) & 0xF) * 10 + (mb & 0xF);
        int day = ((db >> 4) & 0xF) * 10 + (db & 0xF);
        time_t cur = (time_t)x68k_bus_rtc_now(bus);
        struct tm t = *localtime(&cur);
        t.tm_year = (yr2 + 1980) - 1900;
        t.tm_mon = mon - 1;
        t.tm_mday = day;
        time_t target = mktime(&t);
        x68k_bus_rtc_set(bus, (int64_t)target);
        mc68k_set_dreg(cpu, 0, 0);
        break;
    }
    case 0x53: /* _TIMESET: 時刻設定 → D1 = BCD時刻(TIMEGETと同形式)。
                * DATESETと同様、日付側は現在値を維持したまま設定する。
                * 返値は仕様上常に0。 */
    {
        uint32_t d1 = regs.d[1];
        uint8_t hb = (uint8_t)(d1 >> 16);
        uint8_t mb = (uint8_t)(d1 >> 8);
        uint8_t sb = (uint8_t)d1;
        int hour = ((hb >> 4) & 0xF) * 10 + (hb & 0xF);
        int min = ((mb >> 4) & 0xF) * 10 + (mb & 0xF);
        int sec = ((sb >> 4) & 0xF) * 10 + (sb & 0xF);
        time_t cur = (time_t)x68k_bus_rtc_now(bus);
        struct tm t = *localtime(&cur);
        t.tm_hour = hour;
        t.tm_min = min;
        t.tm_sec = sec;
        time_t target = mktime(&t);
        x68k_bus_rtc_set(bus, (int64_t)target);
        mc68k_set_dreg(cpu, 0, 0);
        break;
    }
    case 0x54: /* _DATEGET: 日付取得。D0=$0w_yy_mm_dd、年月日は BCD。 */
    {
        time_t now = (time_t)x68k_bus_rtc_now(bus);
        struct tm *t = localtime(&now);
        int yr2 = (t->tm_year + 1900) - 1980; /* 1980 起点 2桁値 */
        uint8_t yr_bcd = (uint8_t)(((yr2 / 10) << 4) | (yr2 % 10));
        uint8_t mo_bcd = (uint8_t)((((t->tm_mon + 1) / 10) << 4) | ((t->tm_mon + 1) % 10));
        uint8_t day_bcd = (uint8_t)(((t->tm_mday / 10) << 4) | (t->tm_mday % 10));
        uint32_t wday = (uint32_t)(t->tm_wday & 0x7);
        mc68k_set_dreg(cpu, 0,
                       (wday << 24) | ((uint32_t)yr_bcd << 16) | ((uint32_t)mo_bcd << 8) |
                           day_bcd);
        break;
    }
    case 0x56: /* _TIMEGET: 時刻取得 → D0 = (hour_BCD<<16)|(min_BCD<<8)|sec_BCD */
    {
        time_t now = (time_t)x68k_bus_rtc_now(bus);
        struct tm *t = localtime(&now);
        uint8_t h_bcd = (uint8_t)(((t->tm_hour / 10) << 4) | (t->tm_hour % 10));
        uint8_t m_bcd = (uint8_t)(((t->tm_min / 10) << 4) | (t->tm_min % 10));
        uint8_t s_bcd = (uint8_t)(((t->tm_sec / 10) << 4) | (t->tm_sec % 10));
        mc68k_set_dreg(cpu, 0, ((uint32_t)h_bcd << 16) | ((uint32_t)m_bcd << 8) | s_bcd);
        break;
    }
    case 0x55: /* _DATEBIN: BCD 日付 $0w_yy_mm_dd を binary $wy_yy_mm_dd へ変換する。 */
    {
        uint32_t d1 = regs.d[1];
        uint32_t wday = (d1 >> 24) & 0x7u;
        uint8_t yb = (uint8_t)(d1 >> 16);
        uint8_t mb = (uint8_t)(d1 >> 8);
        uint8_t db = (uint8_t)d1;
        int yr2 = ((yb >> 4) & 0xF) * 10 + (yb & 0xF); /* 1980起点 */
        int mon = ((mb >> 4) & 0xF) * 10 + (mb & 0xF);
        int day = ((db >> 4) & 0xF) * 10 + (db & 0xF);
        int year = yr2 + 1980;
        mc68k_set_dreg(cpu, 0,
                       (wday << 28) | ((uint32_t)year << 16) | ((uint32_t)mon << 8) |
                           (uint32_t)day);
        break;
    }
    case 0x57: /* _TIMEBIN: BCD 時刻 $00_hh_mm_ss の各フィールドを binary へ変換する。 */
    {
        uint32_t d1 = regs.d[1];
        uint8_t hb = (uint8_t)(d1 >> 16);
        uint8_t mb = (uint8_t)(d1 >> 8);
        uint8_t sb = (uint8_t)d1;
        uint32_t hour = ((hb >> 4) & 0xF) * 10u + (hb & 0xF);
        uint32_t min = ((mb >> 4) & 0xF) * 10u + (mb & 0xF);
        uint32_t sec = ((sb >> 4) & 0xF) * 10u + (sb & 0xF);
        mc68k_set_dreg(cpu, 0, (hour << 16) | (min << 8) | sec);
        break;
    }
    /* ---- テキスト VRAM 矩形ブロック転送（ROM 委譲）----
     * _VRAMGET($17)/_VRAMPUT($18) は「バイト単位」の名前に反して、実機 ROM
     * (ff7074/ff7084) は矩形ブロックコピーである:
     *   a2=TVRAM, a1=RAMバッファ, d1=バイト/行-1, d2=行数-1, d3=行間ストライド。
     * Human68k の白帯（ディスクエラー等）ルーチンは帯下の TVRAM をこの $17 で
     * 退避し、$18 で書き戻して帯を消去する。「A1 の 1 バイトだけ read/write」
     * という誤った規約で実装すると a2(TVRAM) に一切触れず、restore が no-op に
     * なり帯が永久に消えない。ROM の正しい実装へ委譲する。 */
    case 0x17:    /* _VRAMGET */
    case 0x18:    /* _VRAMPUT */
        return 0; /* ROM IOCS ハンドラへ委譲 */

    /* ---- システム情報 ---- */
    case 0x7F: /* _ONTIME: D0=当日分の経過 1/100 秒、D1=経過日数。 */
    {
        /* time(NULL)は秒単位の解像度しかなく、「値が変わるまで待つ」形の
         * ウェイトループ（1/100秒刻みの遅延プリミティブ）が本来10msで
         * 抜けるべきところ最大1秒spinしてしまう（GSHELL.Xの帰還アニメーション
         * で実測20倍以上の遅延を確認）。main.cが$9CAシステムワーク更新に
         * 使うゲスト単調時計と同じ時刻源に揃え、1/100秒粒度を回復する。
         * デバッガ停止中はこの時計も凍結する。 */
        uint64_t cs =
            (x68k_bus_guest_mono_us(bus) - s_app.boot_time_us) / 10000u; /* 起動後経過1/100秒 */
        uint32_t day = (uint32_t)(cs / 8640000u);
        uint32_t tod = (uint32_t)(cs % 8640000u);
        mc68k_set_dreg(cpu, 0, tod);
        mc68k_set_dreg(cpu, 1, day & 0xFFFFu);
        break;
    }
    case 0x8F: /* _ROMVER: ROM バージョン取得 → 実ROMハンドラへ委譲。
                * ROM 自身の版数（IPLROM 1.6 は XEiJ のリリースごとに異なり、例: 2024-08-25 版
                * $16240825 / 2025-08-03 版 $16250803）を返さないと、版数を検査する 060turbo.sys
                * 等が ROM を識別できない。 */
        return 0;

    /* ---- ベクタ設定 ---- */
    case 0x80: /* _B_INTVCS: 割り込みベクタ設定  D1=ベクタ番号, A1=ハンドラアドレス */
    {
        /* D1 = ベクタ番号（下位16bit）。$100 未満は CPU 例外ベクタ、$100 以上は
         * IOCS 呼び出しベクタ表（$0400 + fn*4）を指す。実ROMの _B_INTVCS
         * (ffac06) は move.w d1,d0; asl.l #2,d0 で D1 全16bitを使うため、
         * 下位バイトのみでマスクすると $100 以上のベクタ番号が壊れる
         * （例: SCSIINROM が _B_INTVCS d1=$1F5 で IOCS $F5 用ハンドラを
         * 登録しようとしても $3D4 に誤格納され、本来の $7D4 に届かない）。
         * 設定前のベクタを D0 に返すのが本来の仕様。これを返さないと、
         * ドライバの「旧ベクタ保存→復元」で 0 が書き戻され割り込み時に
         * 番地 0 付近へ暴走する。*/
        uint32_t va = (uint32_t)(regs.d[1] & 0xFFFFu) * 4u;
        uint32_t old = bus ? x68k_bus_read32(bus, va) : 0u;
        if (bus)
            x68k_bus_write32(bus, va, regs.a[1]);
        mc68k_set_dreg(cpu, 0, old);
        break;
    }

    /* ---- ROM 委譲: RAM コードから呼ばれても ROM ハンドラに任せる ---- */
    /* 0x82-0x89 のメモリ操作は、A1 の自動更新や転送方向を含む IOCS 規約を
     * ROM ハンドラに任せる。 */
    case 0x82: /* _B_BPEEK */
    case 0x83: /* _B_WPEEK */
    case 0x84: /* _B_LPEEK */
    case 0x85: /* _B_MEMSTR */
    case 0x86: /* _B_BPOKE */
    case 0x87: /* _B_WPOKE */
    case 0x88: /* _B_LPOKE */
    case 0x89: /* _B_MEMSET */
        return 0;

    case 0x11: /* _CONTRAST */
    case 0x12: /* _HSVTORGB */
    case 0x13: /* _TPALET */
    case 0x14: /* _TPALET2 */
    case 0x15: /* _TCOLOR */
    case 0x19: /* _FNTGET: CGROMからフォントパターンを読むだけで、DMAC等の追加
                * ハードウェア依存が無いため、ROMの実ハンドラへ委譲すれば
                * 正しく動作する（CGROMは既にロード済み）。 */
    case 0x1A: /* _TEXTGET */
    case 0x1B: /* _TEXTPUT */
    case 0x1C: /* _CLIPPUT */
    case 0x1D: /* _SCROLL */
    /* 0x50/0x52: バイナリ→BCD変換。RTC等のハードウェアには触れずレジスタ渡しの
     * 値のみで完結する。特に_DATEBCDは単純なBCD変換に加え閏年カウンタ・曜日
     * カウンタも計算する仕様のため、自前実装より正確なROMの実ハンドラへ委譲
     * する方が安全。 */
    case 0x50: /* _DATEBCD */
    case 0x52: /* _TIMEBCD */
    /* 0x58-0x5C: 日付/時刻/曜日の文字列⇔バイナリ変換。RTC等のハードウェアには
     * 一切触れず、レジスタ渡しの値/文字列バッファのみで完結する純粋な変換
     * ロジックなので委譲して問題ない。 */
    case 0x58: /* _DATECNV */
    case 0x59: /* _TIMECNV */
    case 0x5A: /* _DATEASC */
    case 0x5B: /* _TIMEASC */
    case 0x5C: /* _DAYASC */
    /* 0x60-0x67 _ADPCMOUT/INP/AOT/AIN/LOT/LIN/SNS/MOD: MSM6258 ADPCM
     * 音源制御。DMAC ch3汎用エンジン(dmac.c)・MSM6258レジスタ($E92001/03、
     * adpcm.c)経由でROM実ハンドラが完結する。chain(_ADPCMAOT/AIN/LOT/LIN)の
     * フレームまたぎ走査、_ADPCMMODのCCR $10/$20/$08(停止/一時停止/再開)との
     * 連動もdmac.cのadpcm_dma_tステートマシンで対応済み。 */
    case 0x60: /* _ADPCMOUT */
    case 0x61: /* _ADPCMINP */
    case 0x62: /* _ADPCMAOT */
    case 0x63: /* _ADPCMAIN */
    case 0x64: /* _ADPCMLOT */
    case 0x65: /* _ADPCMLIN */
    case 0x66: /* _ADPCMSNS */
    case 0x67: /* _ADPCMMOD */
    case 0x38: /* _SETFNTADR */
    /* OPM音色設定とステータス取得はROMからOPMレジスタ経路へ委譲する。 */
    case 0x68: /* _OPMSET */
    case 0x69: /* _OPMSNS */
    case 0x6A: /* _OPMINTST */
    case 0x6B: /* _TIMERDST */
    case 0x6C: /* _VDISPST */
    case 0x6D: /* _CRTCRAS */
    case 0x6E: /* _HSYNCST */
    case 0x6F: /* _PRNINTST */
    /* マウス IOCS。ここで横取りして D0=0 を返すと ROM 側マウスドライバの
     * ワークエリアが未初期化のまま呼び出し元（X-BASIC 起動シーケンス等）が
     * 参照し、不正ポインタで暴走する。ROM に委譲すれば初期化が正常に完了し、
     * SCC ch B 経由の実マウス入力（mouse_move/mouse_click MCPツール）も
     * 正しく機能する。 */
    case 0x70: /* _MS_INIT */
    case 0x71: /* _MS_CURON */
    /* 0x72 _MS_CUROF は前方の画面制御・コールバック設定ブロックで委譲済み */
    case 0x73:    /* _MS_STAT */
    case 0x74:    /* _MS_GETDT */
    case 0x75:    /* _MS_CURGT */
    case 0x76:    /* _MS_CURST */
    case 0x77:    /* _MS_LIMIT */
    case 0x78:    /* _MS_OFFTM */
    case 0x79:    /* _MS_ONTM */
    case 0x7A:    /* _MS_PATST */
    case 0x7B:    /* _MS_SEL */
    case 0x7C:    /* _MS_SEL2 */
    case 0x7E:    /* _DENSNS: 電卓制御。ROM実ハンドラ($FF3662)へ委譲する。 */
        return 0; /* ROM IOCS ハンドラへ委譲 */
    case 0xA0:    /* _SFTJIS */
    case 0xA1:    /* _JISSFT */
    case 0xA2:    /* _AKCONV */
    case 0xA3:    /* _RMACNV */
    case 0xA4:    /* _DAKJOB */
    case 0xA5:    /* _HANJOB */
    case 0xAC:    /* _SYS_STAT */
    case 0xAD:    /* _B_CONMOD */
    case 0xAE:    /* _OS_CURON */
    case 0xAF:    /* _OS_CUROF */
    /* グラフィック文字描画 IOCS（Console/Graphic IOCS ドライバが処理）。
     * ここで横取りして D0=0 を返すと描画が行われず、SWITCH.X 等が
     * 未初期化バッファを使って暴走する。ROM ディスパッチャ経由で
     * ドライバの登録ハンドラに渡す。*/
    case 0xD3: /* _TXXLINE */
    case 0xD4: /* _TXYLINE */
    case 0xD5: /* _TXLINE  */
    case 0xD6: /* _TXBOX   */
    case 0xD7: /* _TXFILL  */
    case 0xD8: /* _TXREV   */
    case 0xDF: /* _TXRASCPY */
    /* グラフィック画面 IOCS。GVRAM 本体・CRTC 色数モード・
     * グラフィックパレットは既にバス層で実装済みのため、line/circle/paint
     * 等の描画アルゴリズムは再実装せず ROM の Graphic IOCS ドライバに委譲する。 */
    case 0x90: /* _G_CLR_ON: グラフィック画面初期化。ROM 内部の「使用可否」フラグ
                * ($964 相当)をここで初期化するため、委譲しないと以降の
                * _WIPE/_FILL 等が全てグラフィック使用不可エラーになる。 */
    case 0x94: /* _GPALET */
    case 0x95: /* (未公開) カラーコード設定。ROM実ハンドラ($FFB206)へ委譲する。 */
    case 0x96: /* (未公開) アクセスページ設定。ROM実ハンドラ($FFB214)へ委譲する。 */
    case 0x97: /* (未公開) GVRAMドット読み込み。ROM実ハンドラ($FFB22C)へ委譲する。 */
    case 0x98: /* (未公開) 色マスク付きGVRAMドット書き込み。ROM実ハンドラ($FFB2CA)へ委譲する。
                */
    case 0x99: /* (未公開) GVRAMドット書き込み。ROM実ハンドラ($FFB39A)へ委譲する。 */
    case 0x9A: /* (未公開) GVRAMビットパターン書き込み。ROM実ハンドラ($FFB440)へ委譲する。 */
    case 0x9B: /* (未公開)
                  バックカラー付きビットパターン書き込み。ROM実ハンドラ($FFB476)へ委譲する。 */
    case 0x9C: /* (未公開) GVRAMビットパターン拡大書き込み。ROM実ハンドラ($FFB4DC)へ委譲する。
                */
    /* _DRAWMODE/_GETGRM/_PUTGRM も同じ Graphic IOCS ドライバへ委譲する。 */
    case 0xB0:    /* _DRAWMODE */
    case 0xB1:    /* _APAGE */
    case 0xB2:    /* _VPAGE */
    case 0xB3:    /* _HOME */
    case 0xB4:    /* _WINDOW */
    case 0xB5:    /* _WIPE */
    case 0xB6:    /* _PSET */
    case 0xB7:    /* _POINT */
    case 0xB8:    /* _LINE */
    case 0xB9:    /* _BOX */
    case 0xBA:    /* _FILL */
    case 0xBB:    /* _CIRCLE */
    case 0xBC:    /* _PAINT */
    case 0xBD:    /* _SYMBOL */
    case 0xBE:    /* _GETGRM */
    case 0xBF:    /* _PUTGRM */
        return 0; /* ROM IOCS ハンドラへ委譲 */

    /* スプライト/BG IOCS。$EB0000-$EBFFFF のレジスタとメモリはバス層が扱うため、
     * 上記グラフィックIOCS群と同じ理由でROMのSprite/BG IOCSドライバに委譲する。 */
    case 0xC0:    /* _SP_INIT   スプライト初期化 */
    case 0xC1:    /* _SP_ON     スプライト表示 */
    case 0xC2:    /* _SP_OFF    スプライト消去 */
    case 0xC3:    /* _SP_CGCLR  スプライトパターンのクリア */
    case 0xC4:    /* _SP_DEFCG  スプライトパターンの定義 */
    case 0xC5:    /* _SP_GTPCG  スプライトパターンの読み込み */
    case 0xC6:    /* _SP_REGST  スプライトレジスタの設定 */
    case 0xC7:    /* _SP_REGGT  スプライトレジスタの読み出し */
    case 0xC8:    /* _BGSCRLST  BGスクロールレジスタの設定 */
    case 0xC9:    /* _BGSCRLGT  BGスクロールレジスタの読み出し */
    case 0xCA:    /* _BGCTRLST  BGコントロールレジスタの設定 */
    case 0xCB:    /* _BGCTRLGT  BGコントロールレジスタの読み出し */
    case 0xCC:    /* _BGTEXTCL  BGテキストのクリア */
    case 0xCD:    /* _BGTEXTST  BGテキストの設定 */
    case 0xCE:    /* _BGTEXTGT  BGテキストの読み出し */
    case 0xCF:    /* _SPALET    スプライトパレットの設定 */
        return 0; /* ROM IOCS ハンドラへ委譲 */

    /* _OPMDRV は 0x68/0x69 と同様に ROM から OPM レジスタへ委譲する。 */
    case 0xF0:    /* _OPMDRV */
        return 0; /* ROM IOCS ハンドラへ委譲 */

    /* _SCSIDRV: 実体はSCSIINROM($FC0000)内にあり、うちでは再実装しない。
     * SCSIINROM 起動時に _B_INTVCS(d1=$1F5) で $0400+$F5*4=$7D4 に実ハンドラを
     * 登録済みのため、ROM の iocs_dispatcher(ff1e10) 経由でそこへ委譲する。 */
    case 0xF5:    /* _SCSIDRV */
        return 0; /* ROM IOCS ハンドラへ委譲 */

    case 0xFD: /* _ABORTRST: アボートするための環境を再設定する。実ROMハンドラ($FF063E)へ
                * 委譲する（return 0）。ROMハンドラは$0ADE/$0BC4/$0BC5/$0BBFの
                * OSアボート状態ワークをクリアし、MFP IMRA bit4(USART RX割り込み)を
                * 再許可する自己完結コードで、触れるハードウェアはMFPレジスタ1本
                * (bset #4,$E88013)のみ。mfp.cはRMW/RX FIFO昇格を正しく扱うため、
                * 委譲すればキーボード受信が正しく再開する。0x4E/0x4Fのような
                * 物理排出の副作用は無い。 */
        return 0;

    case 0xFE: /* _IPLERR: ブートデバイスが見つからない場合にIPLのブート走査ループ
                * ($FF01E6付近)が最終フォールバックとして踏む再起動処理($FF0258)。
                * a7を$2000へ強制再設定しブート判定を先頭($FF0010)からやり直す、
                * 実行中コンテキストを一切保存しない破壊的な強制ソフトリブート。
                * ただし呼び出し元はIPL自身の`bra _IPLERR`のみで、これはROM内の
                * 直接分岐でありTRAP #15(IOCS)を経由しない。Human68k起動後は
                * HUMAN.SYSも常駐ドライバもこのIOCS番号を経路として使わないため、
                * TRAP #15 D0=0xFEとして実際に呼ばれることは無いと判断する
                * （調査済み。他経路からの呼び出しは考慮しない前提）。
                * 実害の無い経路のため他の未実装番号と同様default委譲に委ねてよいが、
                * 破壊的ハンドラである旨を明記するため明示caseとして残す。 */
        return 0;

    case 0xFF: /* _ABORTJOB: 実行中のコマンド/プロセスを中断しコマンドラインへ復帰する
                * （アボート）。Human68kは起動時にIOCSベクタ表$07FC(=$0400+0xFF*4)を
                * 自前ハンドラL0086f2($0086f2)へ差し替える（HUMAN.SYS $007942の
                * `move.l #L0086f2,($07fc)`）。L0086f2は`movea.l ($1c62),a7`で保存済み
                * アボートコンテキストのSSPを復元し、$1bc8(PC)/$1c60(SR)で新しい例外フレームを
                * 組んでrteでコマンドラインへ非局所ジャンプする（呼び出し元へは戻らない）。
                * return 0でROM iocs_dispatcher(ff1e10)経由のベクタ表ディスパッチに委ね、
                * $07FCのHUMAN.SYSハンドラを起動させる必要がある（D0=0でベクタ表への
                * ジャンプをスキップするとHUMAN.SYSハンドラが起動せず、rteがTRAP#14
                * 発行元のディスクドライバへ戻り再アクセス→再エラーの無限ループになる）。
                * 0xF5 _SCSIDRVと同型の「常駐側が登録したIOCSベクタへの委譲」であり、
                * アボートコンテキスト($1c62/$1bc8/$1c60等)はHUMAN.SYSが通常RAM上で
                * 管理するためエミュレータ側にメモリ管理構造体の追加実装は不要。 */
        return 0;

    case 0x81: /* _B_SUPER: スーパーバイザ/ユーザモード切り換え。実ROMハンドラ($FFAC14)へ
                * 委譲する。ハンドラはTRAP例外フレーム上のSRを直接書き換える
                * 自己完結コードで、他のIOCS/DOSコールに依存しない（DOS _SUPER $ff20の
                * dos_super($00A438)も同様の自己完結ロジックで、これとは独立して動作する）。 */
        return 0;

    /* 0x8A-0x8C _DMAMOVE/_DMAMOV_A/_DMAMOV_L: DMAC ch2($E84080-)を使った
     * 汎用メモリ間転送。
     * ROMハンドラ($FFAC8A/$FFAD16/$FFAD66)はOCR/SCR/MTC/MAR/DAR/BTC/BARを
     * 設定してCCRへ#$88(START)を書くだけで即rtsするため、実転送と完了通知は
     * dmac_write8()のCCR横取り(dmac2_start())側で行う。ROMは転送完了を
     * ポーリングせず、レベル3・ベクタ$68の割り込みでのみ待ちフラグ($C34)を
     * 解除するため、DMAC ch2の実装（レベル3割り込み配線含む）が前提
     * （『Inside X68000』 図3/図12/図16で仕様確認、
     * IPLROMXV.S ffac8a-ffad8a 逆アセンブルでROM側のレジスタ設定順を確認）。 */
    case 0x8A: /* _DMAMOVE */
    case 0x8B: /* _DMAMOV_A */
    case 0x8C: /* _DMAMOV_L */
        return 0;
    /* 0x8D _DMAMODE: 上記$C34を読むだけの単純な状態検査。DMAC ch2実装と
     * セットで委譲する（自前実装すると$C34の値と二重管理になり不整合の元）。 */
    case 0x8D: /* _DMAMODE */
        return 0;

    case 0x8E: /* _BOOTINF: 起動情報。実ROMハンドラ($FFAD92)へ委譲する。
                * 実ハンドラはboot_dispatcher自身が記録した実際のブート方法を
                * 返すため、$ED0018を読んでこちらで推測するより正確・堅牢
                * （SRAM $ED0018がSCSI16IN経由の起動優先度を指していても、
                * 固定値を返すとFDD起動と誤認しブート元判定処理が誤動作する）。 */
        return 0;
    default:
        /* 未実装の IOCS 呼び出し: ROM ハンドラへ委譲する。IOCS ベクタテーブル
         * ($0400 + fn*4) は IPLROM が全256エントリ分を初期化しており、
         * 「(未公開)」とされる番号にも実ハンドラが存在する
         * （例: $91 グラフィック画面モードの設定 = $FFB114 は R20 上位バイトと
         * ビデオコントローラR0($E82400)を、$93 画面表示及び特殊モードの設定 =
         * $FFB16C は R2($E82600)を設定する）。
         * D0=0 を返して握りつぶすと呼び出し元は「設定できた」と誤認したまま進む。
         * mint.xのPICビューアが$93/$91で512x512・65536色へ切り替えられず、
         * 768x512のまま画像が左右に折り返して表示される不具合の原因だった。
         * 番号自体をログには残し、未実装の存在に気付けるようにする。 */
        pal_log("IOCS unimplemented fn=0x%02X D1=%08X D2=%08X A1=%08X PC=%08X (ROM委譲)\n", fn,
                regs.d[1], regs.d[2], regs.a[1], mc68k_get_pc(cpu));
        return 0;
    }
    return 1; /* handled — ベクタテーブルへのジャンプをスキップ */
}
