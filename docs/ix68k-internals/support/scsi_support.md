# SCSI (SPC/MB89352) 実装状況

`src/peripheral/scsi.c`（SPCレジスタ・コマンド）と
`src/peripheral/scsi_media.c`（メディア接続・切断）。詳細な設計経緯は
`docs/ix68k-internals/design/scsi_design.md` 参照。
関連: DMAC ch1 は `docs/ix68k-internals/support/dmac_support.md`、IOCS は `docs/ix68k-internals/support/iocs_support.md`。

> **必須**: SPC のレジスタ・コマンドを実装・変更したら、このファイルの該当表を必ず更新します。

---

存在監査（2026-07-29）でレジスタマップ・コマンドセットを一次資料と突き合わせ。

### レジスタマップ

全14レジスタ（BDID/SCTL/SCMD/INTS/PSNS/SDGC/SSTS/SERR/PCTL/MBC/DREG/TEMP/TCH/TCM/TCL）
とも実装済み。オフセットは`docs/ix68k-internals/design/scsi_design.md` §5に記載の通り、
『Inside X68000』図11のOCR列崩れ（PSNS/SDGCを誤って別オフセット2行に
分離した結果、SSTS以降が全て+2ずれていた）をXEiJ突き合わせで訂正済みのものを採用
しており、コードと設計資料は一致している（監査で再確認、新規の齟齬なし）。

### コマンドセット実装状況

`spc_execute_command()`で15コマンドを実装:

| コマンド | opcode | 状態 |
|---|---|---|
| Test Unit Ready | 0x00 | ✅ |
| Rezero Unit | 0x01 | ✅ no-op（XEiJ準拠） |
| Request Sense | 0x03 | ✅ |
| Format Unit | 0x04 | ✅ X68SCSI1ヘッダを保持するno-op（FmtData=0を想定）。write_protectedユニットはDATA PROTECT |
| Read(6) | 0x08 | ✅ |
| Write(6) | 0x0A | ✅ write_protectedユニットはDATA PROTECT |
| Seek(6) | 0x0B | ✅ LBA=CDB[1]下位5bit:CDB[2]:CDB[3]。範囲内はGOOD(no-op)、範囲外はILLEGAL REQUEST（XEiJ scuDoSeek6準拠） |
| Inquiry | 0x12 | ✅ peripheral device type: CD-ROM=0x05、それ以外(HDD/MO)=0x00 |
| Mode Select(6) | 0x15 | ✅ パラメータ受信のみ、内容非解釈 |
| Mode Sense(6) | 0x1A | ✅ ページコード0x00/0x3Fのみ対応（ヘッダ+ブロックディスクリプタ）。write_protectedユニットはWPビット(device-specific parameter bit7)を立てます。未対応ページ（page 4=rigid disk geometry、page 5=flexible disk等）はILLEGAL REQUEST / ASC=0x24でCHECK CONDITION |
| Start-Stop Unit | 0x1B | ✅ イジェクト制御は非対応、常にGOOD |
| Prevent-Allow Medium Removal | 0x1E | ✅ no-op |
| Read Capacity | 0x25 | ✅ |
| Read(10) | 0x28 | ✅ |
| Write(10) | 0x2A | ✅ write_protectedユニットはDATA PROTECT |
| READ TOC | 0x43 | ✅ CD-ROMのみ。HDD/MOユニットではILLEGAL REQUEST |

**未実装コマンド（`default:`分岐でCHECK CONDITION応答、ハングはしない）**:
Reserve(0x16)/Release(0x17)、Copy(0x18)、
Receive Diagnostic Results(0x1C)、Send Diagnostic(0x1D)、
Seek(10)(0x2B)、Write and Verify(0x2E)、Verify(0x2F)等。

Format UnitはHuman68k `FORMAT.X`が後続処理で管理領域を書き直す前提でGOODを返します。
媒体全域の物理消去や欠陥リスト処理は行いません。

### MO(リムーバブル)ライトプロテクト対応状況

