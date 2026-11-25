# ix68k-debugger ユーザーズマニュアル

バージョン 1.0（`ix68k_debugger.py --version` で表示。ウィンドウのタイトルにも出ます）。

`ix68k-debugger` は、ix68k の MCP ソケットに接続して CPU の状態を観察・操作する外部デバッグモニタです。レジスタ、逆アセンブル、メモリ、ブレークポイント（BP）、ウォッチポイント（WP）、実行履歴（ring）を 1 つのウィンドウで関連付けて見られます。

ix68k 本体に GUI は追加されません。デバッガは別プロセスとして動き、Claude Code やスクリプトと同じ MCP ソケットを共用します。

```text
ix68k ── MCP ソケット ── ix68k-debugger
                      └─ Claude Code / スクリプト
```

## 1. 準備

### 1.1 動作条件

| 項目 | 内容 |
| --- | --- |
| Python | Python 3（Tk 付き）。追加の pip パッケージは不要 |
| Tk | Python の Tkinter が必要。macOS（Homebrew）は `brew install python-tk`、Debian/Ubuntu は `sudo apt install python3-tk` |
| ix68k | 起動済みであること。MCP サーバは ix68k 起動時に自動で待ち受けを始める |

動作確認の状況は macOS のみ（ix68k 起動中の GUI 手動確認まで）です。Linux と Windows は未検証です。

### 1.2 起動

先に ix68k を起動し、別のターミナルでデバッガを起動します。

```bash
# ターミナル 1: ix68k
/tmp/x68000-build/ix68k -rom rom/IPLROM.DAT -cgrom rom/CGROM.DAT

# ターミナル 2: デバッガ
python3 tools-host/debugger/ix68k_debugger.py
```

**メニューから起動する**: ix68k の `File > Open Debugger...` を選ぶと、デバッガが別プロセスで起動します（macOS・Windows・Linux）。すでに起動している間は、もう1つ起動することはありません（終了すれば再度起動できます）。この項目は開発ツリー向けです。起動スクリプトを、カレントディレクトリ基準で探すため、リポジトリルートで ix68k を起動してください（`local/run_*.sh` は自動で `cd` します）。見つからないときは項目が灰色になります。別の場所のスクリプトを使うときは、環境変数 `IX68K_DEBUGGER_LAUNCHER` にパスを指定します。接続先は、ix68k の `IX68K_MCP_SOCK`（Windows は `IX68K_MCP_PORT`）がそのまま引き継がれます。

| OS | 起動スクリプト | 動作確認 |
| --- | --- | --- |
| macOS / Linux | `tools-host/debugger/launch_debugger.sh` | macOS のみ動作確認。Linux はビルドの構文チェックまで |
| Windows | `tools-host\debugger\launch_debugger.cmd` | 未確認（ビルド・実行とも未実施） |

Windows は、コンソール窓を出さずに起動するため、スクリプトのエラーメッセージ（tkinter のある Python が無い等）は見えません。原因を調べるときは、コマンドプロンプトから `tools-host\debugger\launch_debugger.cmd` を直接実行してください。

**tkinter のある Python を自動で選ぶ**: 起動スクリプトは、`python3` が tkinter なしの版を指す環境でも動くように、`import tkinter` が通る Python を探します。使う Python を決めたいときは `IX68K_PYTHON=/path/to/python` を指定します。ターミナルからもスクリプトを直接実行できます。

- `launch_debugger.sh`: `python3`、`python3.15` … `python3.10`、`/usr/bin/python3` の順に試し、通った最初のものを使います。
- `launch_debugger.cmd`: `py -3`、`python` の順に試し、通った最初のものを使います。

接続先は次のとおりです。

| OS | 既定 | 環境変数で変更 | 起動オプション |
| --- | --- | --- | --- |
| macOS / Linux | `/tmp/ix68k_mcp.sock`（UNIX ソケット） | `IX68K_MCP_SOCK` | `--socket PATH` |
| Windows | `127.0.0.1:26800`（TCP） | `IX68K_MCP_PORT` | `--port PORT` |

