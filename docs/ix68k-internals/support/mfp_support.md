# MFP (MC68901) 実装状況

`src/peripheral/mfp.c` + `src/bus/x68k_bus.c`（IACK部）。関連: FDC は
`docs/ix68k-internals/support/fdc_support.md`、DMAC は `docs/ix68k-internals/support/dmac_support.md`、IOCS は `docs/ix68k-internals/support/iocs_support.md`、
割り込み系統の全体像は `docs/ix68k-internals/addrmap.md` を参照。

> **必須**: MFP のレジスタ・割り込みチャンネルを実装・変更したら、この
> ファイルの該当表を必ず更新します。

---

> 2026-07-29 全面監査で全面書き直し。旧版は
> Timer A/D・ラスタ一致(GPIP6)・H-SYNC(GPIP7)実装より前の内容で古くなってきました。
> 監査は「『Inside X68000』記載の正式レジスタ/割り込みチャンネル一覧」と
> `src/peripheral/mfp.c` + `src/bus/x68k_bus.c`（IACK部）のgrep突き合わせによる
> **存在監査**（実装の有無）。ビット単位の正しさまでは保証しません。

### USART（キーボード）

- IERA/IMRA 書き込みで USART RX 割り込みを arm。
- `mfp_tick` がキーボードバイトを UDR（`$E8802F`）経由でキューから供給。
- FDC IRQ 処理後も MFP IRQ6 が pending のままなら、`x68k_bus_irq_level()` が次の命令境界で再び 6 を返す（明示的な復元処理は不要）。

### Timer C

- `x68k_bus_mfp_set_tcdcr(bus, 0x70)` で div/200 モードとして再起動。
- `MFP_TICK_CYCLES=200` サイクル単位で `mfp_tick` を呼び出し（Timer C 周期 ~10000 cy）。

### 割り込み16チャンネルの実装状況（優先度降順、`x68k_bus.c` IACK level6分岐）

MC68901は16種の割り込み要因を固定優先度で持つ（ch15最高〜ch0最低、ベクタ下位4bit=ch番号）。
『Inside X68000』のMFP割り込みチャンネル一覧の正式割り当てとの突き合わせ:

| ch | レジスタ位置 | 要因 | 実装状況 |
|---|---|---|---|
| 15 | IERA bit7 | GPIP7 (H-SYNC) | ✅ 実装済み（擬似走査線） |
| 14 | IERA bit6 | GPIP6 (CIRQ, ラスタ一致) | ✅ 実装済み |
| 13 | IERA bit5 | Timer A | ⚠️ V-DISP終了時のイベントカウントのみ（1フレーム1回減算）。CRTCラスターモデル無効時はフレーム末尾へフォールバック。ディレイモード/パルス幅測定モードは未実装 |
| 12 | IERA bit4 | 受信バッファフル (USART RX/キーボード) | ✅ 実装済み（FIFO） |
| 11 | IERA bit3 | 受信エラー | ❌ 未実装 |
| 10 | IERA bit2 | 送信バッファ空 | ❌ 未実装 |
| 9 | IERA bit1 | 送信エラー | ❌ 未実装 |
| 8 | IERA bit0 | **Timer B** | ✅ 実装済み（ディレイモードのみ）。IPLROM既定はIERA/IMRA bit0=0固定のため通常は割り込み自体発生しません。イベントカウント/パルス幅測定はX68000ではTBI端子L固定のため停止扱い |
| 7 | IERB bit7 | GPIP5 | 該当なし（実機で外部Hレベル固定・未使用ピンなので割り込み自体発生しません。対応不要） |
| 6 | IERB bit6 | **GPIP4 (V-DISP)** | ✅ 実装済み。CRTCラスターモデルのV-DISP終了時に毎フレーム1回発火し、モデル無効時はフレーム末尾へフォールバック。Timer A(TAI)と同じ物理線を共有するが独立チャンネルとして扱います。AERのエッジ選択は未反映 |
| 5 | IERB bit5 | Timer C | ✅ 実装済み |
| 4 | IERB bit4 | Timer D | ✅ 実装済み |
| 3 | IERB bit3 | GPIP3 (FMIRQ/OPM) | ✅ 実装済み |
| 2 | IERB bit2 | GPIP2 (POW SW, 電源スイッチ) | ✅ 実装済み。ix68k起動時はON相当のLOW。入口は File メニューの「Power Switch (OFF)」（`pal_power_switch_off_requested()` → `x68k_bus_mfp_power_switch(b, 0)`）。AER bit2で選ぶ遷移をIERB/IPRB/IMRB経由でch2へ反映 |
| 1 | IERB bit1 | GPIP1 (EXPON, 拡張スロット) | ❌ 未実装 |
| 0 | IERB bit0 | GPIP0 (ALARM, RTC) | ✅ 実装済み。RTC ALARMのLOW/HIGH遷移をAER bit0で選び、IERB/IPRB/IMRBを通じてch0へ反映 |

