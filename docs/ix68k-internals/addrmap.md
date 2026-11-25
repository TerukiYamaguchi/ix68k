# アドレスマップ（現在の実装状態）

| アドレス範囲          | サイズ  | 実装状態          | 用途                    |
|----------------------|---------|-------------------|-------------------------|
| 0x000000–RAM上限-1   | 1–12MB  | ✅ 実装済み        | メイン RAM（上限はSRAM `$ED0008` のMEMORY設定） |
| RAM上限–0xBFFFFF     | 可変    | ⚡ バスエラー       | 未増設メモリ（既定RAM上限は1MB） |
| 0xC00000–0xDFFFFF    | 2MB     | ✅ 実装済み        | GVRAM                    |
| 0xE00000–0xE7FFFF    | 512KB   | ✅ 実装済み        | TVRAM（4 プレーン）       |
| 0xE80000–0xE8FFFF    | 64KB    | 🔄 部分実装        | CRTC。内側に$E88001–$E8802F(奇数)=MFP、$E8A001–$E8BFFF(奇数、32B周期ミラー)=RTC(RP5C15)を専用デコード（[CRTC](support/crtc_support.md)、[MFP](support/mfp_support.md)、[RTC](support/rtc_support.md)） |
| 0xE90000–0xE91FFF    | 8KB     | ✅ 実装済み        | OPM FM音源（YM2151、ymfm移植の合成コア+MFP GPIP3(FMIRQ)割り込み配線まで実装済み、[詳細](support/opm_support.md)） |
| 0xE92000–0xE93FFF    | 8KB     | ✅ 実装済み        | PCM / ADPCM (MSM6258、[詳細](support/adpcm_support.md)) |
| 0xE94000–0xE95FFF    | 8KB     | 🔄 部分実装        | FDC (µPD72065B、[詳細](support/fdc_support.md)) |
| 0xE96000–0xE97FFF    | 8KB     | 🔄 部分実装        | SASI HDC / 内蔵SCSI SPC（[詳細](support/scsi_support.md)） |
| 0xE98000–0xE99FFF    | 8KB     | ✅ 実装済み        | SCC ch A（RS-232C）/ ch B（マウス） |
| 0xE9A000–0xE9BFFF    | 8KB     | 🔄 部分実装        | PPI（A/B=ジョイスティック、C=ADPCM/FDC制御） |
| 0xE9C000–0xE9DFFF    | 8KB     | 🔄 部分実装        | IOI I/O割り込みコントローラ |
| 0xE9E000–0xE9FFFF    | 8KB     | ⚡ バスエラー       | XB1 拡張ボード領域1（68000 の `fpu = on` / `68881` では `$E9E000-$E9E01F`、`68881-secondary` では `$E9E080-$E9E09F` の32Bだけ FPU-CIR（CZ-6BP1）として実装。既定の `fpu = off` は全域未装着、[詳細](support/fpu_cir_support.md)） |
| 0xEA0100–0xEA0117    | 24B     | ✅ 実装済み        | HOSTFILE裏口MMIO（ホスト⇔ゲストのファイル1本転送） |
| 0xEA0200–0xEA020F    | 16B     | ✅ 実装済み        | HOSTDRVリモートデバイス用MMIO（`HOSTDRV.SYS`が参照。旧$EA0000–$EA0027はFAT16合成方式時代の配置） |
| 0xEAFF81–0xEAFF89（奇数） | 5B | ✅ 実装済み | X68030エリアセット。$200000–$BFFFFF を256KB単位でユーザアクセス禁止に設定 |
| 0xEAFFEB | 1B | ✅ 実装済み | RTCテストバイト（書込み保持・読出し） |
| 上記以外の0xEA0000–0xEAFFFF | 64KB弱 | ⚡ バスエラー | 拡張スロット未装着 |
| 0xEB0000–0xEBFFFF    | 64KB    | ✅ 実装済み        | スプライトコントローラ（スプライトスクロールレジスタ+BGコントロール+PCG/BGエリア） |
| 0xEC0000–0xECFFFF    | 64KB    | ⚡ バスエラー       | XB3 拡張ボード領域3（未装着。旧「未使用ミラー」表記は誤りだった） |
| 0xED0000–0xED3FFF    | 16KB    | ✅ 実装済み        | SRAM（実機仕様通り）      |
| 0xED4000–0xEFFFFF    | 176KB   | ⚡ バスエラー       | 未使用（実機も未使用）     |
| 0xF00000–0xF7FFFF    | 512KB   | ✅ CGROM ロード    | CGROM（ANKフォント含む） |
| 0xF80000–0xFBFFFF    | 256KB   | ✅ ROM ロード      | IPLROMミラー |
| 0xFC0000–0xFC1FFF    | 8KB     | ✅ 組み込みROM     | 内蔵SCSI BIOS (SCSI16IN) |
| 0xFC2000–0xFDFFFF    | 120KB   | ✅ 固定値          | SCSI BIOS予約領域 (`0xFF`) |
| 0xFE0000–0xFFFFFF    | 128KB   | ✅ ROM ロード      | IPLROM本体 |