`--socket` と `--port` は同時に指定できません。ix68k を複数起動している場合は、デバッガにも同じ接続先を指定してください。

起動直後のデバッガは**接続しません**。`Refresh ON` を押すまで、ix68k へは 1 バイトも送信しません。

## 2. 画面構成

```text
┌──────────────────────────────────────────────────────────────┐
│ [Refresh ON][Refresh OFF] [Run][Break][Step][Step×10] [Snapshot Save][Load][Status]  [調査終了] │ ← 操作バー
│ 状態表示 / 通知 / CPU debug mode・Capture / Requests            │ ← 情報欄
├────────────────────┬─────────────────────────────────────────┤
│ Registers/stopinfo │ Disassembly                             │
│ Breakpoints/       │ Memory                                  │
│  Watchpoints       │ Trace (CALLRING/PCRING/REGRING/Raw)     │
│ Diagnostics        │                                         │
└────────────────────┴─────────────────────────────────────────┘
```

### 2.1 情報欄

| 行 | 内容 |
| --- | --- |
| 1 行目 | `<状態> / Refresh: ON/OFF`。通信エラーがあれば末尾に表示 |
| 2 行目 | 通知。操作の結果（`BP ADD ...: OK` など）や警告を表示 |
| 3 行目 | `CPU debug mode: <MODE> (<EXEC>)    Capture: <CAP>` |
| 4 行目 | `Requests: <累計> total, <直近 1 秒> / sec` |

状態の意味は次のとおりです。

| 状態 | 意味 |
| --- | --- |
| `OFF` | 接続していない（Refresh OFF） |
| `FETCHING` | ix68k から情報を取得中 |
| `DISPLAYING` | 停止中の情報を表示している |
| `RUNNING_POLL` | ゲストが実行中。状態だけを定期確認している |
| `BREAK_POLL` | Break を要求して、停止を待っている |

`CPU debug mode` の `MODE` は `AUTO` / `ON` / `OFF`、`EXEC` は `NORMAL`（通常の高速実行）か `DEBUG`（命令単位の低速経路）です。BP / WP / 例外マスクを使っている間は `EXEC=DEBUG` になり、ix68k が遅くなります。`Capture` は PCRING / REGRING が実際に記録されているかを表します（`NONE` / `PC` / `REG` / `PC,REG`）。性能に影響する設定が残っていないかは、ここで確認できます。

## 3. 基本の使い方

1. ix68k を起動し、デバッガを起動します。
2. `Refresh ON` を押します。接続して現在の状態を取得し、以後 250ms 間隔で更新します。
3. ゲストが実行中なら `Break` で停止します。停止すると全ペインが埋まる。
4. `Step` で 1 命令ずつ進める、`Run` で再開する、などを行います。
5. 調べ終わったら `Refresh OFF` で通信を止めます。必要なら `調査終了` で設定を元に戻します。

`Refresh OFF` の間、デバッガは ix68k へ通信しません。このため、デバッガを開いておくだけで ix68k が遅くなることはありません。ウィンドウを最小化したときも自動的に `Refresh OFF` になります。

## 4. 実行制御

| ボタン | キー | 動作 | 使える状態 |
| --- | --- | --- | --- |
| Run | F5 | 実行を再開（`DBG CONT`） | 停止中 |
| Break | Shift+F5 | 実行中のゲストを停止（`DBG BREAK`） | 実行中 |
| Step | F10 | 1 命令実行して停止（`DBG STEP 1`） | 停止中 |
| Step ×10 | F11 | 10 命令実行して停止（`DBG STEP 10`） | 停止中 |

使えない状態のボタンは灰色になり、キーを押しても何も送信されません。

