# MCP デバッグ参照資料

`/mcp-debug`（`.claude/commands/mcp-debug.md`）から分離した個別手順・一覧。
全文は読まず、`rg -n '^#'` で見出し位置を引いて必要な節だけ offset/limit で読む。

## 利用可能なツール一覧

| ツール | 説明 |
|--------|------|
| `screenshot` | 画面キャプチャ（PNG）を取得 |
| `read_bitmap_text` | TVRAM のテキスト内容を取得（`@` はカーソルブリンカーのアーティファクト） |
| `read_stdout_text` | メモリ上の文字列を読み出し |
| `read_memory` | 任意アドレスから最大 256 バイトを hex ダンプで読み出し |
| `write_memory` | 任意アドレスにバイト列を書き込む（最大 256 バイト） |
| `key` | キー入力を送信（`action="press"/"down"/"up"`） |
| `paste_text` | 文字列をキー入力として流し込む |
| `run_command` | Human68k コマンドを実行して出力を返す |
| `disk` | FD/SCSIディスクイメージの挿入・排出（`action="insert"/"eject"`、`media="fd"/"scsi"`） |
| `get_registers` | CPU レジスタを取得 |
| `get_mmu_regs` | PMMU/access-controlレジスタ(TC/CRP/SRP/TT0/TT1/MMUSR)を取得。68000/68010/68020は非搭載 |
| `send_interrupt` | NMI（レベル7割り込み）を発生させ実行中プログラムを中断 |
| `whiteband` | 白帯検出による停止の管理（`action="on"/"off"/"continue"`。停止は `STOP=WHITEBAND` として `debug_control` からも見え、そちらの `continue` でも再開できる） |
| `reset` | エミュレータをソフトリセット（SRAM は保持）。`force_fd0=True`でOPT.1押下相当（FD0起動を強制） |
| `power_switch` | 前面電源スイッチ操作（`on=False`でOFF→IPLROMの電源断処理でix68kが終了。COMMAND.Xプロンプトで実施） |
| `mouse_move` | マウスを相対移動（SCC ch B 経由） |
| `mouse_click` | マウスボタンをクリック |
| `joystick` | ジョイスティック#1/#2の状態を管理（`action="set"/"release"`、離すまで保持） |
| `hostfile` | macOS↔Human68k間のファイル転送（`action="put"/"get"/"status"`） |
| `trace_iocs` | IOCS/TRAP 呼び出しの全トレースを stderr ログへ出力 |
| `key_wait` | CPU のキー入力待ち検出（`action="wait"`でブロック、`"check"`で即時確認。旧`is_waiting_for_key`/`wait_key_input`を統合） |
| `read_text_events` | IOCS `B_PUTC`/`B_PRINT`とテキストプローブの出力を非停止で取得（サーバ側に直近1MiB保持） |
| `wait_for` | テキスト出力にパターンが現れるまで待つ（正規表現可。`login:`待ち等が1回で完結） |
| `text_probe` | プロファイル駆動の非停止テキスト出力プローブ管理（`action="status"/"clear"/"enable"`）。HIOCS.X / IOCS.X 常駐時の出力採取点は自動で有効になる（`text_auto_probe` 設定、`MCP_TOOLS.md` を参照） |
| `ram_watch` | 指定範囲へのRAM書き込みをstderrログへ記録（書き込み元PCが分かる。値変化での停止は`watchpoint`） |
| `disassemble` | 指定アドレスから非侵襲に逆アセンブル（最大64命令） |
| `breakpoint` | アドレス比較型ブレークポイント管理（`action="add"/"wait"/"remove"/"enable"/"clear"/"list"`、条件式・パスカウント対応） |
| `watchpoint` | 値変化検出型ウォッチポイント管理（`action=...`、書き込みで実際にCPUを停止） |
| `exception_break` | 例外ブレークマスクの設定・取得（指定ベクタの例外発生時にCPUを停止） |
| `debug_control` | デバッグ実行制御（`action="status"/"continue"/"step"/"break"/"mode"`） |
| `dump_ring` | PC/レジスタ/コール/フォールト履歴リングのダンプ取得（`kind="pcring"/"regring"/"callring"/"faultring"`） |
| `diag_log` | 診断ログ・トレース記録のON/OFF（`kind="badjump"/"badmovea"/"badaddr"/"badori"/"pcring"/"regring"/"faultring"/"pctrace1024"`） |
| `performance_counter` | 仮想CPUサイクルの区間計測（`action="arm"/"status"/"clear"`、`start_pc`/`end_pc`。ホスト負荷に依存しない比較用。詳細は`tools-host/mcp/MCP_TOOLS.md`） |
| `snapshot` | 停止中のVM状態をプロセス内単一スロットへ保存・復元（`action="save"/"load"/"status"`）。媒体を変更するとスロットは無効。load成功時は `STOP=SNAPSHOT RINGS=CLEARED` で停止する。 |

