# FDC (µPD72065B) 実装状況

`src/peripheral/fdc.c`。関連: MFP は `docs/ix68k-internals/support/mfp_support.md`、DMAC ch0 は
`docs/ix68k-internals/support/dmac_support.md`、IOCS は `docs/ix68k-internals/support/iocs_support.md` を参照。

> **必須**: FDC のレジスタ・コマンド実装を追加・変更したら、この
> ファイルの該当表を必ず更新します。

---

µPD72065B は D0-D7（ローバイト）接続、チップ内部 A0 = バス A1 のため
**奇数アドレスのみ**が実アクセスに対応する（$E94000–$E95FFF にマップ、
偶数バイトは無視）。

### レジスタ

| アドレス | 名称 | R/W | 実装状況 |
|---|---|---|---|
| $E94001 | Main Status Register (MSR) | R | ✅ IDLE=0x80 / CMD=0x90 / RESULT=0xD0 の3フェーズを返す（`fdc_msr`） |
| $E94001 / $E94003 | Data Register | W | ✅ コマンドバイト列の受付（`fdc_write_data`） |
| $E94003 | Data Register | R | ✅ リザルトバイト列の読み出し（`fdc_read_data`） |
| $E94005 | Drive Status | R | ✅ **2026-07-08訂正**: 直前に同レジスタ($E94005)へ書き込んだドライブ選択ビット(bit0-3、複数ならOR)に対応するドライブのDISK IN(bit7)を返す自己参照レジスタ（『Inside X68000』図4/3-1節）。旧実装は無関係な$E94007(`fdc_drive_sel`)に依存しており、ROMの`_B_DRVCHK`（$E94005へドライブビットを書いて直後に読み戻す実装）が常に同じ結果を返し続けるバグの原因だった |
| $E94005 | Drive Control（イジェクト） | W | ✅ EJECT ON/OFF（bit5）+ 対象ドライブ（bit3-0）の fall edge 検知でイジェクト実行（『Inside X68000』の$E94005解説準拠） |
| $E94007 | Drive Select | R | ❌ 常に 0xFF 固定（未実装） |
| $E94007 | Drive Select | W | ✅ `fdc_drive_sel` に保持。bit4（2DD/2D=250kbps）は `fdc.density_2dd` として FDC の転送速度に反映し、媒体プロファイルと不一致なら READ ID は MA、READ/WRITE/SCAN/FORMAT TRACK は ND（回転数は非対応） |
| DMA MAR ($E8400D–$E8400F) | READ/WRITE DATA 転送先/元アドレス | R/W | ✅ `crtc_regs[0x400D-F]`（ビッグエンディアン24bit）経由で反映。通常は`dmac_ch0_fdc_transfer()`（下記DMACセクション参照）が読み書きします。CCR STARTで武装されていない異常系のみ`fdc_exec`が直接参照するフォールバック経路が働く |

### コマンド