`write_protected`（Write(6)/(10)・Format UnitのDATA PROTECT拒否、Mode Sense(6)の
WPビット応答）自体はCD-ROM対応で汎用実装済みだったが、**設定側**は
種別ごとに固定値でした。設計は`docs/ix68k-internals/design/scsi_design.md`を参照。

- **`attach_hdd()`（`scsi_media.c`）**: `fopen(path, "r+b")`が失敗したら`fopen(path, "rb")`
  へフォールバックする（読み取り専用ボリューム・chmod済みイメージでも接続できる
  ようにするため）。`r+b`に失敗した場合は種別を問わず`write_protected=1`を強制する
  （物理的に書けないファイルへの書き込みを防ぐ安全策）。DATA OUT後の`fwrite()`/
  `fflush()`失敗もCHECK CONDITION（MEDIUM ERROR / WRITE ERROR）で返します。
- **removable(MO)は`x68k_bus_scsi_insert()`（`x68k_bus.c`）で確定判定**: `scsi_attach()`
  成功後、`removable && kind==SCSI_UNIT_DISK`のユニットのみ、ホストのファイル
  書き込み権限(`access(W_OK)`、FDD側`fdc_probe_write_protect()`を汎用名
  `host_path_write_protected()`へ改名し共有)から`write_protected`を上書きします。
  `scsi_media.c`内の`static`関数として直接呼べない（別翻訳単位）ため、呼び出しを
  bus層に一本化しています。非removable(HDD)は`attach_hdd()`が設定した値をそのまま使う
  （固定HDDには物理ノッチが無いため自動判定・トグルAPIの対象外、CD-ROMは
  `write_protected=1`固定のまま変更なし）。
- **トグルAPI**: `x68k_bus_scsi_write_protected()`/`x68k_bus_scsi_set_write_protect()`
  （`x68k_bus.h`の`x68k_scsi_err_t`）。対象はremovableな`SCSI_UNIT_DISK`のみで、
  HDD/CD-ROMは`X68K_SCSI_ERR_NOT_SUPPORTED`。ビジー判定は`spc->target_id == id`
  （SCSIは単一共有バスのため対象ID単位のブロックで足り、FDCの「フェーズ全体を
  一律ブロック」より狭い）。ホストのファイルパーミッション（owner writeビットのみ）
  を実際に変更し、同一ホストファイルを複数SCSI IDへ挿入している場合に備え
  接続中の全SCSIユニットを再判定する（FDDのライトプロテクト判定と同じ方針）。
- 内蔵HDC（`sasi.c`、`units[]`をSPCと共有）はWRITE(0x0A)を実装しておらず、
  MOへ書き込める経路はSPC(本ファイル)のみであることを確認済み（迂回リスク無し）。
- `tests/scsi_wp_test.c`に18ケースを追加（自動判定・rbフォールバック・トグルAPI・
  複数ID同一ファイル対策・ビジーガード・HDD/CD-ROM除外・退行確認）。

### 固定HDD COW差分

設定ファイルの `scsi_overlayN` は、同じ `scsi_diskN` で接続した非リムーバブルHDDだけに
copy-on-write差分を有効化します。差分は `IX68COW1` ヘッダとブロックごとの追記レコードで構成し、
base HDSは一切変更しません。Read(6)/(10) はbase読出し後に差分の最新ブロックを重ね、
Write(6)/(10) は差分だけへ書き込みます。CD-ROM・MOには指定できません。

差分を有効にしたHDDはゲストから書込み可能だが、baseの媒体フィンガープリントを保持するため、
停止中の `SNAPSHOT SAVE/LOAD` を使用できます。保存時の差分索引を復元時に差し替えるので、
保存後のゲスト書込みはloadで見えなくなります。差分ファイル自体のマージ・圧縮・複数プロセス共有は
未対応です。回帰は `tests/scsi_wp_test.c` と `tests/bus_snapshot_test.c` が担います。

### CD-ROM(CloneCD)対応状況