## チェックポイント

`snapshot(action="save")` と `snapshot(action="load")` は停止中だけ使える。入力ペースト、
シリアル通信、FD0強制起動処理中は拒否される。保存後にFD/SCSI媒体を挿入・排出・書込み保護変更すると、
`snapshot(action="status")` は `SLOT=INVALID REASON=MEDIA_CHANGED` を返し、そのスロットはloadできない。
読み取り専用媒体、または `scsi_overlayN` を指定した固定HDDで使い、保存・復元の前に入力を止めること。
COW差分を使う場合、load後に見えるディスク内容は保存時点へ戻る。SRAMは終了時に、最後にloadした状態の
内容で書き出される。

## ドライブレター割り当て（起動デバイスで変わる）

Human68kは起動元デバイスによってドライブレターの並びが変わる（FD2台構成の場合）:

- **FD0から起動**: `A:`=FD0, `B:`=FD1, `C:`=HDD...
- **HDDから起動**: `A:`=HDD, `B:`=FD0, `C:`=FD1, `D:`=他のHDD...

`disk(action="insert", media="fd", index=1, ...)`（FD1）で挿入したディスクが、HDD起動時は`C:`に見える
（`B:`ではない）。`DIR B:`で「ディスクが入っていません」と出たら、まず`drive`コマンドで
現在のドライブ構成を確認すること（思い込みで決め打ちしない）。

### ドライブ変更前のメディア確認（白帯回避）

`<d:>`単体入力・`CD <d:>`・`DIR <d:>`のような明示的なドライブ切り替えに限らない。
**コマンド文字列中のどこかにドライブレター付きパスが1箇所でも含まれていれば対象**
（例: `A:\BIN\ED.X`をそのまま`run_command`で実行、`TYPE B:\README.TXT`、
`COPY A:\X.TXT C:\Y.TXT`）。空きドライブに触れるコマンドを1回でも打つと「ディスクが
入っていません」の白帯（エラー画面）が出て`A`/`R`での復帰操作が必要になる。これは
事後に`paste_text("A")`等で復帰できるが、毎回無駄な往復になるため、コマンド文字列に
登場する**すべての**ドライブレターについて、打つ**前**に以下を確認し、空なら先に
挿入してから進める:

```python
disk(action="status")   # FD0-3/SCSI各ユニットの実際のメディア挿入有無を確認
run_command("DRIVE")    # 現在どのドライブレターがどのユニットに対応するか確認
# 対応するユニットが空なら、切り替え・アクセス前にinsertする
disk(action="insert", media="fd", index=<空だった番号>, path="...")
```

## FD0からの強制起動テスト（OPT.1相当、SRAM起動デバイス設定を一時的に無視）

実機ではOPT.1キーを押しながら電源ON/リセットすると、SRAMの起動デバイス設定を無視して
標準順序（フロッピー→SASI/SCSI→ROM→SRAM）でデバイスが検索される（OPT.2は別機能＝
ROMデバッガ組み込み、SRAM `$ED0058`と連動。混同注意）。IPLROMXV逆アセンブル
（`$FF01E6`付近、リセットベクタ`$FF0010`から追跡）で裏付け済み:

