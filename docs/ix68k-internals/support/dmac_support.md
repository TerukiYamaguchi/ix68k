# DMAC (HD63450) 実装状況

`src/peripheral/dmac.c`。関連: ch0=FDCは`docs/ix68k-internals/support/fdc_support.md`、ch1=SCSI/HDCは
`docs/ix68k-internals/support/scsi_support.md`、ch3=ADPCMは`docs/ix68k-internals/support/adpcm_support.md`、IOCS `_DMAMOVE`系は
`docs/ix68k-internals/support/iocs_support.md`を参照。

> **必須**: DMAC のチャンネル実装を追加・変更したら、このファイルの該当表を必ず更新する。

---

`src/peripheral/dmac.c`。4チャンネル構成（各チャンネル$40バイト間隔、
『Inside X68000』図3）: ch0=$E84000(FDC)、ch1=$E84040(内蔵SCSI/HDC)、
ch2=$E84080(汎用メモリ間転送)、ch3=$E840C0(ADPCM)。

### レジスタ単位の存在監査（2026-07-29）

『Inside X68000』図3のHD63450全レジスタ（CSR/CER/DCR/OCR/SCR/CCR/MTC/
MAR/DAR/BTC/BAR/NIV/EIV/MFC/CPR/DFC/BFC/GCR）と`dmac_write8`を突き合わせ。

- **CSR/CCR**: 各chで専用ハンドラあり（write-1-to-clear、STARTビット等）
- **OCR/SCR/MTC/MAR/DAR/BAR/BTC**: 専用ハンドラは無いが、CCR STARTハンドラ内で
  `dmac_reg_*`ヘルパー経由で読み取られ転送に使われる（機能的に生きている）
- **NIV/EIV**: レジスタは読み書き可能な生ストレージ。正常なレベル3 IACKでは
  `dmac_iack()`が要求中チャネルの**NIVをライブ参照**し、ch2優先で返す。
  **IPLROMは実際にNIV/EIVへ書き込む**（ch2: `IPLROMXV_addr.S` `ff0e40`/`ff0e46`で
  $68/$69、ch3は下記「ch3: 完了割り込み」参照）。ch2のEIVはエラー完了（ITE=1、
  CER/ERR）の割り込みで使われ、`dmac_iack()`がch2のエラーpendingに対してEIVを返す
  （ch3のエラー割り込みは未実装）。RESET命令時は`dmac_reset()`が
  NIV/EIVレジスタを$0F（未初期化割り込みベクタ番号15相当）へ戻す（下記
  「RESET命令時の初期化」参照）。
- **DCR・MFC・DFC・BFC・CPR・GCR(ch3のみ$E840FF) は`dmac_write8`にもcrtc_regs
  経由の読み取りロジックにも一切登場せず、値の保持のみ**（読み出しても書いた値が
  そのまま返るだけで、転送ロジックに一切影響しない）。PCLピン機能(DCR)・
  CPU機能コード(MFC/DFC/BFC)・チャンネル優先度(CPR)は、ix68kが
  ch2を時間を持つ転送（START後、経過サイクルで完了）として扱い、複数チャンネル間の同時転送を調停しない設計上、複数チャンネル間の実際の
  バス調停や機能コードによるアドレス空間分離が発生しないため、値を無視しても
  今のところ実害は出ていないと推測される（未検証。これらのレジスタ値を
  実際に読み戻して分岐するソフトがあれば影響が出る可能性はある）

### チャンネル別実装状況