### レジスタ単位の実装状況

`mfp_reg_read8`/`mfp_reg_write8`（`src/peripheral/mfp.c:99-282`）が扱う範囲。
アドレス・ビット配置は『Inside X68000』のMFP章（80,81,88,92,94,96,102ページ相当）準拠。

| レジスタ | アドレス | 実装状況 |
|---|---|---|
| GPIP | $E88001 | ⚠️ **bit0=RTC ALARM（通常HIGH、ALARM時LOW）、bit2=POW SW（ix68k起動時はON相当のLOW）**。**bit7=H-SYNC・bit4=V-DISP**で、どちらもCRTCラスターモデル由来（`-mfp-gpip-raster on`が既定）。`raster.valid=0`・R00-R03不正・`off`指定時だけ位相近似へフォールバックし、常時HIGH/LOWにはしません。bit7は200サイクルチャンク粒度により単発待ちが長くなる場合があります。**bit3(FMIRQ)は`b->opm.fmirq_line`、bit6(CIRQ)は`b->raster.line`とR09の一致**を反映します。現行仕様は`mfp_design.md` §5.1・§8を参照 |
| AER | $E88003 | ⚠️ 値を保持。**bit0はGPIP0(ALARM)、bit2はGPIP2(POW SW)の立上り/立下り選択として機能**します。V-DISP関連bit4等はエッジ方向としての機能が無い（`x68k_bus_mfp_vdisp_tick`はフレーム境界で無条件加算するため意味を持たない簡略化） |
| DDR | $E88005 | ❌ レジスタ自体が存在しない（read/writeともcase文に無くdefaultで0x00返却/書き捨て）。実機は常時全入力(0x00)設定なので実害は薄いと推測されるが、読み戻し検証をするソフトがあれば矛盾する |
| IERA/IERB | $E88007/$E88009 | ✅ 値は保持。実際に割り込み条件判定に使われるのは上表で✅の各bitのみ |
| IPRA/IPRB | $E8800B/$E8800D | ✅ 保持・各実装済みチャンネルが更新。ゲスト書き込みもAND意味論（`&=`）。書き込み後は`imr & ipr`で各チャンネルのpendingを再評価する（USART_RXは別経路で管理のため対象外） |
| ISRA/ISRB | $E8800F/$E88011 | ✅ 実装済み。書き込みはAND意味論（`&=`、"0"を書いたビットだけクリア）。IACKで該当ビットをセット（S=1のときのみ）。S=1のときはIn-Serviceゲーティング（下位優先度チャンネルのブロック）が有効、S=0（既定）では常に全許可に退化 |
| IMRA/IMRB | $E88013/$E88015 | ✅ 実装済み各チャンネルのマスクとして機能。書き込み時のpending再評価はチャンネルごとに対称（`(ier & ipr & imr)`が非0ならpendingへ昇格、0ならクリア。修正済み）。旧実装はクリア方向のみで、マスク中に条件成立した割り込みがアンマスク時にpendingへ昇格されず永久に取りこぼされるバグがあった（ZMUSICがFMIRQをマスク/アンマスクしながら使う際にOPM Timer Aと競合し、男弾のBGMが1音で止まる不具合として発現） |
| VR | $E88017 | ✅ 実装済み。上位4bit(V7-V4)はベクタ生成に使用。bit3(S)はソフトウェアEOIモード選択として`mfp_vr_s`に分離保持、読み戻しも可能。既定は`-mfp-soft-eoi off`（事前の実測でS=1書き込みが観測されなかったため）で、この間はXEiJ同様S=0固定・常に自動EOI相当。`-mfp-soft-eoi on`でS=1書き込みが有効化される |
| TACR | $E88019 | ⚠️ イベントカウントモード(bit3)のみ対応。ディレイモード/パルス幅測定モード（bit3=0またはbit3=1かつ他ビットで指定）は値の保持のみで動作しない |
| TBCR/TBDR | $E8801B/$E88021 | ✅ 実装済み（ディレイモード）。TBCR bit7(RST)/bit4(TBO)は保持しない（読み出しは`&0x0F`）。TBO→USART伝送クロックの実配線は非スコープ（USART RXはFIFOショートカット実装のため無関係） |
| TCDCR | $E8801D | ✅ 実装済み（div/200等のプリスケールモード、`mfp_tc_prescale`） |
| TADR/TC/TCDR | $E8801F/$E88023 | ✅ 実装済み（Timer A=V-DISPイベントカウント近似、TCは初期値レジスタ+現在カウンタでロールオーバー再ロード） |
| TDDR | $E88025 | ✅ 実装済み（Timer D） |
| SCR | $E88027 | ❌ レジスタ自体が存在しません。ただしInside X68000本文に「X68000ではUSARTを非同期モードで使用するのでSCRは無視してよい」と明記されており、意図的な省略として妥当 |
| UCR | $E88029 | ❌ レジスタ自体が存在しません。USART RXはFIFOショートカット実装（実際のシリアルフレーミングを模していない）のため、通信モード設定値自体を参照する箇所が無く実害は無いと推測されるが、読み戻し検証をするソフトがあれば矛盾する |
| RSR（USART受信） | $E8802B | ✅ 実装済み（bit7=BFのみ。他ビットのRE/SS等は未反映） |
| TSR（USART送信） | $E8802D | ⚠️ 常時ready(0xFF)を返すのみ。UDR書き込みは即時に受理される（ROMの$ff6502ポーリングは1回で抜ける） |
| UDR（USART受信） | $E8802F | ✅ 実装済み（FIFO経由のarmed/pending制御込み） |
| UDR（USART送信） | $E8802F | ⚠️ 書き込みをキーボードコマンドとしてデコードし保持（LED=$80-$FF、LED明るさ=$54-$57、リピート開始時間=$60-$6F、リピート間隔=$70-$7F。その他は件数のみ）。送信時間・TSR BE遷移は未再現。保持値はホスト側のキーリピート・UIへ未反映 |