- **Break**: ix68k は次の実行チャンクの境目で停止します。要求直後の応答が「実行中」でも失敗ではありません。デバッガが 100ms ごとに確認し、停止すると表示が切り替わります。5 秒待っても停止しない場合は `Break pending or unavailable` と通知し、実行中の表示へ戻ります。
- **Step**: BP / WP / 例外が途中で命中した場合は、その停止理由が優先されます。
- **macOS の F11**: 既定で「デスクトップを表示」に割り当てられているため、そのままでは届きません。Shift+F11 を使うか、システム設定のキーボードショートカットで割り当てを外してください。

### 4.1 スナップショット

停止中の仮想マシン状態を、ix68k プロセス内の単一スロットへ保存・復元します。Claude Code の `snapshot` ツールと同じスロットを共有します。

| ボタン | 動作 | 使える状態 |
| --- | --- | --- |
| Snapshot Save | 現在の状態をスロットへ保存（`SNAPSHOT SAVE`） | 停止中 |
| Snapshot Load | スロットの状態へ復元（`SNAPSHOT LOAD`）。確認ダイアログを出す | 停止中 |
| Snapshot Status | スロットの状態を照会（`SNAPSHOT STATUS`）。結果は通知欄に出る | 常時 |

- **Load 後**: ゲストは `STOP=SNAPSHOT` で停止したままです。履歴リング（CALLRING / PCRING / REGRING）は空になり、通知欄に表示されます。実行を続けるには Run を押します。
- **保存できない構成**: 次の場合、`SNAPSHOT SAVE` / `SNAPSHOT LOAD` は `ERR NOTREADY EXTERNAL_RESOURCE` で拒否されます。どれが原因かは、エラーメッセージからは分かりません。
  - 書き込み可能な SCSI ディスクに `scsi_overlayN` を指定していません。
  - HOSTDRV が有効（設定の `hostdrv_dir` が空でない）。
  - 書き込み可能な FD を挿入しています。
  - HOSTDRV の転送中、ADPCM の録音中、入力のペースト中、シリアル通信中。
- **ディスク付きで使う**: 設定に `scsi_overlayN = <新規の .cow ファイル>` を足し、`hostdrv_dir` を空にして起動します。ゲストの書き込みは `.cow` に入り、元の HDS は変わりません。`.cow` はディスクごとに新規作成し、複数のプロセスで共有しないでください。Load すると、ディスクの内容も保存時点に戻ります（`docs/ix68k-internals/boot.md` の「SCSI HDDブート」）。
- **スロットが無効になる場合**: 保存後にディスク媒体を操作すると、スロットは無効になります（Status が `SLOT=INVALID REASON=MEDIA_CHANGED`）。
- 要求の待ち時間は、他の操作（0.5 秒）より長い 10 秒です。

## 5. 各ペインの説明

### 5.1 Registers / stopinfo

停止時の `DBG STATUS`（停止理由・`MODE`・`CAP`・`EXEC` など）と `REGS`（D0〜D7、A0〜A7、PC、SR、USP、SSP）、`MMU GET`（CPU モデルと MMU レジスタ。68030 は TC/CRP/SRP/TT0/TT1/MMUSR、68040/060 は URP/SRP/TC/DTT/ITT。68000/68010/68020 は `MMU=UNSUPPORTED`）を表示します。横にスクロールしなくて済むよう、応答を分解して縦に並べます（1行は最大で約47桁）。

```text
STATE=paused   STOP=USER          ← 状態と停止理由（BP=・WP=・RINGS= など未知のフィールドもここに出る）
MODE=AUTO      CAP=NONE           ← 動作モード（MODE / CAP / EXEC / WB）
EXEC=NORMAL    WB=OFF

PC=00059762    SR=2010
  T=00 S=1 M=0 IPL=0              ← SR の分解（トレース・特権・マスタ・割り込みレベル）
  XNZVC=10000                     ← SR の CCR（X N Z V C の順）
D0=00000000 D1=00000000 D2=00000002 D3=00000200   ← D0〜D7 と A0〜A7 は4つずつ
D4=000BA800 D5=00000000 D6=00000000 D7=00000005
A0=0008D76C A1=000B98F2 A2=0008E126 A3=0008DA78
A4=0008D377 A5=000BA2EA A6=000BA2EC A7=000082D0
USP=000B3408   SSP=00008364

MMU  MODEL=68060                  ← MMU GET（機種で項目が違う）
URP=00BD8000   SRP=00BD8000
TC=0000C210
DTT0=00000000  DTT1=00000000
ITT0=00000000  ITT1=00000000
```