| ch | 用途 | 実装状況 |
|---|---|---|
| 0 | FDC ($E94000-) | ✅ CSR write-1-to-clear + CCR START時に武装フラグを立て、FDCコマンド実行完了時に`dmac_ch0_fdc_transfer()`がch2と同じ汎用エンジン(`dmac_transfer_block`)で転送（旧・決め打ちmemcpyショートカットから移行） |
| 1 | 内蔵SCSI(SPC)/HDC(SASI) | ✅ CSR write-1-to-clear + CCR START時にch2と同じ汎用エンジン(`dmac_transfer_block`)でOCR.DIR/SCR.MAC・DACを解釈し転送（旧・決め打ちmemcpyショートカットから移行） |
| 2 | 汎用メモリ間転送 (IOCS `_DMAMOVE`系) | ✅ CSR write-1-to-clear + CCR STARTで転送状態をラッチし、`x68k_bus_dmac_tick()`が10 DMAC clock/byte（10MHz、XEiJ read/write各5 clock由来・未較正の近似）ごとに1 byteをバス経由で進める。単純/アレイ/リンクアレイチェーンを扱い、MTC/MAR/DARは途中状態を公開する。COCは最終byte後、CCR.ITEが有効ならIRQを要求する。OCR.SIZEは未実装でバイト単位。 |
| 3 | ADPCM (MSM6258) | ✅ CSR write-1-to-clear + CCR START時は瞬時転送せず`adpcm_dma_t`へラッチのみ行い、`x68k_bus_adpcm_tick()`がCPUチャンク単位で経過サイクル相当のバイト数だけ`dmac_transfer_block`で漸進転送（当初はフレーム単位、2026-07-25にCPUチャンク粒度へ変更）。完了後にレベル3の正常割り込みを要求し、IACK時にch3 NIVを返す。アレイ/リンクアレイチェーン、`_ADPCMMOD`の停止/一時停止/再開との連動も実装済み。CCR.CNT(コンティニュー動作)・CSR.ACT(チャンネル動作中)もモデル化済み（下記） |

### ch2: OCR/SCR ビットと転送ロジック

`dmac2_start()`（CCRへ`#$88`書き込みで起動）が以下を解釈する
（『Inside X68000』図12/図16、IOCS `_DMAMOVE`系公式仕様
`X68k_IOCS_Data_Crystal.html`と突き合わせ済み）:

- **OCR bit7 (DIR)**: 0=MAR→DAR、1=DAR→MAR。
- **OCR bits3-2 (CHAIN)**: 00=単純転送、10=アレイチェーン、11=リンクアレイ
  チェーン（`_DMAMOVE`/`_DMAMOV_A`/`_DMAMOV_L`がそれぞれ設定）。
- **SCR bits3-2 (MAC)**: MAR(a1相当)の1バイト転送ごとの増減（0固定/1++/2--）。
- **SCR bits1-0 (DAC)**: DAR(a2相当、転送先)の同上。チェーンモードでは
  ブロックをまたいで累積する（転送先を連続領域として埋める用途）。
- アレイ/リンクチェーンのテーブルエントリ形式は
  `X68k_IOCS_Data_Crystal.html`の`_DMAMOV_A`/`_DMAMOV_L`仕様どおり
  （+0=long 転送元アドレス, +4=word 転送バイト数, リンクのみ+6=long
  次テーブルアドレス・0で終端）。

### ch2: 完了割り込み（レベル3・NIV）

IPLROMハンドラ（`$FFAC8A`/`$FFAD16`/`$FFAD66`）は転送起動後**即座にrts**で
戻り、ROM側ワーク`$C34`（DMA実行中フラグ）は**完了割り込みでのみ**クリア
される（ポーリングしない設計）。よって`_DMAMOVE`系を連続呼び出しした場合、
2回目以降は前回分の割り込みが実際に配信されるまで冒頭の`tst.b($C34)`
busy-waitでブロックする。

- ベクタ番号`$68`はIPLROM起動時のベクタテーブル直接書き込み
  （`IPLROMXV_addr.S` `ff0e4c`/`ff0e56`: vector$68→`$FF11A6`(CSRクリアのみ
  =正常完了用)、vector$69→`$FF1194`(CER退避付き=エラー用)）から特定した。
  DMAC自体のNIV/EIVレジスタ（$E840A5/A7）にもIPLROMは実際に$68/$69を書き込む
  （`IPLROMXV_addr.S` `ff0e40`/`ff0e46`）。`dmac_iack()`はch2 pending時にこの
  NIVの現在値を返す。
- `src/peripheral/dmac.c`の`dmac_iack()`、`src/bus/x68k_bus.c`の`bus_iack()`
  （レベル3ハンドリング）、`x68k_bus_irq_level()`（`src/bus/x68k_bus.c`）のIRQレベル判定で配線。
- ix68k上での検証: `tools-x68k/asmtest/dmamove_test.s`（単純転送・2回連続呼び出し
  ＝割り込み配線の証明・アレイチェーン2エントリ、いずれも成功。
  2026-07-14。ch2を時間を持つ転送にした後の2026-10-27にも68000構成で再実行し、
  同じ結果と`_DMAMODE`=0を確認）。