```
move.w ($00ed0018),d1        ; SRAM起動デバイス設定を読む
beq    probe_all             ; 未設定なら標準探索
btst.b #$02,($0000080e).l    ; システムワーク$80E bit2（OPT.1相当フラグ）
bne    probe_all             ; 立っていればSRAM設定を無視して標準探索へ
...(以下、SRAM設定に従って個別デバイスへ分岐)
```

`reset(force_fd0=True)`で同等の操作ができる（下記の`$80E`直接書き込みは、その仕組みの
裏付けと`force_fd0`が使えない場合の代替手順）。

MCPの`key`は`action="press"`単発では1回の呼び出しで押下→離上まで完結してしまう
（`hold_ms`で押下時間を模擬する方法もあるが）。ここではより確実な方法として、
リセット**前**に`write_memory`で`$80E`のbit2を直接立てておく方式で
代用する（`reset()`は`bus_reset_peripheral()`を呼ぶが、これはIOI/SCSI/SASI/SCC/ADPCM/OPM等
周辺デバイス状態のみをクリアし、一般RAM（`$80E`を含む）には触れないため、リセット前に
書いた値がそのまま起動シーケンスに読まれる。ブレークポイントで正確なタイミングを
狙うより確実で、対象アドレスに到達する前に競合するリスクがない）。

```python
# 1. FD0にテスト対象イメージを挿入（既に何か入っていれば先にeject必要）
disk(action="eject", media="fd", index=0)
disk(action="insert", media="fd", index=0, path="/path/to/test.XDF_or_2HD")

# 2. リセット前に$80E(10進2062) bit2を立てる = OPT.1押下相当
write_memory(address=2062, data=[4])

# 3. リセット（SRAMの起動デバイス設定=SCSI0等があっても無視されFD0優先になる）
reset()

# 4. 起動後は必ずbit2を戻す（OPT.1を離す相当）。刺したままだと次回reset()にも
#    影響が残ってしまう（一般RAMはreset()でクリアされないため）。
write_memory(address=2062, data=[0])
```

`key_wait`はB_KEYINPポーリングでない起動直後のプログラム（ゲーム等がB_KEYSNSで
ポーリングする場合）には反応せずタイムアウトすることがある（ツールの既知の制限）。
起動確認は`screenshot`/`read_bitmap_text`で`A>`（FD0起動）か`C>`（SCSI起動）かを見る方が確実。

## PC履歴リング（PCRING）

PC履歴は通常実行のホットパス負荷を避けるため既定OFF。暴走や白帯を調査する前に有効化する。

```bash
./build/ix68k -pcring true
```

```python
diag_log(kind="pcring", enable=True)
dump_ring(kind="pcring", count=64)
diag_log(kind="pcring", enable=False)
```

`X68K_WHITEBAND=1`、`-badjump-log true`、`-badori-log true`、実行時の
`whiteband(action="on")`、`diag_log(kind="badjump")`、`diag_log(kind="badori")`は、
診断前の履歴が必要なためPCRINGも有効化する。複数機能で共有されるため、それらの
OFFでPCRINGは自動停止しない。停止するには明示的に`diag_log(kind="pcring",
enable=False)`を使う。

`-badaddr-log true` または `diag_log(kind="badaddr", enable=True)` は、word/long
アクセスの上位8bitを調べるX68000固有の診断を有効化する。アクセスごとに大量出力され
うるため調査時だけ使う。リングダンプを行わないのでPCRINGは変更しない。

**`exception_break`の実機での注意**: ROM領域には`write_memory`で書き込んでも
反映されない（ROM保護が正しく機能している証拠でもある）ため、意図的にILLEGAL
命令を実行させて例外停止を再現するのは容易ではない（PCを直接書き換えるAPIは
無い）。暴走発生時に自然に機能することを期待する用途と割り切り、機能自体の
単体検証は`extern/cmc68000`側のCTest（`test_debug_run`等）に委ねる。