### 既知の未実装・要検証まとめ

- ch11-9(USART RX err/TX empty/TX err)、ch1(EXPON)は未実装だが実害未確認。ch0 ALARM、ch2 POW SWは対応済み。ch8 Timer Bは対応済み
- GPIP bit6(CIRQ)/bit3(FMIRQ)の実状態反映は対応済み。ポーリングでこれらを見るソフトが実在するかは未確認
- GPIP bit4(V-DISP)/bit7(H-SYNC)ともラスター由来化まで対応済み（既定`-mfp-gpip-raster on`）。MCP検証でハング・目立った遅延は観測されありませんでした。200サイクル量子化により単発 H-SYNC 待ちが長くなる制約は `mfp_design.md` §8 を参照
- VRのSビット（ソフトウェアEOI）・ISRA/ISRBのIn-Serviceゲーティングは実装済みだが、既定は`-mfp-soft-eoi off`。S=1側の実地検証は実CPU上での能動検証のみ
- **IPRA/IPRB書き込み時のpending再評価修正（2026-08-20）で判明した残課題（未起票）**:
  - IPRA bit4(USART_RX)は`mfp_reeval_group_a_pending()`の対象外のまま。IMRA書き込み側の既存除外（pendingがFIFO駆動でレジスタ値だけからは「set」できないための意図的除外、`mfp_usart_rx_try_load_latch`で別管理）をそのまま踏襲したが、IPRA書き込み側は「クリアする方向」だけが問題になる上記のIMRA/IMRB再評価漏れと同型のバグ余地が残る。ソフトウェアがUDR読み出し経由（`mfp_reg_read8` `:276-278`）ではなく明示的にIPRAへ書き込んでbit4を落とした場合、`ch_pending`のUSART_RXビットは連動してクリアされない
  - 当初計画にあった「XEiJ `MC68901.java`のIPR書き込み時のAND意味論・再評価挙動との突き合わせ」は今回未実施（実装のみで検証は行っていない）


## 変更履歴