### ch3: フレームペースドDMAエンジン

ch0/ch1の`dmac_transfer_block()`は「CCR START時に全バイト瞬時転送」する同期
モデル（ch2はデータ移動のみ同関数を使い、完了通知を時間経過で遅らせる）だが、ADPCM(MSM6258)再生・録音はサンプリング周波数(3.9k-15.6kHz)に
同期した実時間転送が前提（`_ADPCMOUT`等のウェイトモード仕様がその根拠）。
このため ch3 だけ他chと異なる設計にしている:

- **CCR START(`$E840C7`へ`#$80`)時**: OCR/SCR/MTC/MAR/DARを`b->adpcm_dma`
  （`bus_internal.h`）へラッチするのみで転送しない。OCR CHAIN(bits3-2)!=0
  （`_ADPCMAOT`/`AIN`/`LOT`/`LIN`が使うアレイ/リンクアレイチェーン）は
  BAR/BTCをラッチし、最初のエントリは`x68k_bus_adpcm_tick()`側でロードする。
- **`x68k_bus_adpcm_tick()`（CPUチャンク単位、`src/app/runtime.c`のcatch-up処理から
  `x68k_bus_mfp_tick()`/`x68k_bus_opm_tick()`と同じ`MFP_TICK_CYCLES`粒度で
  呼ぶ。当初のフレーム単位呼び出しから2026-07-25にチャンク粒度化）: 1バイト=2サンプルのため経過CPUサイクル
  数に`adpcm_get_sample_rate_hz()`で算出した`Fs`を掛け`2*CPU_HZ`(=1バイト
  あたりのCPUサイクル数の逆数)で割った本数だけ`dmac_transfer_block`で漸進
  転送し、MAR/DAR/MTCレジスタへ都度書き戻す（chain==0のみ。理由は下記
  チェーンの項参照）。残数0でCSR.COC を立て`dmac3_irq_pending`で通知する。
  フレーム先頭で1回だけ呼ぶ旧方式では、音楽ドライバが行う「CCR SABで一時
  停止→数千サイクル後にCCR STARTで再武装」の窓がフレーム境界を跨ぐと
  そのフレームが丸ごと0バイト転送になり、実機の0.4ms程度のギャップが
  16.7msの音切れへ拡大していた（`taken=0`の実ギャップ）。
- **MARは ch0/ch1と同じ`dmac_reg_addr24`（24bit）**。ch2のような4バイト
  読みではない（ch3はch1[SCSI/HDC]同様「デバイスchでMAR=RAM上バッファ」の
  ため。Opus設計コメントで確定）。DARは全ch共通の`dmac_reg_l`（4バイト、
  実際は`$E92003`固定でDAC=0のため不変）。

### ch3: アレイ/リンクアレイチェーン + _ADPCMMOD（2026-07-16）

`_ADPCMAOT`/`AIN`（アレイチェーン、OCR CHAIN=2）・`_ADPCMLOT`/`LIN`
（リンクアレイチェーン、CHAIN=3）は、ch2の`dmac2_start()`と同じテーブル
形式（+0=long転送元アドレス, +4=word転送バイト数、リンクのみ+6=long次
テーブルアドレス・0で終端）をCPUチャンクまたぎで漸進的に走査する。ch2は
`for(;;)`の同期ループで全エントリのデータを即座に移動する（完了通知は時間経過後）が、ch3は実時間転送が
前提のため、`adpcm_dma_t`に`chain`/`bar`/`btc`/`chain_done`を追加し、
`x68k_bus_adpcm_tick()`の冒頭で「現在のエントリを使い切った(remaining==0)
かつチェーンモード」を検知するたびに次エントリを1つロードする状態機械
にした（`adpcm_dma_load_next_entry()`）。

- アレイ(chain==2)は`btc`（残エントリ数）が0になった時点で完了。
  リンク(chain==3)は読み込んだエントリの次テーブルアドレスが0だった
  場合に`chain_done`を立て、そのエントリの転送完了後に完了処理へ回す
  （ch2のロジックと同じ「読んでから使い切る」順序）。