## レジスタトレース（REGRING）

CPU の全レジスタを 512 命令分（直近のみ、リングバッファ）記録し、BADJUMP 時にダンプ（既定オフ）:

```bash
# 起動時に有効化
./build/ix68k -regring true
```

```python
# 実行中に切替（MCP ツール経由）
diag_log(kind="regring", enable=True)
diag_log(kind="regring", enable=False)
```

API: `mc68k_set_reg_ring(cpu, 1/0)` / `mc68k_dump_reg_ring(cpu, count)`

## フォールト履歴リング（FAULTRING）

group0例外（バスエラー/アドレスエラー）1回ごとに、PC・フォールトした論理アドレス・
SSW・PMMU変換情報（CRP/SRP/TC、ATC/テーブルウォーク段数等）を記録する（既定オフ、
最大32件）。MMU環境（NetBSD/x68k等）でデマンド
ページングのフォールトを調べるとき、ゲストカーネルのバスエラーハンドラ内部に
BPを張らずに済む（従来は手動でBPが必要だった）。

起動時オプションは無く、MCPからの有効化のみ:

```python
diag_log(kind="faultring", enable=True)
# ... フォールトを起こす操作（例: NetBSDのページアウト誘発） ...
dump_ring(kind="faultring", count=32)
diag_log(kind="faultring", enable=False)
```

MMU環境でSIGSEGV相当のバスエラーが出て原因不明のときは、まずこのリングを見る
（フォールトしたアドレス・アクセス種別・PMMUの変換失敗理由が一度に分かる）。

API: `mc68k_set_fault_ring(cpu, 1/0)` / `mc68k_dump_fault_ring(cpu, count)`。
出力フォーマットとフィールドの意味は `extern/cmc68000/docs/mc68000-internals/design/fault_ring.md`
§4.7 参照。MMUレジスタ（TC/CRP/SRP等）単体のスナップショット取得は `get_mmu_regs`
（フォールト発生時点の値ではなく、呼び出し時点の現在値）。

## PC周期トレース（PCTRACE1024）

REGRINGが「直前数百命令の詳細」を見る道具なのに対し、こちらは「数百万命令規模の
実行傾向（どこに滞留しているか）」を見る道具。1024命令ごとにPCをstderrへ出力し
続ける（既定オフ、区間は無制限）。mint.x調査でXEiJとの制御フロー分岐点
特定に使用、82%対0%という定量的な滞留差を検出できた。

```bash
# 起動時に有効化
./build/ix68k -pctrace1024 true
```

```python
# 実行中に切替（MCP ツール経由）
diag_log(kind="pctrace1024", enable=True)
diag_log(kind="pctrace1024", enable=False)
```

出力形式: `[PCTRACE1024] n=<通算命令数> pc=<PC(hex)>`。特定範囲（例: 調査対象プログラム
のロード範囲）に絞って比較したいときは、出力を`grep`するかログをPythonで集計する
（`off=PC-ロードベース`を計算して比率を出す等）。

API: `mc68k_set_pctrace1024(cpu, 1/0)`

## キーコード

X68000 キーコード: `kcode << 8 | char`

| キー | コード |
|------|--------|
| RETURN | 0x1D0D |
| ESC | 0x011B |
| UP | 0x3C00 |
| DOWN | 0x3E00 |
| LEFT | 0x3B00 |
| RIGHT | 0x3D00 |
| F1-F10 | 0x6300-0x6C00 |

`paste_text("\\x1A")` は Ctrl+Z として送られる。`COPY CON` の終了を含む
Ctrl+英字の入力は、`paste_text` に制御文字を含めるか、`key(action="down",
key="CTRL")` と `key(action="press", key="A"-"Z")` を組み合わせる。