| コマンド | コード | 実装状況 |
|---|---|---|
| SPECIFY | 0x03 | ✅ 受理のみ（SRT/HUT/HLT/NDM タイミング値は無視） |
| RECALIBRATE | 0x07 | ✅ シリンダ0復帰、IRQ発行 |
| SENSE INTERRUPT STATUS | 0x08 | ✅ ST0/PCN 返却（pending 有無で1/2バイト切替） |
| SENSE DRIVE STATUS | 0x04 | ✅ **2026-07-08訂正**: RDY/TR0に加えTS(bit3、内蔵ドライブは常に2-side)・HD(bit2、コマンドフェーズで指定された値をそのまま返却)を追加。旧実装はTS/HDとも常に0で『Inside X68000』図33/図34と不一致だった（FDC直叩きソフト向けの精度改善）。**ST3 bit6(WP)も追加**（下記「FDDライトプロテクト」節参照）。~~IOCS `_B_DRVSNS`は別経路のCスタブでこの層を経由しないため無関係~~ → **陳腐化（2026-07-18に`0x44`をROM委譲へ切替済み、`src/iocs.c` case 0x44）。ROMハンドラ(`$FF8D18`)はこの層が返すST3の生バイトをそのままD0上位バイトへ載せるだけで、ST3を自前で合成しない** |
| SEEK | 0x0F | ✅ **2026-07-08訂正**: ST0のHDビット(bit2)を撤去。『Inside X68000』図30ではSEEKのC-PHASE第2バイトはUS1/US0のみでHDフィールドが存在せず、旧実装は誤ってbit2をHDとして抽出しST0に混ぜていた（RECALIBRATEも同様にHDなし、対応済み） |
| READ ID | 0x0A | ✅ 固定値応答（sector=1, N=3 決め打ち） |
| READ DATA | 0x06 | ✅ XDF から DMAC ch0 汎用エンジン経由で転送。未挿入/範囲外はエラー応答（ST0/ST1 該当ビット）。DDAM(削除データ)ビットが立っているセクタを読むとST2にCM(削除データ不一致)を立てる（下記参照） |
| WRITE DATA | 0x05 | ✅ DMAC ch0 汎用エンジン経由で XDF イメージへ書き込み、`disk_path` があれば `fopen("r+b")` でファイルへも fwrite バック（イジェクト対応の検証時に実装済み、後にDMAC ch0汎用エンジン経由に統合）。書き込み時にDDAMビットをクリアする（通常データへ復帰）。**追記**: ライトプロテクト中はNot Readyの直後・N>7等のジオメトリ検証より前でST0=AT+ST1 bit1(NW)を返し、DMA/DDAM更新とも一切行わない（下記「FDDライトプロテクト」節参照） |
| READ TRACK（Inside X68000は「READ DIAGNOSTIC」表記） | 0x02 | ❌ 未実装（byte-count用の最初のswitchには0x02があるが、実行用switch(`fdc.c:133`)にcaseが無く`default:`分岐でInvalid Command応答） |
| WRITE DELETED DATA | 0x09 | ✅ **実装済み**。IOCS `_B_WRITED`(fn=0x49)用。`case 0x05`と合流させて実装（`fdc.c`）。DDAMビットマップ(`fdc_t.ddam[4][154]`)へセクタ単位でセットします。下記「IOCS `_B_WRITED`/`_B_READDL`の ROM 委譲」節参照 |
| READ DELETED DATA | 0x0C | ✅ **実装済み**。IOCS `_B_READDL`(fn=0x4C)用。`case 0x06`と合流させて実装（`fdc.c`）。DDAMビットが立っていない(通常データ)セクタを読むとST2にCMを立てます。下記参照 |
| FORMAT TRACK（Inside X68000は「WRITE ID」表記） | 0x0D | ✅ 実装済み。IOCS `_B_FORMAT`(0x4D)は標準2HDも含め実機ROMへ委譲し、ROMがこのコマンドを発行します。IDタプル列を媒体プロファイルと照合してフィル・DDAMクリア・書戻しを行います。NR/NW/NDの結果ステータスはFDC側で構築 |
| SCAN EQUAL | 0x11 | ✅ **実装済み**。IOCS `_B_VERIFY`(fn=0x41)用。ホスト→FDC方向(WRITE DATAと同一OCR.DIR=0)でDMAC ch0から送られる目的データを、`disk[drv]`の記録データと1バイトずつ比較する（`dma_cmp`フラグ、`fdc.c`）。目的データ`$FF`はドントケア(µPD765仕様、非対称)。1コマンド=1セクタ(EOT=RをROM実測($FF8B7E)で確認、マルチセクタ化はROM側`dbra`ループが担当)。ディスクへの書き込みは発生しません。**DDAM検出時のCMは立てない**（ROMの`_B_VERIFY`はCMを見ないため機能差なし。既知の非対応として記載） |
| SCAN LOW OR EQUAL / SCAN HIGH OR EQUAL | 0x19 / 0x1D | ❌ 実行未実装。コマンド長(9バイト)のみ`fdc_cmd_len_tab()`に登録済みで、コマンドフェーズを完走した上でInvalid Command応答へ落ちる（ROMが発行する経路が無く検証不能なため実行コードは見送り） |
| SET STANDBY（µPD72065B拡張コマンド） | 0x35 | ❌ **監査で新規発見**。同上、未実装・未記載でした。FDC内部クロック停止によるスタンバイ移行で、AC電源のX68000では実用上ほぼ意味を持たないコマンドとInside X68000本文に明記あり |
| RESET STANDBY（同上） | 0x34 | ❌ 同上 |
| SOFTWARE RESET（同上） | 0x36 | ❌ 同上 |