- chain!=0はMAR/BAR/BTCがエントリごとに切り替わり、かつROM側もこれらを
  ポーリングしない設計（ch2同様、完了は割り込みでのみ通知）のため、
  chain==0の単純転送と異なりMAR/MTCレジスタへの書き戻しは行わない。

`_ADPCMMOD`(IOCS $67, d1.l=0:終了/1:中断/2:再開)はCCRへ個別ビットを書く
（Opus設計: $10=SAB停止/$20=HLT一時停止/$08=INT enableのみ=再開）。
`adpcm_dma_t.paused`を追加し、`x68k_bus_adpcm_tick()`の先頭で
`!active || paused`なら即returnして転送を止める。SAB(停止)は`active=0`
まで落とす中断のため、完了割り込みは発生させない（明示的な中断でROM側の
ウェイトループを誤って解いてしまう方が実害が大きいと判断、実機挙動は
未確認の仮定）。

動作検証: 3エントリ計15000バイトの`_ADPCMAOT`/`_ADPCMLOT`が単一バッファ
15600バイト時(当時の実測、約10.7秒/39000バイト換算で約4.1秒相当)と近い
約4.5秒/約5.5秒で完走し、全エントリが走査されたことをタイミングから確認
（1エントリ分5000バイトだけなら約1.4秒で完了するはずのため明確に区別
できる）。`_ADPCMMOD`はHLT一時停止(約5.4秒)を挟んだ`_ADPCMOUT`が通常時
約10.7秒に対し約15.0秒（一時停止時間相当の延長）で完走し、一時停止/再開
が機能していることを確認。ビルド警告ゼロ・`ctest`全通過
（SDL3版・ヘッドレス版とも）、FDD通常起動含め回帰なし。

### ch3: 完了割り込み（レベル3・NIV）

ROM初期化ルーチン（`IPLROMXV.S:12312-12323`）がNIV/EIVレジスタへ固定値
`$6A`/`$6B`を書き込み、ベクタテーブル`$1A8`/`$1AC`へそれぞれ
`$FF1156`(CSRクリアのみ=正常完了用)/`$FF113C`(CER退避付き=エラー用)を配線
する。`_ADPCMOUT`等はデータ長を`$FF00`バイト単位に分割し、各チャンクで
ROM側ワーク`$C32`（実行中フラグ）が0になるまでスピンする。`$C32`は完了
割り込み（vector$6A）でのみクリアされるため、ch2同様、実際にレベル3割り込み
を発生させないと呼び出しが無限ビジーウェイトする。

- ch2/ch3が同時pending時はch2を優先（HD63450はチャネル番号が小さいほど
  優先度が高い）。`dmac_iack()`はch3 pending時にch3 NIVの現在値を返す。
  ch2のエラー割り込みはEIV(`$69`)を返す（エラーpendingを正常完了より先に返す）。
  ch3のEIV(`$6B`)側はモデル化しない（ch3は転送エラー自体を生成しないため）。
- `src/peripheral/dmac.c`の`dmac_iack()`、`src/bus/x68k_bus.c`の`bus_iack()`
  （レベル3ハンドリング）、`x68k_bus_irq_level()`（`src/bus/x68k_bus.c`）のIRQレベル判定で配線。
- 動作検証: 使い捨てアセンブリテストで`_ADPCMOUT`(ウェイトモード、39000
  バイト@3.9kHz)を実行し、`run_command`の所要時間が約10.7秒（瞬時完了で
  はなくフレームペーシング相当）であることを確認。$C32が正常にクリア
  され`A>`へ復帰、FDD通常起動・SCSI-HD起動とも回帰なしを確認（2026-07-15）。

### ch3: CNTコンティニュー動作とCSR.ACT（2026-08-20）

`PCM8A.X`等のADPCM連続再生ドライバは、CCR.CNT(bit6)によるダブルバッファ再生を
使う。MTCが0になった時点でCNTが立っていれば、HD63450は転送を終了せずに次の動作を
行う（XEiJ `HD63450.java` `dmaTransfer()`のコンティニューモード分岐に準拠）:

1. CSR.BTC(bit6, Block Transfer Complete)を立てる
   — レジスタのBTC(+$1A, Base Transfer Counter)とは名前が同じだけの別物