| 日付 | 内容 |
|------|------|
| 2026-08-20 | 下記のレベル6スプリアス割り込み対策C（2026-08-20）で「複雑・副作用リスク大」として見送っていた根本原因（200サイクル窓）を、mc68000コア側にpull型IRQ受付（`mc68k_set_irq_level_hook`）を追加する形で解消。`x68k_bus_irq_level()`（NMI>IOI>DMAC>SCC>MFPの優先度判定、副作用なし）を命令境界ごとにCPUコアが直接問い合わせるようになり、`x68k_cycle_catchup()`の200サイクル粒度によるIRQ線更新の遅延窓自体がなくなりました。`x68k_bus_pin_spurious_vector()`（対策C）は理論上不要になった可能性があるが、既存の安全網を崩すリスクを避けるため今回は削除せず維持（別課題として検討）。`ZP F:\MUSIC\G9.ZMS`約35秒再生で白帯なし・CPU正常応答を確認。性能実測（ヘッドレスbefore/after、Human68kアイドル20秒×3）でCPU時間+9〜10%増を確認したが、60fpsペーシング下で体感影響なし（正しさ優先、キャッシュ化は見送り）。**将来課題（`ipl_dirty`キャッシュ化）**: `x68k_bus_irq_level()`を毎命令境界で素通し呼びせず、`bus`に`cached_ipl`/`ipl_dirty`を持たせてダーティ時のみ5判定関数（IOI/DMAC ch2/DMAC ch3/SCC/MFP）を再計算する案。効果はマイクロベンチ内訳から低シングル桁%程度まで下がると推定（未実測）。難易度は書き込み箇所の網羅性に集中: MFPは`mfp_ch_pending_set/clear`ヘルパー2箇所（27呼び出し元を集約済み）に手を入れれば足りるが、DMAC ch2/ch3 `irq_pending`（7箇所）・IOI `ioi_request`/`ioi_enable`（13箇所）・SCC `recv_request(_a)`（8箇所）は未集約の生フィールド書き込みで個別に約28箇所へダーティ化を挿入する必要があります。加えて`bus_iack()`（受け付け時のクリア）を1箇所でも見落とすと「古いIRQの再受付」という過去のIRQ関連の不具合と同型の不具合を再生産するリスクがあります。実害（低スペック環境でのフレーム落ち等）が実際に観測された場合に着手する方針とし、現時点では未着手 |
| 2026-08-20 | IPRA/IPRB書き込みハンドラ（`case 0xE8800B`/`0xE8800D`）が`ipra`/`iprb`をANDクリアするだけで`mfp_ch_pending`を再評価していなかったバグを修正。IMRA/IMRB書き込みの個別再評価ロジック（USART_RXを除く8ch）を`mfp_reeval_group_a_pending()`/`mfp_reeval_group_b_pending()`へ共通化し、IPRA/IPRB書き込みからも呼ぶようにしました。修正前は「ソフトがIPRで保留を取り消してもch_pendingが残りIRQ6の線が立ち続け、取り消したはずのチャンネルの正規ベクタが返る」誤動作の余地があった（実害は未観測、コードレビューでの指摘）。`ctest`全33件通過 |
| 2026-08-20 | レベル6スプリアス割り込みの対策Cを実装。CPUへのIRQ線更新は`x68k_cycle_catchup()`（200サイクル粒度）でしか行われない一方、MFP IMRA/IMRB書き込みは`mfp_ch_pending`をその場でクリアするため、「線はレベル6のまま・pendingは空」という古い線が生じレベル6スプリアス割り込み(ベクタ24)を稀に引く（PCM8A.X+ZMUSIC ver2実演奏で15秒以内に再現）。根本原因（200サイクル窓）の解消（IRQ即時再評価コールバック、旧B案）は複雑・副作用リスク大と判断し見送り、実機（060turbo ROM）と同種の対症療法を採用: ベクタ24を`x68k_bus_pin_spurious_vector()`でIPLROM内の単独`rte`命令($0000FE006C、`nmi_handler`末尾)へ毎tick冪等に固定（`sram.c`、`x68k_cycle_catchup()`から呼ぶ）。当初RAM[0x0100]（`install_rte_stubs()`のRTEスタブ）を使う設計だったが、Human68k起動後にそこがワークエリアとして再利用され「おかしな命令」例外に化けることを動作確認で発見し、read-onlyなROM領域へ変更。`ZP F:\MUSIC\G9.ZMS`2分再生（白帯検出HALT有効）で白帯なしを確認。catchup粒度自体の粗さは別課題へ分離 |
| 2026-08-20 | レベル6スプリアス割り込みの対策Aを実装。`bus_iack()` level6の`candidates==0`分岐をオートベクタ(ベクタ30)からスプリアス割り込み(ベクタ24, `iack_spurious()`)へ変更、level1/3/5と挙動を統一 |
| 2026-08-11 | IMRA/IMRB書き込みハンドラ（`case 0xE88013`/`0xE88015`）のpending再評価をクリア方向のみから対称（set/clear両対応）に修正。マスク中に条件成立した割り込みがアンマスク時にpendingへ昇格されず取りこぼされるバグを解消（ZMUSICのFMIRQマスク/アンマスクパターンとOPM Timer Aの競合で男弾のBGMが1音のみ再生される不具合として発現・特定）。MCP動作検証でBGMが正常に鳴ることを確認 |
| 2026-07-31 | GPIP bit7のラスター由来化を実装。`mfp_raster_h_params()`/`mfp_raster_hsync_high()`でR00-R03由来のライン内位相を算出。両ビットともラスター由来のときは`gpip_fallback_phase`を進めない（読み出し副作用ゼロ）よう統合。単体テストで量子化による最大待ち増大を確認したが、MCP検証でハング・目立った遅延なしを確認 |
| 2026-07-31 | GPIP bit4のラスター由来化を実装。`gpip_raster_enabled`フラグと`-mfp-gpip-raster on\|off`（既定on）を追加。bit4(V-DISP)を`mfp_raster_vdisp_high()`でCRTCラスターモデル由来に、`raster.valid=0`または`off`時は位相ハックへフォールバック。MCP動作検証でハング・目立った遅延なしを確認 |
| 2026-07-31 | GPIP bit7/bit4の役割是正を実装。GPIP読み出しのbit7(H-SYNC)/bit4(V-DISP)の役割入れ替わりバグを是正。`vdisp_phase`を`gpip_fallback_phase`へ改名 |
| 2026-07-31 | Timer B(ch8)をディレイモードのみ対応で実装（TBCR/TBDR read/write、`x68k_bus_mfp_tick()`にカウントブロック追加、IACK/IMRA個別再評価はmfp_ch_pendingへ自動統合）。IPLROM既定はIERA/IMRA bit0=0固定のため通常は割り込み発生せず後方互換 |
| 2026-07-31 | GPIP読み出し(`$E88001`)のbit6(CIRQ)に`b->raster.line`とR09の一致を反映。`raster.valid=0`時は常時HIGH |
| 2026-07-31 | GPIP読み出し(`$E88001`)のbit3(FMIRQ)に`b->opm.fmirq_line`を反映 |
| 2026-07-31 | pending変数8個（`mfp_hsync_pending`等7個＋変則実装の`mfp_pending_vec`）を`uint16_t mfp_ch_pending`（ビット位置=チャンネル番号）へ一本化。`bus_iack()`のMFP 8分岐を最上位ビット取得1回のループへ統合 |
| 2026-07-31 | ISRA/ISRB・IPRA/IPRB書き込みをAND意味論へ、VR Sビット(ソフトウェアEOI)・IACKでのISRセット・In-Serviceゲーティング(`mfp_isr_top()`)を追加。既定`-mfp-soft-eoi off`（事前の実測でS=1書き込み未観測のため）。pending変数一本化は別課題へ分離 |
| 2026-07-29 | ch6 GPIP4/V-DISP割り込み(IERB bit6)を`x68k_bus_mfp_vdisp_tick`とIACK level6分岐に追加、OTOKO.X/XSPサンプルのハング解消を確認 |
| 2026-07-29 | 全面監査で全面書き直し。ch6 GPIP4/V-DISP割り込み完全未実装を特定、Timer B・USART TX・GPIP0-2系統も未実装と判明 |
| 2026-07-20 | 「割り込み系統」セクション新設に合わせ、レジスタ単位（GPIP/AER/DDR/Timer A-D/USART TX等）の実装状況表を追加（FM音源実装調査でGPIP4がポーリング専用ハックと判明したのを機に全体調査） |
| 2026-07-21 | MFP GPIP3(FMIRQ)配線をレジスタ表に反映（IERB/IPRB/IMRB bit3、Timer C割り込みパスと同型で実装）。CT1/CT2の二重管理・OPM内蔵Timer A/BとMFP内蔵Timer A/B/Dの混同注意を明記 |
| 2026-07-29 | `docs/hw_support.md`から本ファイルへ分離（ペリフェラル単位でのファイル分割） |
| 2026-10-10 | GPIP2 の入口として File > Power Switch (OFF) を追加し、$E8E00F の電源断シーケンスと連動 |