MC68882 FPCP はCPU空間のコプロセッサ接続であり、MMIOアドレスを持ちません。`fpu=off|on|68882` と CIR の関係は [FPCP対応状況](support/fpu_fpcp_support.md) を参照。

## バスエラー（未マップアドレスアクセス、ベクタ2）

mc68000コア側 `mc68k_signal_bus_error()` を
`src/bus/x68k_bus.c` の `cpu_read8`/`cpu_write8`（CPU バスコールバック専用ヘルパー）に配線済み。上表「⚡ バスエラー」の5範囲
（RAM上限–$BFFFFF, $E9E000–$E9FFFF, $EA0000–$EAFFFFの拡張スロット部分［例外あり、下記］, $EC0000–$ECFFFF,
$ED4000–$EFFFFF）にCPU命令がアクセスすると実機同様バスエラー例外が発生します。

- MCP `read_memory`/`write_memory`（`x68k_bus_read8/16/32` 等）は
  CPU 命令実行を経由しない「裏口」アクセスのため、意図的にこの通知をスキップし、
  従来通り `0xFF`（$E9E000–$E9FFFF・DEV領域の拡張スロット部分・$EC0000–$ECFFFF は `0x00`）読み出し/
  書き込み無視のまま動作する（`read8`/`write8` を直接呼ぶ）。
- 実機でチップが存在する範囲（$E90000–$E9DFFF の OPM/PCM/HDC/SCC/PPI/IOI）は、
  個別機能が未実装でも実機ではバスエラーにならないため対象外。未対応レジスタは
  デバイスごとの既定値または保持値を返します。
  $EB0000–$EBFFFF のスプライトコントローラは実装済み。
  IPLROM が起動時に画面モードレジスタ$EB080C等へ実際に書き込むため、この範囲を
  バスエラーにすると起動が壊れることを動作検証で確認済み。
- $FC0000–$FC1FFF は内蔵SCSI BIOS ROM、$FC2000–$FDFFFF は予約ROM領域として
  `0xFF`を返すため、未マップ領域のバスエラー対象外です。
- **$E8A000–$E8BFFF（RTC RP5C15）の偶数アドレスへのバイトアクセスもバスエラーになる**
  （上表「5範囲」とは別枠）。CRTC全体（$E80000–$E8FFFF）は
  マップ済み領域のため上記5範囲には含まれないが、RTCチップ自体はA0線を無視せず
  奇数アドレスのみデコードする実機/XEiJ準拠の挙動。詳細は`docs/ix68k-internals/support/rtc_support.md`参照。
- **$E9E000–$E9FFFF（XB1）・DEV領域内の拡張スロット部分・$EC0000–
  $ECFFFF（XB3）: 拡張ボード領域**。実機/XEiJ（`MemoryMappedDevice.MMD_XB1/
  MMD_XB2/MMD_XB3`の各`mmdRbz()`）ではボード未装着時いずれもバスエラーになる
  （$00EAFA00=MIDIボード(CZ-6BM1)等の固定I/Oアドレスは`XEiJ.java`参照）。ZMUSIC.X常駐
  直後のIPL=7ビジーウェイトデッドロック（バスエラーはIPLマスクの影響を受けない同期
  例外のため、0x00固定のままだと本来即座に発生するはずのバスエラーが起きず、レディ
  ビット待ちが永久に成立しない）調査で判明し、対象に追加。$EC0000–$ECFFFF は従来
  「未使用ミラー」としていたが、根拠（IPLROMが$EB080C等へ書き込む事実）は
  $EB0000–$EBFFFF（スプライトコントローラ）のみに当てはまり誤りでした。$EA0200–$EA020F
  （HOSTDRV）・$EA0100–$EA0117（HOSTFILE、うちの裏口MMIO）・$EAFF81–$EAFF89の奇数
  （X68030エリアセット）・$EAFFEB（RTCテストバイト、書き込み保持・読み返し用）は
  対象外で従来通り。読み出し側のデコードは
  `read8()`が拡張スロット領域全体を`bus_read_expansion8()`へ渡し、その中で
  上記4種に振り分ける（個別ウィンドウの定数で範囲条件を組むと
  `HOSTFILE_BASE < HOSTDRV_RD_BASE`のため空範囲になり、全読み出しが0xFFへ落ちる）。

## CRTC / ビデオコントローラ / スプライトコントローラ

X68000 の画面機構は CRTC・ビデオコントローラ・スプライトコントローラの3チップに
分かれている（*Inside X68000* 3-1節、図13/図14）。「スタブ」ではなく、
3チップとも合成描画に必要な範囲は本格実装済み。
GR/TX/SPの優先順位切替・複数ページも実装済み。実機のタイミング精度
（垂直帰線期間中のみレジスタ更新有効等）は未実装、という区分。