2. CCR.CNTを自動クリアする（次ブロックの継続はソフトウェアが再セットして指示する）
3. CCR.ITE(bit3)が有効なら正常割り込み（IACK時にch3 NIV）を要求する
4. BAR→MAR / BTC→MTC を取り込み、そのまま転送を継続する

これを実装するまでは、単純転送はMTC=0で常にCSR.COC(bit7)を立てて停止していた。

あわせて以下の2点が同時に必要になる。`PCM8A.X`の割り込みハンドラは3つとも
参照しており、どれか1つでも欠けると実PCMデータが一度も再生されない。

- **CSR.ACT(bit3)のモデル化**: CCR STARTで立て、転送終了・SABで落とす
  「チャンネル動作中」のライブステータス。`PCM8A.X`はハンドラ内で
  `BTST #3,(A5)` により、動作中ならBAR/BTCを差し替えてCNTを再セットするだけの
  継続経路へ、停止していればSAB→MAR/MTC再設定→STARTのフルリスタート経路へ
  分岐する。後者ではMARに無音バッファを設定するため、ACTを立てないと
  実PCMデータを積んだBAR側のバッファへ永久に到達しない。
- **CSR write-1-to-clearの対象外ビット**: ACT(bit3)は転送の進行状態そのもの、
  PCS(bit0)は~PCL端子の現在値であり、どちらも書き込みでは変化しない。
  `PCM8A.X`はハンドラ冒頭で`ST (A5)`（=#$FF書き込み）によりCSRをクリアしてから
  ACTを読むため、#$FFでACTまで落としてしまうと上記の判定が常に「停止」側に倒れる。
- **CCR.CNTの読み返し**: `PCM8A.X`はCCRへCNTを書いた直後に`BTST #6,7(A5)`で
  読み返し、立っていなければフルリスタート経路へ落ちる。よってCCRのbit6は
  書いた値をそのまま保持するのではなく、実際に武装できたかどうかを返す
  （チェーンモードでのセット＝コンフィギュレーションエラー、動作中でも
  START同時書き込みでもないときのセット＝タイミングエラーはいずれも武装しない）。

CNTは単純転送(chain==0)専用で、アレイ/リンクアレイチェーンとは併用しない。

動作検証: `PCM8A.X` v1.02 + `Z-MUSIC` ver2.08e 常駐環境で
`ZP F:\MUSIC\G9.ZMS`（Z-music ver2 + PCM8）を演奏し、DMAC ch3のレジスタが
CSR=`$08`(ACT のみ)・CCR=`$48`(CNT+ITE)で定常し、MARが実PCMデータを積んだ
2つのバッファを交互に走査することを確認（修正前はMARが無音バッファに固定され、
ブロックごとにSAB→フルリスタートを繰り返していた）。

### ch0: 汎用エンジンへの統合（2026-07-14）

`dmac_write8`のch0 CCR($E84007)ハンドラは、ch1/ch2と同じ`dmac_transfer_block`
を使うよう統合済み。ただし**ch1/ch2とはトリガー順序が逆**という構造的な
相違があり、それに合わせた設計になっている。

- **CCR STARTがFDCコマンドより先に発行される**（ch1のCCR STARTはSCSI
  コマンド発行後に来るのと対照的）。事前の計装で、何百セクタも
  例外なくこの順序であることを確認した。CCR START時点ではFDCがまだ
  cyl/head/secを受け取っておらず、どのセクタを転送するか未確定なため、
  CCR STARTハンドラ自体では転送できない。
- そこでCCR STARTハンドラは`b->dmac_ch0_armed`という武装フラグを立てる
  だけにし、実転送は`fdc_exec()`がREAD/WRITE DATAコマンド実行完了時に
  `dmac_ch0_fdc_transfer()`を呼んで行う設計にした（トリガーの後ろ倒し）。
  参照実装XEiJ（`HD63450.java`の`dmaStart`/`dmaFallPCL`、`FDC.java`の
  `fduWriteDataEPhase`の`fdcIndex`/`fdcLimit`）でも同型の設計であることを
  ソース照合で確認済み（FDCのREQGはEXTERNAL_REQUESTで、CCR STARTは armed
  状態にするだけで実転送しない）。