- 実行中は `DBG STATUS` だけなので、状態と動作モードの行だけになります。
- レジスタ以外の項目は 2 列に並べます。`STOP=BREAKPOINT` のように長い項目や、68030 の `CRP` / `SRP`（16 桁）は、2 列に並べず 1 行で出します。
- `ERR` 応答は、生の 1 行で出します。
- 想定外の長い行が出ても折り返すので、横スクロールは出ません。

停止理由（`STOP=`）は次のいずれかです。

| STOP | 意味 |
| --- | --- |
| `USER` | `Break` による停止 |
| `STEP` | `Step` の完了 |
| `BREAKPOINT` | BP に命中（`BP=<id>`） |
| `WATCHPOINT` | WP に命中（`WP=<id> WPADDR= OLD= NEW=`） |
| `TRACE` | トレース例外 |
| `EXCEPTION` | 例外による停止（`VEC=<ベクタ番号>`） |
| `HALTED` | CPU が HALT 状態 |
| `WHITEBAND` | 白帯（Human68k のエラー表示）検出による停止 |
| `SNAPSHOT` | `SNAPSHOT LOAD` の直後（履歴リングは空。`RINGS=CLEARED`） |

ix68k の WP は、書き込み命令が完了した直後（次の命令の PC）で停止します。

### 5.2 Disassembly

停止中の PC から前方 32 命令を表示します。現在の PC の行は黄色で強調されます。68000 は可変長命令のため、PC より前を正確に逆アセンブルする機能はありません。

### 5.3 Memory

アドレス入力欄の位置から 256 バイトを、16 バイト × 16 行の 16 進 + ASCII で表示します。停止中は更新のたびに再取得します。

| 操作 | 内容 |
| --- | --- |
| アドレス入力欄 | `1000`、`$1000`、`0x1000` のいずれも可。右のラジオボタンで選んだ幅（24bit は `FFFFFF`、32bit は `FFFFFFFF`）まで |
| 24bit / 32bit | アドレスの表示幅（6 桁 / 8 桁）と入力範囲を切り替えます。初期値は初回の Refresh 時に `MMU GET` の `MODEL=` で決める（68000/68010 は 24bit、68020 以降は 32bit。取得できなければ 24bit）。手動で選んだ後は自動判定で上書きしません。Trace のアドレスは本体の出力どおり 6〜8 桁を読む |
| Go（Ctrl+G） | 入力したアドレスへ移動 |
| PC / A7 / USP / SSP | 該当レジスタの値へ移動（Ctrl+L は PC へ戻る） |
| バイトをクリック | そのアドレスを入力欄へ反映し、下段に `byte` / `word` / `long`（ビッグエンディアン）を表示。以後の更新でも選択は保たれる |
| Read once | `MEM READ` で 1 回だけ読む |
| SUPER | `MEM PEEK ... SUPER` で、現在の SR によらずスーパーバイザプログラム空間として読みます。ユーザーモード中でもカーネルのテキストを見られます。チェックすると表示位置を読み直し、以後の自動更新にも適用される（`MEM READ` には効かない） |

通常の表示は副作用のない `MEM PEEK` です。`MEM READ` は実際のバス読み出しなので、I/O レジスタなど**読み出しに副作用のある領域**ではデバイスの状態が変わることがあります。副作用を許容できるときだけ、明示的に押してください（自動更新には使われません）。ix68k の起動直後・リセット直後は `ERR NOTREADY` や `ERR NOCPU` が表示されることがあります。しばらくして再度更新されれば解消します。