現行仕様は`docs/ix68k-internals/design/scsi_design.md`参照。`scsi_unit_t`に
`kind`(`SCSI_UNIT_DISK`/`SCSI_UNIT_CDROM`)・`write_protected`を追加し、
既存の`bytes_per_record`/`disk_end_record`経路をそのまま流用する設計
（CD-ROMではマウント時に`bytes_per_record=2048`・`disk_end_record=リードアウトLBA`
を設定するだけで、Read(6)/(10)・Read Capacity・Mode Senseの応答は無改造で
正しい値を返す）。

- **P1完了**（メディア層）: `scsi_media.c`が`.ccd`+`.img`(CloneCD)のマウントに対応。
  拡張子`.ccd`(大小無視)または内容が`[CloneCD]`で始まればCD-ROMと判定します。
  Mode1データトラック・`DataTracksScrambled=0`以外は拒否。`scsi_unit_read()`
  （`spc_read_blocks()`の内部で使用）が2352Bセクタの先頭16バイトをスキップして
  2048Bユーザデータを抽出します。空CD-ROMドライブは`scsi_attach_empty_cdrom()`
  （cfg `scsi_diskN = CD`）。
- **P2完了**（本表のコマンド層）: 上表の通りINQUIRY・Mode Sense・書き込み系を
  CD-ROM対応。`CDROM.X`はINQUIRYのAllocation Lengthに92を指定し、内蔵SCSI ROMは
  その値をDMAC転送カウントへそのまま設定するため、標準応答36バイト以降を要求長まで
  ゼロ埋めして返します。36バイトでDATA INを終了するとDMACカウントが残り、IOCSが完了待ちに
  なります。動作検証では`CDROM 2`が対象CloneCDのルート68項目（2ページ）を正常表示しました。
- **P3完了**（READ TOC）: `scsi_write_toc_descriptor()`でトラック/リードアウトの
  8バイトディスクリプタを組み立てます。`start_track`が0(トラック1扱い)/`0xAA`(リードアウトのみ)
  /実トラック番号を受理し、範囲外はILLEGAL REQUEST。MSF=0はLBAをビッグエンディアン32bit、
  MSF=1は`total=lba+150; M=total/4500; S=(total%4500)/75; F=total%75`で変換。
  Allocation Lengthは`transfer_len=min(alloc, response_len)`で頭打ちし、ヘッダには
  常に完全な応答長を記録する（安全な方式）。`cd_leadout_adr_control`
  （`.ccd`のPoint=0xA2エントリのADR/Control）を新規フィールドとして`scsi_unit_t`に追加。
  `tests/scsi_cdrom_test.c`にLBA/MSF変換・リードアウトのみ要求・Allocation Length境界・
  HDD回帰の5ケースを追加。動作検証: `SUSIE -ID2 *:`常駐で`SUSIE E: -P`のDriver欄が
  **IS9**、Volume欄が**MKCDP0001**、`DIR E:`でISO-9660ファイル一覧が正常表示されました。
- **P4完了**（Human68k運用）: 起動HDS(`disk/SYSTEM_HD.HDS`)のCONFIG.SYSに
  `DEVICE = \SYS\SUSIE.X -ID2 *:`を追加、`ix68k.cfg`に`scsi_disk2 = CD`
  （空CD-ROMドライブとして起動時接続）を追加。動作検証: 冷起動でSUSIE.Xが自動常駐し
  `D:`がメディア未挿入(`** Not Inserted **`)で確保され、MCPでイメージ挿入後は
  reset無しで認識、`DIR D:`/`TREE D:`/`COPY D:\...`（対象CloneCDからのファイル
  コピー）が正常動作しました。
- **P5完了**（MCP・文書）: `INSERT_SCSI`（`tools-host/mcp/ix68k_mcp_server.py`の`disk()`）は
  P1の`scsi_attach()`形式自動判別をそのまま使うため追加配線は不要と確認、
  docstringにCD-ROM(.ccd指定)の説明を追記。`docs/x68k-usage/freeware/SUSIE.X.md`
  の検証状態を📖→✅へ更新。GUI(`src/ui/disk_window.c`のユニット種別表示)は
  設計文書で任意項目のため見送り。