### ドライブ番号対応（`fdc_unit_to_drive`）

**2026-07-08訂正**: 恒等対応（`unit == drive`、スクランブル無し）。
ROM `$FF93FC`（`unit = PDA & 3`）と『Inside X68000』（$E94005/$E94007
とも「内蔵ドライブは0と1」と明記）の双方で確認。旧表（下記、内蔵FD0=unit2
等の逆マッピング）は根拠となる一次資料が無く誤りでした。旧表のままROM委譲
すると起動時のブートプローブが内蔵FD0を「未接続」と誤認し起動不能になります。

| FDC unit (cmd[1] & 3) | 内部 drive index | 対応 |
|---|---|---|
| 0 | 0 | 内蔵 FD0 |
| 1 | 1 | 内蔵 FD1 |
| 2 | 2 | 外付け FD2 |
| 3 | 3 | 外付け FD3 |

### XDF セクタオフセット計算

```
offset = ((cyl*2 + head)*8 + (sec-1)) * ssize   // ssize = 128 << N（N=3 → 1024 bytes）
```

### 対応ディスクイメージ形式

`x68k_bus_load_fdc()`（`src/bus/x68k_bus.c`）はマジックナンバー等の内容判定は行わず、
拡張子のみで媒体プロファイルを決める（`fdc_profile_for_path()`、大小文字は区別しない。
サイズからの推定はしない）。イメージはいずれもヘッダなしの生セクタダンプで、`D88`等の
ヘッダ付き形式を含む未知の拡張子は読み込み時にエラーを返し拒否する。対応拡張子:
`.xdf` / `.hdm` / `.2hd`（標準2HD、1,261,568バイト、実体は同一形式。上記オフセット計算は
この形式の式）、`.2hs` / `.2hde` / `.2ht` / `.2hdez`（9SCSET系）、`.2hc` / `.2dd9` / `.2dd8` /
`.2hq`（512byte/セクタ系）。各媒体のジオメトリとID配置は
`src/peripheral/fdc.c` の `fdc_profile_*` と [fdc_design.md](design/fdc_design.md) を参照。
`tools-host/hu68disk`側は`image_load()`でフロッピー判定時のみ標準2HDの3拡張子
（`.xdf`/`.hdm`/`.2hd`）に限って拡張子チェックを行う（`fd_ext_ok()`、SCSI HDDは内容シグネチャ
判定のため対象外）。
`.hdf`/`.hdi`等のSCSI HDDイメージは別ジオメトリのため両ツールとも対象外
（`insert_scsi_disk`側）。動作確認: `.hdm`（`disk/Dennou Ongaku Classic 1.hdm`、
2026-07-21、IPL起動〜タイトル画面表示まで確認）。

### 割り込み

RECALIBRATE / SEEK / READ DATA / WRITE DATA 完了後に `fdc.irq` が立ち、
`ioi_fdc_rise()` 経由でIOIへ反映し、`x68k_bus_irq_level()`（`src/bus/x68k_bus.c`）がIPL1を返します。

### 既知の未実装・注意点

- **Drive Select レジスタ（$E94007）の読み出し**は未実装（常に 0xFF）。実機で
  この値を参照するソフトがあれば影響が出る可能性があります。ただし ROM の
  `_B_READ`/`_B_WRITE`ハンドラ（`IPLROMXV_addr.S` $FF8E20/$FF8D58）はこの
  レジスタを読まないため、IOCS fn=0x45/0x46 のROM委譲（下記）には影響しません。
- **READ TRACK** は未実装（WRITE DATA・FORMAT TRACKは実装済み）。READ TRACKを
  使う `_B_READDI` はIOCS層でCスタブ（D0=0）のため実FDCに到達しない
  （`_B_VERIFY`、`_B_WRITED`/`_B_READDL`、`_B_FORMAT` はROM委譲済み、下記参照）。**(READ|WRITE) DELETED DATA自体は
  実装済み**（旧記載を訂正）。
- **DDAM(削除データ)状態はメモリ上のみで永続化しない**。XDFファイルへは
  書き戻さず、イジェクト・再挿入・エミュレータ再起動でクリアされます。