> **必須**: CRTC、ビデオコントローラ、スプライトコントローラのレジスタ・機能を
> 実装・変更したら、それぞれの support 文書を必ず更新します。

レジスタ単位の詳細は、[CRTC](support/crtc_support.md)（R00–R23・走査・スクロール）、
[ビデオコントローラ](support/video_controller_support.md)（R0–R2・パレット・コントラスト）、
[スプライトコントローラ](support/sprite_support.md)（SP/BG・PCG・合成）を参照。

## 実機との差異

| アドレス | 実機 | 我々 | 影響 |
|---|---|---|---|
| $FC0000–$FC1FFF | 内蔵 SCSI BIOS | 組み込み8KB SCSI16IN ROM | 実装済み |
| $FC2000–$FDFFFF | 内蔵 SCSI BIOS予約領域 | `0xFF` | 未使用領域として処理 |
| $F80000–$FBFFFF | CGROM2 下半 | IPLROM ミラー | 漢字フォント後半不可（起動に無関係） |

## 割り込み系統（CPU IPL別）

CPUへの割り込みはpull型です。CPUコアに登録した`x68k_bus_irq_level()`（`src/bus/x68k_bus.c`）が
命令境界ごとに呼ばれ、pendingのレベルを返します（評価順 NMI(7)→1→3→5→6、最初に該当した
レベルを返す。読むだけで副作用なし）。`x68k_cycle_catchup()`（`src/app/runtime.c`）は周辺機器の
時間を進めるだけで、レベル判定は行いません（NMIボタン入力のbus層への橋渡しのみ担当）。
ベクタ返却とpendingのクリアは`x68k_bus.c`の`bus_iack()`が受付時に一括処理します。各ペリフェラルのレジスタ単位の詳細は`docs/ix68k-internals/support/`配下の
該当ファイルを参照。

| IPL | 割り込み源 | ベクタ決定 | 実装状況 |
|---|---|---|---|
| 1 | IOI（優先度: SPC > FDC > FDD > HDC > PRN） | SPC=`SPC_IRQ_VECTOR`固定、FDC/FDD=`ioi_vector \| IOI_FDC/FDD_VECTOR` | ✅ SPC・FDC・FDDのみ実装（`ioi_request`はbit2=FDC/bit1=FDDの2ビットのみ）。❌ HDC・PRNは優先順位のコメントのみで要求ビット自体が存在しない |
| 3 | DMAC ch2（汎用転送COC）/ ch3（ADPCM COC、およびITE有効時のCNT継続BTC）。ch2優先（HD63450はチャネル番号が小さいほど高優先度） | 要求中チャネルのNIVレジスタ値（ch2=`$E840A5`、ch3=`$E840E5`） | ✅ 実装済み（`docs/ix68k-internals/support/dmac_support.md`参照）。ch0(FDC)/ch1(SCSI/HDC)はDMAC自体のCOC割り込みを使わず、FDC/SPCが自身のIRQ1経由で完了通知する設計 |
| 5 | SCC ch A（RS-232C）/ ch B（マウスRx） | `scc_iack_a()` / `scc_iack()` | ✅ 両チャンネル実装済み。ch Aは`X68K_RS232_PTY=1`または`X68K_RS232_DEV=<path>`でホスト経路を有効化 |
| 6 | MFP（優先度降順: H-SYNC(ch15) > ラスタ一致(ch14) > Timer A(ch13) > USART RX(ch12) > … > Timer B(ch8) > V-DISP(ch6) > Timer C(ch5) > Timer D(ch4) > FMIRQ(ch3)） | `mfp_vr \| ch番号` | ✅ H-SYNC・ラスタ一致・Timer A/B/C/D・USART RX・V-DISP・OPM(FMIRQ, GPIP3)実装済み。❌ USART TX系・GPIP0-2系は未実装。VR SビットによるIn-Serviceゲーティングは実装済みだが、既定`-mfp-soft-eoi off`では自動EOI相当（詳細は`docs/ix68k-internals/support/mfp_support.md`） |
| 7 | NMI（`pal_nmi_requested()`、リセットスイッチ相当） | オートベクタ | ✅ 実装済み |
| 2（例外） | バスエラー | `mc68k_signal_bus_error()` | ✅ 実装済み（`cpu_read8`/`cpu_write8`経由のCPU命令アクセスのみ対象） |

## デバッグ時の ROM アドレス変換（参考）

IPLROMXV.DAT（128KB）をファイルオフセットから実アドレスへ変換する場合:

```
68k アドレス = 0xFE0000 + ファイルオフセット
例: ファイルオフセット 0x1066E → アドレス 0xFF066E
```

（`/rom-grep` スキルで事前生成済み逆アセンブルを参照する際に使用）