- FDC側には実行データフェーズ(`FDC_EXEC`)を新設。`fdc_t`の
  `dma_ptr`/`dma_remaining`/`dma_dir`が現在の転送位置・残バイト数・方向を
  保持し、`$E94003`のread8/write8（`fdc_read_data`/`fdc_write_data`）が
  このフェーズ中は1バイトずつ`disk[drv]`バッファとやり取りする
  （ch1のSPC DREGパスと同型）。残0で`fdc_exec_complete()`が呼ばれ、
  WRITE時のwrite-back・結果フェーズ(`FDC_RESULT`)遷移・IRQ発行を行う。
- 非武装時（`dmac_ch0_armed`が立っていない異常系、計装ではゼロ例外）
  は`fdc_drain_exec_phase()`が内部で直接ドレインするフォールバックに
  委ねる（stale registerによる誤転送を防ぐガード）。
- 動作検証: FDDコールドブート、`DIR`、単一/複数ファイルの`COPY`で
  READ/WRITE DATAコマンド計391回、すべてDMAC武装経路を通過（フォール
  バック0件）。書き込み後のSHA-256完全一致。
- **OCRの共有パターン**（実測）: OCR=`$32`(DIR=0、ホスト→FDC)は
  WRITE DATA・SCAN EQUAL・FORMAT TRACK(ID部データ送出)が共有し、OCR=`$B2`
  (DIR=1、FDC→ホスト)はREAD系が使う。DAR=`$E94003`固定・SCR=`$04`(MAR++/
  DAR固定)は`ioi_init`($FF0F16/$FF0FC0)がFDC/HDC双方のDAR初期値とともに
  ブート時に一括設定する。
- **CCR SAB(bit4、ソフトウェアアボート)対応**: ROMはFDCが
  Not Ready等で実行データフェーズに入らずCOCが立たなかったとき、CCRへ
  `$10`を書いて転送要求を取り消す。従来はCCR STARTのbit7しか見ておらず
  `ch0_armed`が立ちっぱなしのまま残る問題があったため、bit4処理を追加した
  （READ/WRITE DATAのNot Ready経路にも同じ効果が及ぶが、次コマンドが必ず
  自前でCCR STARTを書き直すため実害は無い）。

### RESET命令時の初期化（2026-09-09）

回路図（『Outside X68000』の回路図 p5）でIC23 HD63450-10の27番ピン
(BEC2)がネット`RESET`に直結していることを確認済み。従来`bus_reset_peripheral()`
はch3(ADPCM)の`active`/`paused`/`cnt`とCSR/CCRしか落としておらず、ch0-ch2の
全レジスタ・ch0(FDC)武装フラグ・ch3の転送途中状態がソフトリセット（RESET命令）を
またいで残留していた。参照実装XEiJの`HD63450.dmaReset()`（RESET命令ハンドラ
`irpReset()`から呼ばれる）が4ch全レジスタをクリアしていることを確認し、同型の
`dmac_reset()`を実装した。

- レジスタ実体`b->crtc_regs[$4000-$40FF]`（ch0=$4000/ch1=$4040/ch2=$4080/
  ch3=$40C0、各$40バイト、GCRはch3ブロック末尾の$40FF）をchごとに一括ゼロ
  クリアし、NIV/EIV（ch_base+$25/+$27）だけ実機のRESET後の値`$0F`（未初期化
  割り込みベクタ番号15相当、XEiJ `HD63450.java`のコメント準拠）へ書き戻す。
  この256バイト範囲は`dmac.c`/`fdc.c`（ch0 MAR参照）以外から使われないため、
  CRTC/パレット等の他レジスタを巻き込まない。
- `dmac_t`/`adpcm_dma_t`はホスト側設定・外部ピン相当の状態を持たないため、
  `b->dmac`を構造体ごとゼロクリアする（`ch0_armed`武装フラグ、`ch2_irq_pending`/
  `ch3_irq_pending`、ch3の`dir`/`mac`/`dac`/`mar`/`dar`/`remaining`/`chain`/
  `bar`/`btc`/`chain_done`/`budget_acc`を含む）。
