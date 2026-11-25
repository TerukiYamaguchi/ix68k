# 内蔵 SCSI（SPC/MB89352）設計

## 1. 目的と対象範囲

ix68k は X68000 XVI の内蔵 SCSI を、SPC（MB89352）レジスタ、実 SCSI BIOS ROM、
DMAC ch1、SCSI ターゲットで構成します。本書は現行仕様を定めます。

- 対象: `$E96020` の SPC、`$FC0000` の SCSI BIOS ROM、SCSI HDD/MO/CD-ROM、
  SCSI フェーズとデータ転送、IOI 割り込み。
- 対象外: 拡張 SCSI ボード、リセレクション／ディスコネクト、パリティ。
- レジスタ・コマンドの詳細な対応状況は [`scsi_support.md`](../support/scsi_support.md) を正本とします。

## 2. 実装状態

| 項目 | 状態 | 補足 |
| --- | --- | --- |
| SPC 14 レジスタ | ✅ | 内蔵アドレスへ MMIO 接続 |
| SCSI BIOS ROM | ✅ | `$FC0000–$FC1FFF` の 8 KiB `SCSI16IN` |
| HDD/MO | ✅ | `X68SCSI1` ヘッダ付き raw イメージ |
| 固定HDD COW差分 | ✅ | `scsi_overlayN` 指定時のみ。base HDSは不変 |
| CD-ROM | ✅ | CloneCD の Mode1 データトラック |
| DMAC ch1 転送 | ✅ | DREG を1バイトずつ経由 |
| IOI 割り込み | ✅ | IPL1、固定ベクタ `$6C` |
| 内蔵 SASI/HDC | ✅ | `$E96000`、SCSI ユニット配列を共有 |
| LUN | ⚠️ | 各 SCSI ID の LUN 0 のみ |
| 拡張ボード・切断再選択 | ❌ | 未対応 |

## 3. 設計の要点

1. IPL から実 SCSI BIOS ROM を実行し、ROM が SPC を操作する経路を再現します。IOCS の
   ブロック I/O で置き換えません。
2. SPC は SCSI のフェーズ状態機械を持ち、COMMAND／DATA IN／DATA OUT／STATUS／
   MESSAGE／BUS FREE を遷移します。
3. CPU の手動ハンドシェイクと、DMAC ch1 による DREG 転送は同じフェーズ処理を通します。
4. ターゲットは ID 0–6、LUN 0 のみを提供します。未対応 LUN は INQUIRY では未対応 LUN
   として応答し、他コマンドでは CHECK CONDITION を返します。
5. 内蔵 SASI/HDC も同じユニットの存在判定（媒体ファイルまたは空リムーバブル）を使います。
   CDB の LUN は解釈し、LUN 0 以外を SASI 形式の INVALID COMMAND センスで拒否します。

## 4. 構成とデータモデル

`x68k_bus_t::spc_in` が SPC 状態と最大7ユニットを所有します。各 `scsi_unit_t` はメディア種別、
ホストファイル、ブロック長、総 LBA、リムーバブル／ライトプロテクト状態を持ちます。固定HDDに
明示的なCOW差分を指定した場合だけ、ユニットは差分ファイルとLBA→最新レコードの索引も所有します。

| 要素 | 実装 | 責務 |
| --- | --- | --- |
| SPC 状態機械 | `src/peripheral/scsi.c` | 選択、フェーズ、CDB、応答、センス |
| SASI/HDC 状態機械 | `src/peripheral/sasi.c` | 内蔵 HDC の選択、CDB、1バイトセンス |
| メディア接続 | `src/peripheral/scsi_media.c` | raw HDD/MO・CloneCD の解析と入出力 |
| バス・DMAC・IOI | `src/bus/x68k_bus.c` | MMIO、ch1、IRQ、接続 API |
| ROM | `src/peripheral/scsi_rom_data.h` | `$FC0000` の SCSI16IN |

HDD/MO は `X68SCSI1` ヘッダからブロック長と総ブロック数を得ます。CD-ROM は `.ccd` と対の
`.img` を使い、2352 バイトの Mode1 セクタから先頭16バイトを除いた 2048 バイトを論理ブロック
として提供します。

## 5. インターフェースと転送

SPC は `$E96021–$E9603F` に配置します。BDID、SCTL、SCMD、INTS、PSNS/SDGC、SSTS、
SERR、PCTL、MBC、DREG、TEMP、TCH/TCM/TCL の全14レジスタを実装します。

内蔵 SCSI BIOS は `$FC0000–$FC1FFF` に置き、IOI 経由で IPL1 を要求します。SPC は IOI 内で
最優先、ベクタは `$6C` 固定です。

COMMAND の最初のバイトを受けた時点で opcode から CDB 長を確定します。これは TEMP +
Set/Reset ACK/REQ の CPU 経路と、Transfer + DREG 連続アクセスの PIO/DMAC 経路で共有します。
DREG は `TRIP` と転送カウントが有効な間だけ一バイトを進め、短い DATA IN 応答の後に余剰アクセスが
STATUS や MESSAGE を消費しないよう、残データ量でもゲートします。

DMAC ch1 は DREG を一バイトずつ読み書きします。MAR/MTC は実際に転送した分だけ更新し、
MTC が 0 になった通常完了時だけ COC を立てます。DMAC ch1 の NIV 割り込みは未実装です。

## 6. コマンドとエラー応答

実装済みコマンドは Test Unit Ready、Rezero、Request Sense、Format Unit、Read/Write(6)、
Seek(6)、Inquiry、Mode Select/Sense(6)、Start-Stop、Prevent-Allow、Read Capacity、
Read/Write(10)、CD-ROM 用 READ TOC です。