- **0x05/0x06/0x09/0x0CはIOCSが発行する SK(スキップ)=0・EOT(最終セクタ)=R
  の単一セクタ形のみ対応**。SK=1のスキップ動作、MT(マルチトラック)、
  EOT>Rの複数セクタ探索は未対応。
- **ライトプロテクト(WP)は実装済み**（下記「FDDライトプロテクト」節参照）。

### IOCS `_B_VERIFY`(fn=0x41) の ROM 委譲（2026-09-03）

従来はIOCS層でD0=0固定のCスタブだったが、ROMの`_B_VERIFY`($FF8B48)ハンドラへの
委譲に切替済み。

- ROMハンドラは`_B_READ`/`_B_WRITE`と同じ`dbra d4`ループ構造で、**セクタごとに
  新しいSCAN EQUALコマンド(opcode 0x11)を発行**する（ROM実測$FF8B72の`dbra`）。
  DMA方向はWRITE DATAと同一(ホスト→FDC、$FF92A0がch0 OCR=`$32`を書く)。
- コマンドバイト構成（ROM実測、`docs/ix68k-internals/design/fdc_design.md` の SCAN EQUAL 仕様）:
  byte0=`(D1.b&$C0)|$11`、byte1=`HD<<2|US`、byte2-5=C/H/R/N、byte6=EOT(常にR、
  fdc.cは未使用)、byte7=GPL、byte8=STP(`$FF`、READ/WRITE DATA用DTL値の流用、
  fdc.cは未使用)。
- ゲストが`$E94003`を直接操作できるためROM側検証(`$FF92F6`)には依存できず、
  `disk_off`計算前に標準2HDジオメトリ(N=3,C<77,H/HD一致,1<=R<=8)を検証する
  ガードを追加（未検証だとN>=24でシフト未定義動作・R=0で
  `disk_off`がラップする等の問題があった）。
- DMAC非武装+MAR範囲外のフォールバックでは、READ/WRITE DATAと異なり
  「1バイトも比較していないのに正常終了(SH)」という偽陽性を返さないよう、
  専用の異常終了(ST0=AT, ST1=OR:Overrun)へ遷移します。
- 副次的にDMAC ch0 CCRのSAB(bit4、ソフトウェアアボート)処理を`dmac.c`へ
  追加。ROMはSCANがNot Ready等で実行データフェーズに入らずCOCが立たな
  かったとき、CCRへ`$10`を書いて転送要求を取り消します。従来は`ch0_armed`が
  立ちっぱなしのまま残ってきました。
- **戻り値は`_B_SEEK`と同様`ST0<<24|ST1<<16|ST2<<8|C`の生
  ステータスロングで、一致時も非ゼロ(`$xx000800|C`、ST2のSH)になる**。
- 検証: FD1へ1024バイト書き込み後、一致/不一致/ワイルドカード(`$FF`)/
  境界値(R=0,R=9,N=4,N=3+FM)/シリンダ範囲外/メディア無しをMCP実機トレースで
  確認、TSRフック到達性をRAMベクタ`$0504`のディスアセンブルで確認、実ソフト
  `COPY /V`が正常完了しバイト一致することを確認（詳細は`iocs_support.md`
  2026-09-03エントリ、設計書`docs/ix68k-internals/design/fdc_design.md`）。
- SCSI/SASI側(`$FF94C0`)はSCANを使わずREAD(6)+CPUの`cmpm.b`比較で完結する
  ため、`sasi.c`の既存READ実装で足りる（追加実装不要）。

### IOCS `_B_WRITED`/`_B_READDL`(fn=0x49/0x4C) の ROM 委譲（2026-09-04）

従来はIOCS層でD0=0固定のCスタブだったが、ROMの`_B_WRITED`($FF8FD0)/
`_B_READDL`($FF9068)ハンドラへの委譲に切替済み。

- 両ハンドラは`_B_WRITE`/`_B_READ`のセクタループ本体（`$FF8D90`/`$FF8E4A`）へ
  `bra`で合流する設計（`and.b #$c0,d1`でSK(bit5)を強制0、`or.b #$09,d1`/
  `#$0c,d1`でopcodeのみ差し替える）。したがって発行されるFDCコマンドは
  常に**SK=0・EOT=Rの単一セクタ形**（ROM実測$FF8FD0-$FF9096）。