### センスデータ（Request Sense応答）

`scsi_t.sense_buf` は SCSI-2 の固定長 18 バイト（`SCSI_SENSE_LENGTH`）。
組み立ては `spc_set_sense(spc, key, asc, ascq)` に一本化してあり、
CHECK CONDITION を返す経路は必ずこれを通します。

| オフセット | 内容 |
|---|---|
| 0 | レスポンスコード `0x70`（現在のエラー、拡張センスフォーマット） |
| 2 | センスキー |
| 7 | 追加センス長（`SCSI_SENSE_LENGTH - 8` = 10） |
| 12 | ASC |
| 13 | ASCQ |

`spc_set_sense()` は毎回バッファ全体をクリアしてから書きます。前回のセンスの
ASC/ASCQ が残ると、キーだけ更新した次のエラーで誤った追加センスコードを
返してしまうため。

**キーだけでなく ASC/ASCQ も必ず設定する**。イニシエータは ASC まで見て
エラーを分類します。例:

- NetBSD `scsipi_interpret_sense()`（`sys/dev/scsipi/scsipi_base.c`）は
  NOT READY で `asc == 0x3A` なら `ENODEV`、UNIT ATTENTION で `asc == 0x29` なら
  `ERESTART` と、ASC で分岐する
- センスを設定しないまま CHECK CONDITION だけ返すと、直前の Request Sense が
  `sense_buf` をゼロクリアしているため続く Request Sense が全ゼロを返します。
  `sense[0] == 0x00` はレスポンスコード「エラーなし」と解釈され、
  イニシエータ側は CHECK CONDITION の理由を取得できない

現在の使用値:

| 経路 | センスキー | ASC |
|---|---|---|
| 未対応LUNへの非Inquiryコマンド | 0x05 ILLEGAL REQUEST | 0x25 Logical Unit Not Supported |
| リムーバブルのメディア状態変化 | 0x06 UNIT ATTENTION | 0x28 Not Ready To Ready Transition |
| メディア未挿入 | 0x02 NOT READY | 0x3A Medium Not Present |
| write_protectedユニットへの書き込み系 | 0x07 DATA PROTECT | 0x27 Write Protected |
| 未実装opcode、非CD-ROMへのREAD TOC | 0x05 ILLEGAL REQUEST | 0x20 Invalid Command Operation Code |
| Seek(6)範囲外、Mode Sense(6)未対応ページ、READ TOC開始トラック範囲外 | 0x05 ILLEGAL REQUEST | 0x24 Invalid Field in CDB |

sense はチップ全体で1個のため、LUN・ユニットをまたいで上書きされる（未対応）。

### イニシエータ→ターゲット方向の転送経路（2種類ある）

ホストからSPCへ1バイト送る経路は2つあり、**どちらもフェーズ固有の処理を通す必要がある**。

| 経路 | 手順 | 使う実装 |
|---|---|---|
| CPU 1バイト転送 | `TEMP`へ書く → `SCMD`=Set ACK/REQ(`$E0`) → Reset ACK/REQ(`$C0`) | 内蔵SCSI ROM |
| プログラム転送 | `TC`に長さ設定 → `PCTL` → `SCMD`=Transfer(`$80`) → `DREG`へ連続書き込み | NetBSD/x68k `spc`ドライバ（`sys/dev/ic/mb89352.c` `spc_dataout_pio()`）、DMAC ch1 |

COMMANDフェーズは**先頭バイトのopcode上位4bitでCDB長が決まる**ため、`spc_command_phase()`は
`buf_limit = 1`で開始し、1バイト受領した時点で`SPC_CDB_LENGTH[]`から確定させます。この確定処理は
両経路が共有する`spc_store_out_byte()`に置きます。片方だけに置くと、もう一方の経路ではCDBの
2バイト目以降が`buf_index < buf_limit`ガードに弾かれてCDBが完成せず、`spc_transfer_complete()`
（＝`INTS.CC`）に到達しないままドライバがINTSを無限ポーリングします。