メモリの書き換えはできません。

### 5.4 Breakpoints / Watchpoints

BP / WP の一覧を `BP` / `WP` タブで切り替えます。

| 列 | 内容 |
| --- | --- |
| id | 識別番号（10 進） |
| addr | アドレス |
| en | 有効（1）/ 無効（0） |
| hits | 命中回数 |
| pass | BP の pass count（指定回数だけ無視してから停止） |
| cond | BP の条件式 |
| size / last | WP のサイズと、最後に命中した値 |

| ボタン | 動作 |
| --- | --- |
| Add BP（Ctrl+B） | アドレス入力欄のアドレスに BP を追加。条件式（空欄で無条件）と pass count（0 以上）を入力する |
| Delete BP | 選択した BP を削除 |
| Toggle BP | 選択した BP の有効/無効を切り替え |
| Add WP | アドレス入力欄のアドレスに WP を追加。サイズは 1 / 2 / 4 |
| Delete WP | 選択した WP を削除 |

- BP のアドレスは**偶数**のみ有効です（奇数は送信前に拒否されます）。WP のサイズは 1 / 2 / 4 のみです。
- 一覧をクリックしてフォーカスがあるとき、Delete キー（macOS では delete = BackSpace）で選択項目を削除できます。アドレス入力欄の編集とは干渉しません。
- BP / WP に命中して停止すると、該当する行と Disassembly の PC 行が自動で選択されます。
- ゲスト実行中は一覧を再取得しません（最後に取得した内容を表示し続けます）。BP / WP の操作や `調査終了` の直後だけ、1 回取り直します。他のクライアントが変更した内容は、次に停止したときに反映されます。
- 一覧の宣言件数と解析できた件数が違うと、通知欄に警告が出ます（ix68k 側の応答バッファが切り詰められた場合）。

### 5.5 Diagnostics

ix68k の診断機能のチェックボックスです。チェックすると `DIAG <名前> ON`、外すと `OFF` を送信します。

| 項目 | 内容 |
| --- | --- |
| BADJUMP / BADMOVEA / BADADDR / BADORI | 異常なジャンプ・不正なアドレス・不正命令の検出 |
| PCRING | PCRING（実行した命令アドレスの履歴）を記録 |
| REGRING | REGRING（命令実行直前のレジスタ値の履歴）を記録 |

チェックボックスはデバッガ自身の操作を表すだけで、実際の状態ではありません。実効値は情報欄の `Capture` で確認してください。`BADJUMP` / `BADORI` / `WHITEBAND` を ON にすると、PCRING も自動的に有効になります。これらを OFF にしても PCRING は元に戻らないため、不要なら `PCRING` を OFF にしてください。

PCRING / REGRING の記録は ix68k の実行速度に影響します。必要なときだけ ON にしてください。

### 5.6 Trace

停止中に、直近 200 件の ring を表示します。

| タブ | 内容 | 取得条件 |
| --- | --- | --- |
| CALLRING | 関数の呼び出し（CALL）と復帰（RET） | 常時 |
| PCRING | 実行した命令アドレスの時系列 | `PCRING` が ON のとき |
| REGRING | 各命令の直前のレジスタ値 | `REGRING` が ON のとき |
| Raw | 上記の ring 本文の全文 | — |

- CALLRING / PCRING / REGRING のタブでは、6 桁の 16 進アドレスをクリックすると Memory ペインがそのアドレスへ移動します。形式を確認できた行のアドレスだけがクリックできます。解釈できない行も Raw タブには本文がそのまま残ります。
- **Freeze trace**: 停止中の stopinfo・レジスタ・逆アセンブル・ring をスナップショットとして保持します。再開しても上書きされません。停止中の全ペインが揃ってから押してください。
- **Export trace**: Freeze した内容をテキストファイルへ保存します。