- `fdc.c`側は`case 0x05`+`0x09`、`case 0x06`+`0x0C`をそれぞれ合流させ、
  `deleted`フラグ(`c==0x09`/`c==0x0C`)でDDAMビット操作・ST2(CM)・`res[5]`
  だけを分岐する（コマンドパラメータ検証・DMA設定・エラー応答は共通）。
- **DDAM(Deleted Data Address Mark)状態**は`fdc_t.ddam[4][154]`
  （ドライブ×1232セクタのビットマップ）で保持します。`0x09`はセット、
  `0x05`(通常WRITE)はクリア。`0x06`/`0x0C`は参照のみで、種別が食い違う
  （`0x06`なのにビット=1／`0x0C`なのにビット=0）とST2にCM(`$40`)を立てます。
  この場合**セクタのデータは転送された上で**、`res[5]`(R)は「次セクタ」
  ではなく「実行終了セクタ」自身になる（XEiJ `FDC.java`のREAD DATA/READ
  DELETED DATA両方に同一文言で記載されているR-Phase規則）。
- **DDAM状態は永続化しない**（XDFへ書き戻さず、イジェクト・再挿入・
  再起動でクリア）。`x68k_bus.c`の排出処理と挿入処理(`x68k_bus_load_fdc`)
  の両方でドライブ全体をクリアします。FORMAT TRACK(`fdc_format_complete()`)は
  実際に`memset`したトラックに限り、`fdc_ddam_clear_track()`でそのトラック分をクリアする
  （標準2HDの予備シリンダcyl>=77は成功no-opでDDAMも変更しない）。
- 非FDD PDA（SASI-HDD側）はROMが`btst #12,d1`で弾いて`trap #14`（白帯）へ
  落とすため、`iocs.c`のpre-handlerで`0x4E`スタブと同じ形で事前に`D0=-1`
  を返します。SASI-HDD側の`_B_WRITED`/`_B_READDL`自体はROMにも実装が無い
  （`_B_VERIFY`の`$FF94C0`のようなSASI経路は存在しない）。
- 実行コード追加（`fdc.c`のcase実装）とROM委譲（`iocs.c`のpre-handler）は
  必ず同一コミットで入れます。片方だけ入れるとROM側の`$FF937C`ビジーウェイト
  （FDC完了割り込みでCB=0になるのを待つ）が結果バイトを永久に受け取れず
  ハングする（`default:`が`irq`を立てないため。`_B_VERIFY`のSCAN EQUAL実装時と同型のリスク）。
- ついでに`case 0x11`(SCAN EQUAL)の`128<<N`シフト演算が検証(`N!=3`は拒否)
  より前に評価されていた未定義動作（`N>=24`で符号付きオーバーフロー、
  `N>=32`でシフト幅超過）も本変更で修正した（`0x05`/`0x06`/`0x09`/`0x0C`の
  合流経路で見つかった同型の欠陥）。
- 単体テスト`tests/fdc_deleted_test.c`（T1-T13/T16/T17）+
  `tests/fdc_eject_test.c`（イジェクト・挿入・`format_track()`でのDDAM
  クリア）。