DMAC ch1もDREGを1バイトずつ通します。SPCのゲートは各バイトで再評価するため、COMMANDの
先頭バイトでCDB長が確定する経路と、DATA IN短転送でREQが落ちる経路の双方で余剰転送しません。
MTC/MARは実転送バイト数だけ進め、MTCが0になった通常完了時だけCSR.COCを立てます。SPC側の
フェーズ完了通知はINTS.CC（BUS FREE時はINTS.DC）であり、ch1のDMAC NIV割り込みは未実装です。

Set/Reset ACK/REQ経路は手動ハンドシェイクであり、TCを減算しません。Set ACK/REQ後は
ACK=1/REQ=0、Reset ACK/REQ後はACK=0/REQ=1（転送最終バイトでは次フェーズへ遷移）となります。
TCの各バイト書き込みはTransfer開始前でも、直ちにSSTS.TC0へ反映されます。

DREGのread/writeは`SSTS.TRIP && TC != 0`の間だけ1バイトを進めます。完了判定は`tc == 0`に
加えて`buf_index == buf_limit`でも成立させる（DREG読み出し側と対称。CDB長より大きい`TC`を
設定するドライバでもCDB最終バイトで完了通知に到達させるため）。DATA INの`SSTS.DF/DE`はTCだけで
なく実データ残量で決め、短転送完了時はDFを落としてDEを立てます。これにより余剰DREGアクセスは
STATUS/MSG INを消費しません。

回帰試験は`tests/scsi_dreg_cdb_test.c`（6バイトCDB・10バイトCDBのDREG連続書き込み、
Set ACK/REQ経路の退行確認）。

### 割り込み

IOI経由でIPL1に配線済み（`docs/ix68k-internals/addrmap.md`の「割り込み系統」表参照）。
SPC Hard Error(INTS bit1)・Reset Condition等は`ints`ビット管理のみで、実際に
これらを踏むテストは未実施（TC Parity Error/Short Transfer Periodの発生条件
自体を作るソフトが無いと推測）。


## 変更履歴