Format Unit は `X68SCSI1` ヘッダを保護する no-op とし、後続の管理領域書き込みを許します。
Mode Sense は page `0x00` と `0x3F` のみを扱います。未対応 opcode、未対応ページ、範囲外 LBA、
不正 LUN、非CD-ROMへの READ TOC は CHECK CONDITION となります。

センスは SPC 全体で一つの固定長18バイト SCSI-2 拡張センスを保持します。CHECK CONDITION の都度、
レスポンスコード、センスキー、追加センス長、ASC/ASCQ を設定します。主な応答は NOT READY/
Medium Not Present、UNIT ATTENTION/媒体状態変化、DATA PROTECT/Write Protected、
ILLEGAL REQUEST（不正 CDB・LUN・opcode）です。

## 7. メディアと書き込み保護

CD-ROM は常にリムーバブルかつ書き込み禁止です。MO はリムーバブル raw ディスクとして扱い、
接続時のホスト書き込み権限で保護状態を決めます。`r+b` が開けない場合は `rb` へフォールバックし、
保護状態にします。Write(6)/(10) と Format Unit は DATA PROTECT を返し、Mode Sense は WP ビットを立てます。

リムーバブル MO は API で保護状態を切り替えられます。対象ユニットが選択・転送中なら拒否し、
同一ホストファイルを接続する全ユニットの状態を再判定します。HDD と CD-ROM はトグル対象外です。

挿入・排出後のリムーバブル媒体は UNIT ATTENTION を保留し、次の通常コマンドに
Not Ready To Ready Transition を返します。空の MO/CD-ROM ドライブも接続でき、メディア操作は
CHECK CONDITION/Medium Not Present で応答します。

### 7.1 固定HDDの copy-on-write 差分

設定ファイルで `scsi_diskN` と同じIDに `scsi_overlayN` を指定すると、非リムーバブルの
`X68SCSI1` HDDだけをCOWモードで接続します。CD-ROM、MO、未接続ユニットは対象外であり、
既存の `scsi_diskN` 単独指定の書込み動作は変えません。

base HDSは読み取り専用のままでよい。ゲストには書込み可能なディスクとして提示し、
Write(6)/(10) の完了時はbaseへ書かず、差分ファイルに `{LBA, block size, block data}`
レコードを追記します。Read(6)/(10) は最初にbaseを読み、差分に存在する各LBAだけを最新レコードで
置換します。差分ファイルは `IX68COW1` ヘッダを持ちます。空でない別形式のファイルは拒否し、base HDS
ごとに新規作成します。複数プロセスで同じ差分ファイルを共有してはなりません。

差分を有効にした固定HDDは、base HDSの媒体フィンガープリントを保存するため、メモリ内
スナップショットの外部媒体制約を満たします。スナップショット作成時には差分索引を複製し、復元の
prepare段階で必要な複製を全て確保します。commitは準備済み索引の所有権を移すだけなので、メモリ不足で
部分復元にはなりません。保存後に追加された差分レコードはファイルに残るが、復元後の索引からは参照されず、
保存時点のディスク内容へ戻る。base HDSの変更・媒体挿抜・設定変更は従来どおり復元を拒否します。

## 8. 互換性・既知の制約

- LUN ごとのセンス保存はなく、SPC 全体で共有します。
- CD-ROM は CloneCD の非スクランブル Mode1 データトラックだけを受け付け、音声トラックは未対応。
- Mode Select のパラメータ、Start-Stop のイジェクト、Prevent-Allow は内容を解釈しません。
- 未実装コマンドは CHECK CONDITION で終了します。
- SPC の Hard Error、Reset Condition、パリティ関連の詳細な発生試験は未完了です。
- COW差分はプロセス内スナップショット用であり、差分の圧縮、baseイメージへのマージ、
  並行アクセスの調停は行いません。

## 9. テストと受入条件

| 観点 | テスト | 期待結果 |
| --- | --- | --- |
| ステータス・センス・LUN | `tests/scsi_status_test.c` | CHECK CONDITION と18バイトセンスが一致 |
| CD-ROM | `tests/scsi_cdrom_test.c` | 読み出し、TOC、LBA/MSF、要求長を処理 |
| 書込み保護 | `tests/scsi_wp_test.c` | 自動判定、トグル、DATA PROTECT を確認 |
| COW差分 | `tests/scsi_wp_test.c` | 読取り専用baseを変更せずWrite(6)/Read(6)が往復する |
| COW＋スナップショット | `tests/bus_snapshot_test.c` | 差分書込み後に保存し、追加書込みをloadで巻き戻す |
| CDB/DREG | `tests/scsi_dreg_cdb_test.c` | 6/10バイトCDBと短転送が完了 |
| SASI 選択/LUN | `tests/scsi_status_test.c` | 空リムーバブル、未接続 ID、LUN 拒否、センスを確認 |
| 統合 | SCSI HDD ブート、SUSIE.X | BIOS/DMAC経路の起動・CD-ROM読出し |

## 10. 根拠資料

- 一次資料: *Inside X68000* の内蔵 SCSI、SPC、DMA、SRAM の各節。
- ROM実測: XVI の SCSI BIOS `SCSI16IN` と IPL の `$FC0000` 委譲。
- 参照実装: XEiJ の `SPC.java`、`IOInterrupt.java`、`HD63450.java`。

## 11. 更新履歴

| 日付 | 変更内容 |
| --- | --- |
| 2026-09-11 | 現行仕様を HDD ブート・CD-ROM の設計記録から分離して新設 |
| 2026-09-11 | SASI の選択/LUN、MO の書込み保護を Issue 別設計から統合 |
| 2026-11-02 | 固定HDD向け明示COW差分と、差分索引を含むメモリ内スナップショット復元を追加 |