- **実機(MCPトレース)検証（2026-09-04）**: RAMベクタ`$0524`/`$0530`
  （`$0400+4*0x49`/`$0400+4*0x4C`）をディスアセンブルし、Human68k中継
  ルーチン経由でROM本体`$00FF8FE4`/`$00FF907C`（いずれも設計書§1.1/1.2で
  特定した`_B_WRITED`/`_B_READDL`本体のPDA検査後）へジャンプすることを
  確認（TSRフック到達性・非TSR時のROM実行の両方を裏付け、`_B_VERIFY`と
  同じ確認方法）。FD1(B:, PDA=$91)への実行結果（`tools-x68k/asmtest/
  wr_del_test.s`）: (1)C10H0R1へ`_B_WRITED`→正常終了、(2)直後`_B_READDL`
  →CM無し・データ一致、(3)DDAM=1のまま`_B_READ`(0x46)→**ST2にCM
  (`$0100400A`)、データは転送された上でC自体(`R=sec+1`ではなくsec)を
  確認**、(4)`_B_WRITE`(0x45)で通常データに書き戻し、(5)`_B_READDL`→CM
  (`$0100400A`、DDAMクリアを確認)、(6)未触C11H0R1への`_B_READDL`→CM
  (`$0100400B`、C=11一致)。境界値: (7)`_B_WRITED`C=77→`$4104004D`
  (AT|ND、C echo一致)、(8)(9)R=0/R=9→`$FFFFFFFF`（ROM側`$FF92F6`の
  事前検証で拒否、FDCへ到達せず）、(10)`_B_READDL`C=77→`$4104004D`
  （WRITED側と対称）。非FDD PDA: (11)(12)PDA=`$80`(SASI、未接続)への
  `_B_WRITED`/`_B_READDL`→ともに`D0=-1`（非FDD PDAの事前スタブ動作を確認）。
  境界値・PDAテスト後もC10H0R1のデータが(4)で書いた内容のまま変化して
  いないことを確認（境界値拒否がディスクへ影響しないことの裏付け）。
  メディアなし(`tools-x68k/asmtest/wr_del_nomedia.s`、FD1排出状態でA:から
  実行): `_B_WRITED`/`_B_READDL`とも`D0=$49420000`でST0のNR(bit27)が
  立つことを確認、ハングなし。ライトプロテクトは`fdc.c`のWRITE系
  応答生成にNW設定コードが存在しないことをコードレビューで確認済み
  （実機テスト不要。後日「FDDライトプロテクト」節の実装で対応）。`trace_iocs`でIOCS呼び出し自体
  （D1/D2/D3/A1）が期待通りROMへ渡っていることも確認。

### FDDライトプロテクト(WP)のエミュレーション（2026-09-04）

設計書: `docs/ix68k-internals/design/fdc_design.md`。

- **WP状態の取得経路はFDC `SENSE DRIVE STATUS`(0x04)のST3 bit6のみ**。
  `$E94005`にWPビットは存在しない（R: bit7=DISK IN/bit6=ERR DISK固定、
  `InsideX68000.md`図4）ため触っていません。
- **保持方式**: ドライブ単位のフラグ(`fdc_t.write_protect[4]`)。挿入時
  （`x68k_bus_load_fdc()`）にホストのXDFファイルの書き込み権限
  (`access(path, W_OK)`)から自動判定してセットし、排出でクリアする
  （DDAMと同じ非永続化パターンに倣う）。
- **`fdc.c`**: `case 0x04`のST3組み立てにbit6(WP)を追加（メディア挿入中の
  ときのみ）。`case 0x05`/`0x09`(WRITE DATA/WRITE DELETED DATA)は、
  Not Readyの直後・N>7等のジオメトリ検証より前でST0=AT(0x40)+ST1 bit1=NW(0x02)
  を返し、DMA転送・DDAM更新とも一切行いません。READ系・SCAN EQUAL・
  RECALIBRATE/SEEKはWP判定の対象外です。
- **`_B_FORMAT`(0x4D)**: FDDは標準2HDも含め実機ROMへ委譲する（
  それ以前は標準2HDのみIOCS層でホスト実装していた）。NR→WPの判定と結果ステータスは
  ROM経由のFDC FORMAT TRACK(`0x0D`)側が担い、`iocs.c`はSASI-HD(PDA `$80-$8F`)の
  `D0=-1`返却のみ行います。
- **WPトグル**: `x68k_bus_fdc_set_write_protect()`がホストのファイル
  パーミッション（owner writeビットのみ）を実際に変更します。同一ファイルを
  複数ドライブへ挿入している場合はchmod()の効果が全ドライブへ及ぶため、
  呼び出しのたび挿入中の全ドライブを再判定します。FDC実行中(`phase!=FDC_IDLE`)
  は拒否します。
- **GUI**: ディスク選択ウィンドウ(`src/ui/disk_window.c`)のFDD行にRW/WPの
  トグルボタンを追加（`DISK_WIN_W`を400→464へ拡張）。未挿入行には表示しません。
- **MCP**: `disk`ツールに`action="protect"/"unprotect"`、ワイヤ`DISK WP FD
  <index> <ON|OFF>`、`DISK STATUS`に挿入中FDのみ`FD<n>.WP=<0|1>`行を追加
  （詳細は`tools-host/mcp/MCP_TOOLS.md`）。