| 日付 | 内容 |
|------|------|
| 2026-09-05 | センスデータをSCSI-2固定長18バイト（`SCSI_SENSE_LENGTH`）へ拡張し、`spc_set_sense()`で組み立てを一本化。ASC/ASCQ(バイト12-13)と追加センス長(バイト7)を設定するようにしました。Mode Sense(6)の未対応ページ（NetBSD sd(4)が要求するpage 4/5）はセンスを設定せずCHECK CONDITIONだけ返していたため、続くRequest Senseが全ゼロを返してきました。ILLEGAL REQUEST / ASC=0x24を設定するよう修正。回帰試験は`tests/scsi_status_test.c`の`test_mode_sense_unsupported_page_sets_illegal_request_sense` |
| 2026-09-05 | COMMANDフェーズのCDB長確定を`spc_store_out_byte()`へ共通化し、`SCMD`=Set ACK/REQ経路だけでなく`DREG`書き込み（プログラム転送/DMAC）経路からも通るようにしました。DREG経路の完了判定に`buf_index == buf_limit`を追加。NetBSD/x68kの`spc`ドライバはCDBをTransfer+DREG連続書き込みで送るため、従来は先頭1バイトしか受領できず`INTS.CC`が発火せず`scsibus0: waiting 2 seconds for devices to settle...`で停止してきました。回帰試験は`tests/scsi_dreg_cdb_test.c`。動作検証: NetBSD 10.1(GENERIC)がsd0〜sd7のプローブを完走し`root on sd0a`のマウントまで到達 |
| 2026-09-06 | DREG read/writeを`SSTS.TRIP && TC != 0`でゲートし、短いDATA IN応答の完了後に余剰DREGアクセスがSTATUS/MSG INを消費しないようにしました。SSTS.DF/DEを実データ残量で更新し、18バイトのREQUEST SENSEへ32バイトを要求する回帰試験で、18バイト時点のDFクリア・DEセット・TC=14維持および32回読出し後のSTATUS維持を確認します。 |
| 2026-09-04 | MO(リムーバブル)ライトプロテクトの自動判定・トグルAPIを実装（`x68k_bus_scsi_write_protected()`/`x68k_bus_scsi_set_write_protect()`）。`attach_hdd()`に`r+b`→`rb`フォールバックを追加し読み取り専用イメージの接続を可能にしました。GUI/MCP配線はこの時点では未着手 |
| 2026-09-03 | IOCS `_B_VERIFY`(0x41)のSCSI/SASI経路(PDA `$8n`)はSCAN EQUALを使わず、ROM(`$FF94C0`)がREAD(6)(0x08、CDB雛形`$FF9D0A`)を発行してから`cmpm.b`でCPU比較する設計と判明（1KiB単位、`sasi.c`の既存READ実装で足りるため追加実装は不要）。戻り値はFDD側(生ステータスロング)と異なり一致=`0`・不一致=`$FFFFFFFE`（ROM実測`$FF9514`の`moveq #$fe,d0`）。FDD側のSCAN仕様は`docs/ix68k-internals/design/fdc_design.md`を参照 |
| 2026-09-03 | 内蔵HDC(`sasi.c`、$E96000)のSELECT存在判定を、本ファイルのSPC($E96020、`scsi.c`)と**同一条件 `fp != NULL \|\| present`** に統一した（`units[]`は両者で共有する同じ配列なので、存在判定が2種類あってはならない）。これによりメディア未挿入の空リムーバブルも内蔵HDC経由のSELECTに応答し、未接続ID(`present=0`)は応答せずROM側のSELECTタイムアウトに委ねます。あわせて`sasi.c`にLUN(CDB[1] bit5-7)の解釈を追加した（LUN0のみ実装、それ以外はINVALID COMMAND）。**注意**: SASIの1バイトセンス(`$20` INVALID COMMAND / `$21` INVALID SECTOR ADDRESS)は、本ファイルのSPCが返す拡張センス(`[0]=0x70`, `[2]=センスキー`)とは**別形式**であり、互いに読み替えできません。詳細は`docs/ix68k-internals/support/iocs_support.md`の該当エントリ参照 |
| 2026-09-03 | Seek(6)(0x0B)を新規実装。範囲内はGOOD(no-op)、範囲外はILLEGAL REQUEST（XEiJ `scuDoSeek6`準拠）。メディア無し/未接続は既存のmedia guardに委ねる(0x0Bをexemptリストに追加しない)。**注意**: IOCS `_B_SEEK`(0x40)のSCSI/SASI側(PDA $80-$8F)は本ファイルのSPC($E96020)ではなく内蔵HDC($E96000、`sasi.c`)経由でROMがこのCDBを発行する（トレースで確認、詳細は`docs/ix68k-internals/support/iocs_support.md`の該当エントリ参照）。ここでのSeek(6)実装はSPCを直接叩く別経路向けとして意味を持つが、`_B_SEEK`自体の実装場所ではない |
| 2026-08-19 | CD-ROM(CloneCD)対応 P4/P5: CONFIG.SYS/x68000.cfgへのSUSIE.X常駐登録、MCP `disk()`docstring更新、`SUSIE.X.md`検証状態更新。全フェーズ完了 |
| 2026-08-19 | CD-ROM(CloneCD)対応 P3: READ TOC(0x43)を新規実装 |
| 2026-08-19 | CD-ROM(CloneCD)対応 P2: Inquiryのperipheral device type分岐、Mode SenseのWPビット、Format Unit/Write(6)/(10)のDATA PROTECT拒否を追加 |
| 2026-08-14 | Format Unit(0x04)をHuman68k FORMAT.X向けのno-op成功応答として実装。X68SCSI1ヘッダは保持し、後続の管理領域書き込みに委ねる |
| 2026-07-29 | レジスタ単位の存在監査で新規作成。レジスタマップを現行仕様と一致することを再確認。当時未実装だったFormat Unit(0x04)等を一覧化 |
