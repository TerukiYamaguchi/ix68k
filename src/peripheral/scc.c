#include "peripheral/scc.h"
#include <string.h>

/* IOCSのマウスカーソル座標ワーク（上位16bit=X、下位16bit=Y）。
 * XEiJ Z8530.java（MMUなし分岐）・実機ROM双方で確認済み。 */
#define IOCS_MOUSE_CURSOR_XY 0x0ACEu

/* IOCSマウスドライバの加速度カーブを打ち消す「逆アクセラレーションテーブル」
 * （XEiJ Mouse.java musInit()の生成ロジック準拠）。IOCS側は受信したdx/dyが
 * 大きいほど実際のカーソル移動量を非線形に増幅するため、大きな移動量を
 * そのまま送ると（特に高速に動かした際）カーソルが実際の位置を追い越して
 * 振動する。事前にこのテーブルで変換した値を送ることで、ホストの動きに
 * リニアに追従させる。 */
static int s_decel_table[1025];
static int s_decel_table_built = 0;

static void build_decel_table(void) {
    if (s_decel_table_built)
        return;
    s_decel_table_built = 1;
    int index = 0;
    for (int delta = 0; delta <= 81; delta++) {
        int next = delta + 1;
        if (next >= 8)
            next *= next >> 3;
        next += next >> 2;
        while (index < next && index <= 1024)
            s_decel_table[index++] = delta;
    }
}

/* rawの符号を保ったまま、絶対値をs_decel_table経由で変換する。 */
static int decel_lookup(int raw) {
    build_decel_table();
    if (raw == 0)
        return 0;
    int mag = raw > 0 ? raw : -raw;
    if (mag > 1024)
        mag = 1024;
    int v = s_decel_table[mag];
    return raw > 0 ? v : -v;
}

void scc_reset(scc_t *scc) {
    /* mem_read32コールバックはx68k_bus.c初期化時に一度だけ登録される
     * bus層の橋渡し。ソフトリセット(RESET命令)でも呼ばれるscc_resetの
     * memsetで消してしまうと以後マウスが完全に無反応になるため保持する。 */
    scc_mem_read32_fn saved_fn = scc->mem_read32;
    void *saved_ctx = scc->mem_read32_ctx;
    memset(scc, 0, sizeof(*scc));
    scc->mem_read32 = saved_fn;
    scc->mem_read32_ctx = saved_ctx;
}

/* ベクタ = WR2 に、WR9のベクタ修飾モードが有効ならch B Rx Char Available
 * ステータス(3bit値=2)をbit3-1へ埋め込む。Human68k設定(WR2=$50,WR9=$09等)
 * では常に $54 になる（ROM/XEiJで裏取り済み）。 */
static uint8_t scc_vector(const scc_t *scc) {
    if (scc->wr9 & 0x11u)
        return (uint8_t)((scc->wr2 & 0xF1u) | 0x04u);
    return scc->wr2;
}

static uint8_t scc_rr0_b(const scc_t *scc) {
    uint8_t rr0 = 0x24u; /* TxEmpty(bit2)固定 + DCD(bit5)固定（マウス常時接続扱い） */
    if (scc->rx_count > 0)
        rr0 |= 0x01u; /* Rx Character Available */
    return rr0;
}

static uint8_t scc_data_read_b(scc_t *scc) {
    if (scc->rx_count == 0)
        return 0x00u;
    uint8_t v = scc->rx_data[scc->rx_index];
    scc->rx_index++;
    scc->rx_count--;
    if (scc->rx_count == 0)
        scc->rx_index = 0;
    return v;
}

/* WR0=$38(Reset Highest IUS)。まだ未読バイトが残っていれば次バイトの
 * 割り込みを再武装する。読み出し直後の自動再武装は行わない（キーボード
 * 受信FIFO(mfp.c)と同型の再入バグになるため、明示的な$38コマンド受信時
 * のみ再武装する）。 */
static void scc_ius_reset(scc_t *scc) {
    if (scc->rx_count > 0 && (scc->wr1 & 0x18u))
        scc->recv_request = 1;
}