- 単体テスト: `tests/fdc_wp_test.c`(FDCレイヤ、T1-T11)、
  `tests/fdc_eject_test.c`(busレイヤ、T20-T30。T30は同一ファイル複数ドライブ
  挿入時のWP迂回に対する回帰テスト)。

既知の制約:

- root権限で実行すると`access(W_OK)`が読み取り専用ファイルでも成功するため
  WPを検出できない（通常利用では発生しない）。
- SASI/SCSI側のライトプロテクトは対象外（SASI-HDDに物理WPノッチは無く、
  SCSIはMODE SELECT/DATA PROTECTという別機構。CD-ROMは実装済み）。
- FDC opcode `0x0D`(FORMAT TRACK)は引き続き未実装（上記参照）。

### RESET命令によるソフトリセット（2026-09-10）

現行仕様: `docs/ix68k-internals/design/fdc_design.md`。

- 従来`bus_reset_peripheral()`（CPU RESET命令ハンドラ）は`b->fdc`に対しFDD READY
  信号の復元しか行っておらず、実行データフェーズ・コマンド/結果バッファ・
  割り込み要求がソフトリセットをまたいで残留してきました。IPLROMのFDCステータス
  待ちループ（CB(bit4)ポーリング）がRESET後に永久に抜けられなくなる、
  ソフトリセット後はイジェクトが恒久的に拒否される、等の実害がありました。
- `fdc_reset(fdc_t *f)`を新設（`fdc.c`/`fdc.h`）し、`bus_reset_peripheral()`の
  `dmac_reset(b)`直後と、`x68k_bus_create()`（電源投入経路）の両方から呼びます。
- **リセット対象**: `phase`(→`FDC_IDLE`)、`cmd[]`/`res[]`とその添字、`irq`、
  `dma_ptr`/`dma_remaining`/`dma_dir`/`dma_drv`/`dma_offset`/`dma_size`、
  `dma_cmp`/`scan_mismatch`、`sense_cyl`。`st0`は0ではなく**`0x80`**
  （SENSE INTERRUPT STATUSの「割り込み無し」番兵）へ戻す（0だと「ドライブ0の
  シーク正常終了」を偽って名乗り`irq=0`と矛盾する）。
- **保持対象**: `disk[]`/`disk_bytes[]`/`disk_path[]`/`num_drives`（メディア挿入
  状態、RESET命令は電源断ではない）、`ddam[]`/`write_protect[]`（挿入・排出で
  のみ変化）、`cur_cyl[]`（機械的ヘッド位置。RESETではFLTR/STEP出力がLowに
  なるだけでステップパルスは出ず、ヘッドは動きません。µPD72065Bデータシート
  p4脚注・XEiJ`FDC.java`の`fdcReset()`（`FDUnit.fduPCN`を変更しない）の両方で
  裏付け）。
- **回路図での裏付け**: 『Outside X68000』の回路図 p10「コントロール回路図」
  でIC203(µPD720651S、40pin DIP)の1番ピン(RESET)がRESETネットに直結して
  いることをピンレベルで確認済み。
- µPD72065外の
  コントロール基板ゲートアレイ(PEDEC, IC213)側のFDD制御ラッチも整理しました。CPU RESETでは
  `$E94005`の前回書込みラッチ`drv_ctrl_latch`と、`$E94007`由来でアクセスランプにだけ使う
  `fdc_drive_sel`/`fdc_motor_on`をクリアします。前者はXEiJの
  `fdcDriveLastSelected == null`（Drive Statusが0）と可視挙動を合わせ、RESETをまたぐ
  1→0エッジによる意図しない排出も防ぎます。書込みハンドラの副作用を避け、直接代入します。
  一方、物理イジェクト禁止の確定状態`fdc_eject_masked_bits`と抜き挿し通知
  `fdc_ejected_flag[]`は保持します。PEDEC内部のRESET詳細を示す一次資料はないため、
  この分類はXEiJの可視挙動とIPLROMの選択シーケンスに基づきます。
- FDC割り込み(`fdc.irq`)の伝搬先はMFP GPIPではなく**IOI（$E9C001、IRQレベル1）**
  （`x68k_bus.c:590`付近のエッジ検出経由）。