- ch0(FDC)の実行データフェーズ（`fdc_t`の`dma_ptr`/`dma_remaining`/`dma_dir`等）、
  ch1が使うSPC/HDCの転送状態は`dmac_reset()`の対象外（各デバイス側の責務。
  SPC/HDCは`scsi_reset()`/`sasi_reset()`が別途担当、いずれも
  `bus_reset_peripheral()`内で近接して呼ばれる）。FDC側には対応するリセット
  関数が現状無く、別課題として検討する。
- 旧`dmac_ch3_reset()`（ch3のみ対象）は`dmac_reset()`へ統合し廃止。
- `x68k_bus_create()`（電源投入相当）からも`dmac_reset()`を呼び、電源投入時に
  NIV/EIVが`$0F`になる実機挙動と一致させた（`calloc`だけでは0のまま）。
- 実装前にXEiJ側で`irpReset()`→`HD63450.dmaReset()`の全リセット対象を確認した。

### ch1: 汎用エンジンへの統合（2026-07-14）

`dmac_write8`のch1 CCR($E84047)ハンドラは、ch2と同じ`dmac_transfer_block`を
使うよう統合済み。READ(device→memory)/WRITE(memory→device)とも同一ロジック
で処理し、OCR.DIRだけで方向が決まる（HDC/SASIはWRITEコマンド自体が未実装
のためWRITE経路はSPCのみ）。

- ROM実機採取値（一時計装で採取）: READ=OCR `$B1`(SPC)/`$B2`(HDC)、
  WRITE=OCR `$31`。いずれもSCR=`$04`(MAC=1/DAC=0)、CHAIN=00で共通、DIRのみ
  異なる。
- 実データ移動はDMACが`$E96035`(SPC DREG)/`$E96001`(HDC DATA)を
  `x68k_bus_read8/write8`でバイト単位アクセスすることで行う。
  `scsi_read8/write8`のDREGパス・`sasi_read8`のDATAパスが
  1バイトアクセスごとに`buf_index`/`tc`を自前で前進させ、完了条件
  （`tc==0`または`buf_index==buf_limit`）も自動的に成立する
  ため、CPU直叩き時と全く同じ経路をDMACも通る（実チップにより忠実）。
- 旧・一括ドレイン方式のAPI（`scsi_dma_ptr/advance`、
  `scsi_dma_write_ptr/advance`、`sasi_dma_ptr/advance`）は削除済み。
  「転送可能か」（`*_dma_active`系）と「残りバイト数」（`*_dma_remaining`系、
  MTCとのmin()でDMAC側が転送上限を決めるために使う）のみ提供する。
- 動作検証: `HOSTFILE.X`経由でSCSI-HD(`A:`)へ30MBファイルを書き込み、100%
  完走・SHA-256完全一致（退行なし）。SCSI-HD起動・FDD通常
  起動とも回帰なし。


## 未実装・要検証

ch2（メモリ間転送）の時間モデルは、次の値が実機で較正されていない近似である。XEiJ の値や
IPLROM の速度表示（ix68k 060 で 125%）を実機値として扱わないこと。

- 1 転送単位（byte/word）あたり 10 DMAC clock（read 5 + write 5、XEiJ 由来）。
- DMAC クロック（68000/68010/68020 は 10MHz、68030 以上は 12.5MHz。CPU モデルで切替え、
  実機の機種差は未確認）。
- チェーン表の読出し 5 DMAC clock/byte（array 6 byte、link 10 byte）。
- 実機較正が必要な測定: 転送長・偶奇・SIZE/DPS・チェーン別の所要時間、IPLROM 速度表示との照合。

ch2 の未実装:

- OCR.SIZE=long/pack、DCR.DPS=8-bit ポート（`CER=0x01` で拒否。データ試験と実機測定が
  そろうまで受理しない）。
- OCR の REQG/BTD の検査（現状は受理）。
- DMA 側のバスエラー（`CER=0x09` 系）、CCR.CNT・HLT。
- cycle source が使えない経路（68060 以外、debug_run）の START 時刻同期（チャンク末尾でまとめて進める）。
- 同一 CPU アクセス外の CPU 経路（MCP 等）でのロック書込み破棄範囲の厳密化。

## 変更履歴