/* RTS(WR5 bit1) 0→1 立ち上がりで、IOCS側の現在のマウスカーソル座標($0ACE)と
 * ホストの現在位置(host_x/y)の差分をその場で計算し、3バイト応答
 * (status/dx/dy)として送出する（XEiJ Z8530.java準拠）。差分をこちら側で
 * 蓄積・保持しないため、基準点がズレたまま累積することがない。
 * 送信直前にdecel_lookup()でIOCS側の加速度カーブを打ち消す（そのまま送ると
 * 特に高速移動時にIOCS側で移動量が増幅され、実位置を追い越して振動する）。 */
static void scc_wr5_write(scc_t *scc, uint8_t val) {
    uint8_t rts_now = (uint8_t)((val >> 1) & 1u);
    if (rts_now && !scc->rts_prev) {
        int raw_dx = 0, raw_dy = 0;
        if (scc->mem_read32) {
            uint32_t cur = scc->mem_read32(scc->mem_read32_ctx, IOCS_MOUSE_CURSOR_XY);
            int cur_x = (int16_t)(cur >> 16);
            int cur_y = (int16_t)(cur & 0xFFFFu);
            raw_dx = scc->host_x - cur_x;
            raw_dy = scc->host_y - cur_y;
        }
        int dx = decel_lookup(raw_dx);
        int dy = decel_lookup(raw_dy);
        /* decel_lookup()は|raw|>1024を1024にクランプして変換するため、
         * オーバーフロー判定の閾値もそれに合わせる（XEiJ準拠、通常の速度の
         * 動きでは1024ドット/RTSに達することはまず無くstatusは立たない）。 */
        uint8_t status = 0;
        if (raw_dx > 1024)
            status |= 0x10u; /* Xオーバフロー */
        if (raw_dx < -1024)
            status |= 0x20u; /* Xアンダフロー */
        if (raw_dy > 1024)
            status |= 0x40u; /* Yオーバフロー */
        if (raw_dy < -1024)
            status |= 0x80u; /* Yアンダフロー */
        if (scc->btn_left)
            status |= 0x01u;
        if (scc->btn_right)
            status |= 0x02u;
        scc->rx_data[0] = status;
        scc->rx_data[1] = (uint8_t)(int8_t)dx;
        scc->rx_data[2] = (uint8_t)(int8_t)dy;
        scc->rx_count = 3;
        scc->rx_index = 0;
        if (scc->wr1 & 0x18u)
            scc->recv_request = 1;
    }
    scc->wr5 = val;
    scc->rts_prev = rts_now;
}

/* WR9 bit7-6: 01=ch Bリセット, 11=HWリセット。受信状態・RTSエッジ検出を
 * 初期化する（ch A個別リセットの厳密な区別は本実装では行わない。ch A/B
 * 共通でクリアする、下記参照）。 */
static void scc_wr9_write(scc_t *scc, uint8_t val) {
    scc->wr9 = val;
    if (val & 0xC0u) {
        scc->rts_prev = 0;
        scc->rx_count = 0;
        scc->rx_index = 0;
        scc->recv_request = 0;
        /* ch A 側も同時にクリアする。ビット単位での
         * ch A/B個別リセットの区別は本実装では行わない（ch B同様の簡略化
         * 方針、上記コメント参照）。 */
        scc->rx_a_head = 0;
        scc->rx_a_count = 0;
        scc->tx_a_head = 0;
        scc->tx_a_count = 0;
        scc->recv_request_a = 0;
    }
}

static uint8_t scc_read_b(scc_t *scc, int is_data) {
    if (is_data)
        return scc_data_read_b(scc);
    uint8_t reg = scc->wr0_regsel;
    scc->wr0_regsel = 0; /* 読み出し後はWR0選択に自動復帰 */
    switch (reg) {
    case 0:
        return scc_rr0_b(scc);
    case 2:
        return scc_vector(scc);
    default:
        return 0x00u; /* RR3含め本実装では常に0（ソフトはここを見ない） */
    }
}