- 単体テスト: `tests/fdc_reset_test.c`（実行データフェーズのクリア、MSR CB解除、
  メディア状態/`cur_cyl[]`の保持、`irq`とIOI側の整合、SENSE INTERRUPT STATUS
  応答、リセット後のイジェクト解除、電源投入直後の`st0`確認）。MCP実機相当
  検証（冷起動、68000/68EC030両構成での連続リセット、FDアクセス後のリセット、
  FD未挿入時のSCSI/HDD起動、イジェクト）で回帰なしを確認済み。

### IOCS `_B_READ`/`_B_WRITE`(fn=0x46/0x45) の ROM 委譲（2026-07-09）

従来はIOCS層でXDFイメージへ直接memcpyするバイパスを使っていたが、ROMの
`_B_READ`($FF8E20)/`_B_WRITE`($FF8D58)ハンドラへの委譲に切替済み。

- 両ハンドラは`dbra d4`ループで**セクタごとに新しいREAD/WRITE DATAコマンド
  （opcode 0x06/0x05）を発行**する設計（`IPLROMXV_addr.S` $FF8E8E/$FF8DD4の
  `dbra`）で、上記コマンド表の実装と1コマンド=1セクタの粒度が一致するため、
  `fdc_exec`側の拡張は不要でした。
- 過去に問題化した「コマンド0x80が$FF93D6でDIO待ち無限ループを起こす」
  （commit `db34135`）は、ROMのどのFDDハンドラも妥当なオペコード・バイト数
  でしか呼び出さないため再現しないことをROM逆アセンブル解析で確認済み。
- 検証: 冷起動→`A>`到達、`DIR`/`DIR SYS`/`TYPE`、73462バイト
  （約72セクタ）ファイルのマルチセクタ`COPY`を`BIN\FC.X`でバイト比較
  （「差異なし」）、`eject_disk`後の読み出しで正しい「ディスクが入って
  いません」応答。
- 旧バイパス関数`x68k_bus_fdc_read_raw`/`write_raw`は削除済み（未参照）。


## 変更履歴

| 日付 | 内容 |
|------|------|
| 2026-09-13 | CPU RESETでPEDEC側のDrive Control前回値ラッチとDrive Select由来のアクセスランプ状態をクリア。物理イジェクト禁止状態と抜き挿し通知は保持。 |
| 2026-09-10 | RESET命令(CPU RESET/ソフトリセット)によるFDC(µPD72065)内部状態の初期化`fdc_reset()`を新設。実行データフェーズ・コマンド/結果バッファ・割り込み要求をクリア、メディア挿入状態・`cur_cyl[]`は保持。詳細は本文「RESET命令によるソフトリセット」節参照 |
| 2026-09-04 | FDDライトプロテクト(WP)を実装。ST3 bit6/WRITE系のST1 bit1(NW)/`_B_FORMAT`のWP判定/ホストファイル属性による保持とトグル/GUI・MCP対応。詳細は本文「FDDライトプロテクト(WP)のエミュレーション」節参照 |
| 2026-09-04 | WRITE/READ DELETED DATA(0x09/0x0C)をIOCS `_B_WRITED`(0x49)/`_B_READDL`(0x4C)用に新規実装しROM委譲。DDAMビットマップ(非永続化)を追加。ついでに`case 0x11`のシフト未定義動作も修正。詳細は本文「IOCS `_B_WRITED`/`_B_READDL`の ROM 委譲」節参照 |
| 2026-09-03 | SCAN EQUAL(0x11)をIOCS `_B_VERIFY`(0x41)用に新規実装しROM委譲。詳細は本文「IOCS `_B_VERIFY`の ROM 委譲」節参照 |
| 2026-07-29 | レジスタ単位の存在監査。コマンド表にSCAN EQUAL/LOW/HIGH($11/$19/$1D)・SET STANDBY($35)・RESET STANDBY($34)・SOFTWARE RESET($36)を追加（既存表に一度も記載が無かった未実装コマンド、『Inside X68000』5-7〜5-16節でopcode確認） |
| 2026-07-07 | 初版作成（`.claude/commands/hw-impl.md` から詳細部分を移設・実装状況を実コードと突き合わせて修正。WRITE_DATA は実装済みと訂正） |
| 2026-07-29 | `docs/hw_support.md`から本ファイルへ分離（ペリフェラル単位でのファイル分割） |