## 6. 調査終了

`調査終了` ボタンは、調査用に変更した ix68k の設定をまとめて元に戻します。押すと次のコマンドを列挙した確認ダイアログが出て、OK を選んだときだけ順に実行されます。

```text
BP CLEAR
WP CLEAR
EXC SET 0
WATCH OFF
DIAG PCRING OFF
DIAG REGRING OFF
DIAG BADORI OFF
WHITEBAND OFF
DBG MODE AUTO
```

BP / WP を全削除し、例外マスクと診断機能を解除して、CPU debug mode を `AUTO` に戻します。**他の MCP クライアント（Claude Code など）が設定した内容も消えます**。BP や WP が残っていると、ゲストは低速な命令単位の経路で動き続けます。調査を終えたら実行してください。

## 7. Claude Code・スクリプトとの併用

ix68k の MCP サーバは 1 度に 1 つのクライアントしか処理しません。デバッガは常時接続せず、更新のたびに接続し、必要な要求をまとめて送信して応答を受けたら切断します。そのため、デバッガを開いたまま Claude Code から `REGS` や `MEM PEEK` を交互に使えます。

- 別のクライアントが `DBG WAIT` や `KEY WAIT` で待機している間（最長 25 秒）は、ix68k がデバッガの要求に応答できません。このとき情報欄に `MCP busy — another client may be waiting` と表示され、デバッガは待機間隔を 100ms → 250ms → 500ms → 1 秒と延ばしながら再試行します。待機が終われば自動で復帰します。
- デバッガ自身は `DBG WAIT` / `KEY WAIT` を送信しません。
- Refresh ON の間は、停止中に 1 周期あたり 7〜9 要求（PCRING・REGRING が有効なとき各 1 要求を追加）を送信します（実行中は `DBG STATUS` の 1 要求のみ）。送信数は情報欄の `Requests` で確認できます。

## 8. トラブルシューティング

| 症状 | 原因と対処 |
| --- | --- |
| 起動時に `Tkinter が必要です` と出る | Tk 未導入。「1.1 動作条件」の手順で導入する |
| `Disconnected` 系のエラーが出て `OFF` になる | ix68k が起動していない、または接続先が違います。ix68k の起動と `IX68K_MCP_SOCK`（Windows は `IX68K_MCP_PORT`）を確認し、`Refresh ON` を押し直します。接続エラーでは自動再接続しない |
| `MCP busy — another client may be waiting` が続く | 他のクライアントが待機コマンドを実行中。待機が終わるまで待つ（自動回復） |
| `Protocol error` や PROTO の不一致が出る | ix68k とデバッガのバージョンが合っていません。ix68k を再ビルドして起動し直す |
| `Break pending or unavailable` | 5 秒以内に停止しませんでした。ゲストが長い命令列を実行中か、MCP が応答していません。再度 Break を押す |
| Run / Step が灰色のまま | ゲストが実行中（Break で停止する）、または Refresh OFF。`Refresh ON` を押す |
| ix68k が遅い | BP / WP / 例外マスクや ring の記録が残っています。情報欄の `EXEC` と `Capture` を確認し、不要なら `調査終了` |
| F11 が効かない（macOS） | 「4. 実行制御」を参照。Shift+F11 を使う |
| メモリ表示が `ERR NOTREADY` / `ERR NOCPU` | ix68k の起動直後。次の更新で解消する |

## 9. キーボードショートカット一覧

| キー | 動作 |
| --- | --- |
| F5 | Run |
| Shift+F5 | Break |
| F10 | Step |
| F11 | Step ×10 |
| Ctrl+B | アドレス入力欄のアドレスへ BP 追加 |
| Delete / BackSpace | 選択した BP / WP を削除（一覧にフォーカスがあるときのみ） |
| Ctrl+G | アドレス入力欄のアドレスへ Memory 移動 |
| Ctrl+L | PC へ Memory 移動 |