static void scc_write_b(scc_t *scc, int is_data, uint8_t val) {
    if (is_data)
        return; /* マウス→ホスト方向の送信は無し、無視 */
    uint8_t reg = scc->wr0_regsel;
    if (reg == 0) {
        if ((val & 0xF0u) == 0u) {
            scc->wr0_regsel = val & 0x0Fu; /* レジスタ選択(0-15) */
        } else if (val == 0x38u) {
            scc_ius_reset(scc);
        }
        /* 0x10/0x30/0x80等その他コマンドは本実装では効果なし(no-op) */
        return;
    }
    scc->wr0_regsel = 0; /* 書き込み後はWR0選択に自動復帰 */
    switch (reg) {
    case 1:
        scc->wr1 = val;
        break;
    case 2:
        scc->wr2 = val;
        break;
    case 5:
        scc_wr5_write(scc, val);
        break;
    case 9:
        scc_wr9_write(scc, val);
        break;
    default:
        break; /* WR3/4/6/7/10-15: 通信フォーマット等、マウス動作に無関係(no-op) */
    }
}

/* ベクタ = WR2に、WR9のベクタ修飾モードが有効ならch A Rx Char Available
 * ステータス(3bit値=6)をbit3-1へ埋め込む。Human68k設定(WR2=$50)では
 * $5C になる（ch Bの$54とは別値。Z8530のステータスコード表に基づく）。
 * RR2の通常読み出し(IACK以外)では未修飾のwr2をそのまま返すのが実チップの
 * 仕様（ch Bのみ修飾読み出しに対応）で、scc_read_a() 側は変更しない。 */
static uint8_t scc_vector_a(const scc_t *scc) {
    if (scc->wr9 & 0x11u)
        return (uint8_t)((scc->wr2 & 0xF1u) | 0x0Cu);
    return scc->wr2;
}

static uint8_t scc_rr0_a(const scc_t *scc) {
    uint8_t rr0 = 0x04u; /* TxEmpty固定（送信は都度リングへ積むだけで常時受理できるため） */
    if (scc->rx_a_count > 0)
        rr0 |= 0x01u; /* Rx Character Available */
    if (scc->carrier_a)
        rr0 |= 0x20u; /* DCD相当: ホスト接続時のみ1 */
    return rr0;
}

/* WR0=$38(Reset Highest IUS)のch A版。ch Bのscc_ius_reset()と同型（未読
 * バイトが残っていれば次バイトの割り込みを再武装する）。 */
static void scc_ius_reset_a(scc_t *scc) {
    if (scc->rx_a_count > 0 && (scc->wr1_a & 0x18u))
        scc->recv_request_a = 1;
}

/* ch A(RS-232C)の透過データ経路。
 * WR3/4/5(キャラクタ長・パリティ・ストップビット等)は書き込みを保持
 * するのみでタイミング動作には反映しない。WR2(ベクタベース)とWR9
 * (マスタ割り込み制御)は実チップ上ch A/B共有の物理レジスタで、ROMは
 * WR2=$50をch Aのポート経由で書く（ROM/XEiJで確認済み）。 */
static uint8_t scc_read_a(scc_t *scc, int is_data) {
    if (is_data) {
        if (scc->rx_a_count == 0)
            return 0x00u;
        uint8_t v = scc->rx_a[scc->rx_a_head];
        scc->rx_a_head = (scc->rx_a_head + 1) % SCC_CH_A_RING_CAP;
        scc->rx_a_count--;
        return v;
    }
    if (scc->wr0_regsel_a == 0)
        return scc_rr0_a(scc);
    uint8_t reg = scc->wr0_regsel_a;
    scc->wr0_regsel_a = 0;
    if (reg == 2)
        return scc->wr2; /* RR2(ch A)は未修飾のベクタを返す(実チップ仕様) */
    if (reg == 9)
        return scc->wr9;
    return 0x00u;
}