| 日付 | 内容 |
|------|------|
| 2026-09-03 | ch0 CCRのSAB(bit4)処理を追加し`ch0_armed`を解除するよう修正。IOCS `_B_VERIFY`(SCAN EQUAL)実装のレビューで、Not Ready等の異常系で武装フラグが立ちっぱなしのまま残る問題を発見。OCR共有パターン(WRITE DATA/SCAN EQUAL/FORMAT TRACKがOCR=`$32`共有)も追記 |
| 2026-07-14 | DMAC (HD63450) セクションを追加。ch2汎用メモリ間転送（IOCS `_DMAMOVE`系0x8A-0x8CのROM委譲に伴う実装）とレベル3完了割り込みの配線を記載 |
| 2026-07-14 | ch1(SCSI/HDC)をch2と同じ汎用エンジンへ統合。決め打ちmemcpyショートカット・一括ドレインAPIを撤去し記載を更新 |
| 2026-07-14 | ch0(FDC)をch1/ch2と同じ汎用エンジンへ統合。CCR STARTがFDCコマンドより先に発行される構造的相違に対応した武装フラグ方式・実行データフェーズ(FDC_EXEC)新設を記載 |
| 2026-07-15 | ch3(ADPCM)にフレームペースドDMAエンジンを実装。ch0-2の瞬時転送モデルと異なり、CCR STARTではラッチのみ行い`x68k_bus_adpcm_tick()`が毎フレームサンプリング周波数相当のバイト数だけ漸進転送する設計・レベル3ベクタ$6A割り込み配線を記載。アレイ/リンクアレイチェーンはこの時点では未対応（翌2026-07-16に実装）の既知の制約として明記 |
| 2026-07-16 | アレイ/リンクアレイチェーン(_ADPCMAOT/AIN/LOT/LIN)と`_ADPCMMOD`(一時停止/再開)を実装。`adpcm_dma_t`にchain/bar/btc/chain_done/pausedを追加し、`x68k_bus_adpcm_tick()`をエントリ単位のフレームまたぎ状態機械に拡張。ix68kで3エントリチェーンの完走時間・一時停止による所要時間延長を確認 |
| 2026-07-25 | `x68k_bus_adpcm_tick()`をフレーム単位(60Hz)からCPUチャンク単位(`MFP_TICK_CYCLES`粒度)呼び出しへ変更（「taken=0の実ギャップ」修正）。詳細は`docs/ix68k-internals/support/adpcm_support.md`参照 |
| 2026-07-29 | `docs/hw_support.md`から本ファイルへ分離（ペリフェラル単位でのファイル分割） |
| 2026-08-20 | ch3にCCR.CNT(コンティニュー動作)・CSR.ACT(チャンネル動作中)・CSR write-1-to-clear対象ビットの限定・CCR.CNTの読み返しを実装。`PCM8A.X`のダブルバッファ再生でADPCMパートが無音になっていた不具合を修正（上記「ch3: CNTコンティニュー動作とCSR.ACT」参照） |
| 2026-09-09 | RESET命令でDMAC(HD63450)がリセットされていなかった不具合を修正。`dmac_ch3_reset()`(ch3のみ対象)を4ch全レジスタ・全内部状態をクリアする`dmac_reset()`へ置き換え、`x68k_bus_create()`（電源投入相当）からも呼ぶよう変更。あわせてNIV/EIV($E840A5/A7等)へのIPLROM書き込みに関する誤記載（「ROMは一切書き込まない」）を訂正 |
| 2026-09-12 | DMAC ch2/ch3 のレベル3 IACKを固定ベクタから要求中チャネルのNIVライブ参照へ変更。`dmac_iack()`へch2優先・pending消費を集約。EIVはDMACエラー割り込み未実装のため保持のみ |
| 2026-10-27 | ch2 のメモリ間転送を時間を持つ転送へ変更（START でデータ移動、転送バイト数×10 DMAC clock(10MHz) 後に COC と割り込み）。IPLROM 1.6 の転送速度測定が即時完了で 0 割りになるため |
| 2026-10-29 | ch2を進行型転送へ拡張: CPU bus callbackごとの時刻同期(060turboのみ)、OCR.SIZE=word、DMACクロックのCPUモデル切替、チェーン表読出し時間と循環リンクの安全停止、active中のレジスタ書込み検査(CER=0x02)、未対応構成のCER=0x01拒否、エラー完了のEIV割り込み。 |