static void scc_write_a(scc_t *scc, int is_data, uint8_t val) {
    if (is_data) {
        if (scc->tx_a_count < SCC_CH_A_RING_CAP) {
            int tail = (scc->tx_a_head + scc->tx_a_count) % SCC_CH_A_RING_CAP;
            scc->tx_a[tail] = val;
            scc->tx_a_count++;
        } /* 満杯時は捨てる（TxEmpty常時1のためROM側は待たずに書き続けるが、
           * 自作テスト規模の文字列送受信ではリング容量を超えない） */
        return;
    }
    if (scc->wr0_regsel_a == 0) {
        if ((val & 0xF0u) == 0u) {
            scc->wr0_regsel_a = val & 0x0Fu;
        } else if (val == 0x38u) {
            scc_ius_reset_a(scc);
        }
        return;
    }
    uint8_t reg = scc->wr0_regsel_a;
    scc->wr0_regsel_a = 0;
    switch (reg) {
    case 1:
        scc->wr1_a = val;
        break;
    case 2:
        scc->wr2 = val;
        break;
    case 3:
        scc->wr3_a = val;
        break;
    case 4:
        scc->wr4_a = val;
        break;
    case 5:
        scc->wr5_a = val;
        break;
    case 9:
        scc_wr9_write(scc, val);
        break;
    default:
        break; /* WR6/7/10-15: 同期モード等、非同期透過には無関係(no-op) */
    }
}

uint8_t scc_read8(scc_t *scc, uint32_t addr) {
    if (!(addr & 1u))
        return 0x00u; /* 偶数バイトは未接続（D7-D0のみ実配線、MFPと同様） */
    int off = (int)(addr & 0x7u);
    int is_data = (off & 2) != 0;
    int is_ch_a = (off & 4) != 0;
    return is_ch_a ? scc_read_a(scc, is_data) : scc_read_b(scc, is_data);
}

void scc_write8(scc_t *scc, uint32_t addr, uint8_t val) {
    if (!(addr & 1u))
        return;
    int off = (int)(addr & 0x7u);
    int is_data = (off & 2) != 0;
    int is_ch_a = (off & 4) != 0;
    if (is_ch_a)
        scc_write_a(scc, is_data, val);
    else
        scc_write_b(scc, is_data, val);
}

void scc_set_mem_read32(scc_t *scc, scc_mem_read32_fn fn, void *ctx) {
    scc->mem_read32 = fn;
    scc->mem_read32_ctx = ctx;
}

void scc_rebase_host_pos(scc_t *scc) {
    if (!scc->mem_read32)
        return;
    uint32_t cur = scc->mem_read32(scc->mem_read32_ctx, IOCS_MOUSE_CURSOR_XY);
    scc->host_x = (int16_t)(cur >> 16);
    scc->host_y = (int16_t)cur;
}

void scc_mouse_set_host_pos(scc_t *scc, int x, int y) {
    scc->host_x = x;
    scc->host_y = y;
}

void scc_mouse_button(scc_t *scc, int left, int right) {
    scc->btn_left = (uint8_t)(left != 0);
    scc->btn_right = (uint8_t)(right != 0);
}

int scc_pending_irq(const scc_t *scc) {
    return (scc->recv_request && (scc->wr1 & 0x18u) != 0u) ||
           (scc->recv_request_a && (scc->wr1_a & 0x18u) != 0u);
}

uint8_t scc_iack(scc_t *scc) {
    scc->recv_request = 0;
    return scc_vector(scc);
}

uint8_t scc_iack_a(scc_t *scc) {
    scc->recv_request_a = 0;
    return scc_vector_a(scc);
}

void scc_recv_a(scc_t *scc, const uint8_t *buf, int n) {
    for (int i = 0; i < n; i++) {
        if (scc->rx_a_count >= SCC_CH_A_RING_CAP)
            break; /* 満杯: 以降は捨てる */
        int tail = (scc->rx_a_head + scc->rx_a_count) % SCC_CH_A_RING_CAP;
        scc->rx_a[tail] = buf[i];
        scc->rx_a_count++;
    }
    if (scc->rx_a_count > 0 && (scc->wr1_a & 0x18u))
        scc->recv_request_a = 1;
}

int scc_send_a_dequeue(scc_t *scc, uint8_t *out, int max) {
    int n = 0;
    while (n < max && scc->tx_a_count > 0) {
        out[n] = scc->tx_a[scc->tx_a_head];
        scc->tx_a_head = (scc->tx_a_head + 1) % SCC_CH_A_RING_CAP;
        scc->tx_a_count--;
        n++;
    }
    return n;
}

void scc_serial_set_carrier(scc_t *scc, int connected) {
    scc->carrier_a = (uint8_t)(connected != 0);
}
