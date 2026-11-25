# MCP デバッグワークフロー

エミュレータ実行中に MCP ソケット経由で状態を確認・操作する実践ガイド（正本）。

## 1. このガイドの目的と対象範囲

症状から「どの手段で観測し、どこで止め、何を採取するか」を選ぶためのガイド。

- 引数・戻り値などのAPI仕様は `tools-host/mcp/MCP_TOOLS.md` を正本とし、ここでは再掲しない。各節は「どの症状でこれを選ぶか／選ばないか」に限る。
- MCPプロトコル・デバッガ内部設計は `tools-host/mcp/MCP_TOOLS.md` の「内部プロトコル」節（ローカル専用の設計書 `local/design/design_mcp_debugger.md` は git 管理外）を正本とする。
- ツール一覧表・ドライブレター・FD0強制起動・各リングの詳細は `docs/ix68k-internals/mcp_debug_reference.md`（`rg -n '^#'` で見出しを引き、必要な節だけ読む）。
- 人間が画面で追う外部デバッガ `ix68k-debugger`（Tk GUI、Issue #186）は §6.5 にまとめる。操作は `tools-host/debugger/MANUAL.md` が正本。
- 接続確認・多重起動・Windows差分は末尾「運用注記」にまとめる。

## 2. 最初に守る3原則

1. **まず非侵襲に観測し、再現手順を固定してから停止・詳細観測へ進む。** 画面・レジスタ・メモリ読み出し・ログで足りるうちはCPUを止めない。再現操作列（キー入力・コマンド・ディスク状態）を先に確定する。
2. **メモリはまず非侵襲 `MEM PEEK` で読む。** `read_memory` の既定（`side_effects=False`）が `PEEK_MEM`。`side_effects=True`（`READ_MEM`）はI/Oレジスタのread-clear等を発生させ、FDC結果FIFOなど内部状態を壊す。副作用そのものを調べたい場合だけ使う。
3. **ROM番地ではなく、現在の実行実体を追う。** Human68k常駐ドライバはIOCSベクタ（`$400+4*fn`）と `$BC`（TRAP#15）をRAMへ差し替える。IOCSのBPは `breakpoint(iocs=<fn>)`、または `read_memory(0x400+4*fn, 4)` で生ベクタを解決した番地に張る（§8.1）。

## 3. デバッグ機能の早見表

| やりたいこと | 手段 | 向く／向かない |
|---|---|---|
| 任意の時点で止めて状態を見る | `debug_control(action="break")` → `status` | 症状が出ている最中に使う。応答は要求時点の状態で、実際に停止したかは `status` で確認 |
| 特定アドレスまで進めて止める（単発） | `breakpoint(action="wait", address=...)` | ヒット後もpausedのまま。再開は `debug_control(continue)` |
| 複数箇所を仕込む／条件付きで止める | `breakpoint(action="add", condition=..., pass_count=...)` | 通過するだけのアドレスも取りこぼさない。条件は `&&` のAND結合のみ |
| IOCSコールで止める | `breakpoint(iocs=<fn>)` | ROM番地直指定は不可（§2 原則3） |
| 値が変わった瞬間で止める | `watchpoint(action="add")` | 値変化のエッジ検出。同値の書き戻しは検出しない |
| 誰が書いたか（書込元PC）を知る | `ram_watch` | **メインRAMのみ**（I/O・GVRAMは不可。I/Oは `watchpoint` か書き込み命令へのBP、§4.4）。stderrログのみでCPUは止めない。監視は1範囲 |
| バスエラー・ILLEGAL等で止める | `exception_break(vectors=[2,3,4,8])` | Line-F(11)・TRAP(32-47)はOS動作で頻発するため含めない |
| 白帯（エラーダイアログ）発生時に止める | `whiteband(action="on")` | 既定OFF、再起動でOFFに戻る。armed確認は `WB=ON` |
| 暴走直前の実行経路を見る | `diag_log(pcring)` → `dump_ring(kind="pc")` | 記録のON/OFFとダンプは別操作。既定OFF |
| 暴走直前のレジスタ推移を見る | `diag_log(regring)` → `dump_ring(kind="reg")` | 同上 |
| 呼び出し経路を見る | `dump_ring(kind="call")` | BSR/JSR/RTSの簡易バックトレース。常時記録 |
| MMU環境のバスエラー（SIGSEGV相当）の原因 | `diag_log(faultring)` → 再現 → `dump_ring(kind="fault")` | PC・論理アドレス・SSW・変換失敗理由が一度に出る。BPを張る前にまずこれ |
| IOCS呼び出しの頻度・順序を見る | `trace_iocs` | stderrへ全出力。量が多いので頻度集計に向く。`_ABORTRST`（白帯・ディスクエラー時に発行）で自動OFFになる |
| 不正ジャンプ・不正ポインタ・ゼロ領域暴走 | `diag_log(badjump / badmovea / badaddr / badori)` | `badaddr` は大量出力しうる |
| 画面上のテキストを読む | `read_bitmap_text` / `read_stdout_text` / `read_text_events` | §6.3 |
| 画面の見た目を見る | `screenshot(layers=...)` | §6.3 |
| 起動中のSRAM確認・修正 | `read_memory(0xED0000, 32)` / `write_memory` | 未起動時は `tools-host/sram_switch`（`sram.dat` を直接読み書き） |
| MMUレジスタ確認 | `get_mmu_regs` | 68000/68010/68020はMMU非搭載 |
| 白帯・停止の最終手段の中断 | `send_interrupt`（NMI） | 乱用しない（§7.2） |

## 4. 症状別レシピ

各レシピは「最初の観測 → 絞り込み → 採取」の順。共通の前提は §5、後片付けは §7。

### 4.1 白帯ダイアログが出る・消える

1. `whiteband(action="on")` → 再現操作を `run_command(...)` で実行（`paste_text` は積むだけなので直後の `status` は必ず running）。
2. `debug_control(action="status")` が `STOP=WHITEBAND` なら停止済み（whiteband ON でPCRINGも自動で有効になり、`dump_ring(kind="pc")` が使える。`trace_iocs` は白帯検出で自動OFFになるため、続けるなら再度有効化する）。§5.3 の採取を行う。停止時は直前のIOCS呼び出し履歴がエミュレータのログへダンプされる。
3. 白帯の文言は `read_bitmap_text` で読める（反転表示は `_B_PUTMES` の引数から復元される。画面に残っている間のみ）。
4. ドライブ指定を含む操作なら、先に `disk(action="status")` と `run_command("DRIVE")` で対象ユニットが空でないか確認する。
5. ディスクI/O起因が疑われるときは §4.3 と §8.2（エミュレータのバグと決める前にイメージを疑う）。

### 4.2 キー入力やマウス入力が効かない

1. `debug_control(action="status")` で停止・暴走していないか確認（`STATE=running` か、PCが同じ範囲を回っていないか `get_registers` を数回）。
2. 入力待ちループにいるかは `key_wait` と `get_registers`、必要なら `diag_log(pcring)` で確認。`key_wait` は `_B_KEYSNS` でポーリングするだけのプログラムには反応しない。
3. IOCSのキー入力（`_B_KEYSNS` 等）の呼ばれ方は `trace_iocs` で見る。
4. 割り込み（VBLANK/MFP）が止まっている疑いは §4.4 を併用する。
5. 入力投入の手段を変えて切り分ける: `key` / `paste_text` / `run_command`。

### 4.3 ディスクI/Oで止まる・リトライする

1. `disk(action="status")` と `run_command("DRIVE")` でユニット・ドライブレターの前提を固定する（ドライブレターは起動デバイスで変わる。詳細は参照資料）。
2. `trace_iocs` で `_B_DRVCHK` 等の呼び出し頻度を集計し、「無限リトライ」が実際の振動かを数で確認する。`_ABORTRST`（白帯）が出るとトレースは自動OFFになるので、集計を続けるなら再度有効化する。
3. 読み込み層の正当性は、ホスト側のイメージとのバイト比較で切り分ける。
4. ディスクイメージ側（LFNエントリ混入など）を先に疑う（§8.2）。
5. ファイルを開いたままの `reset()` は見かけ上のディスクエラーを起こす（#140/#141）。調査中のリセットはファイルを閉じてから。

### 4.4 音が鳴らない・テンポがおかしい・割り込みが来ない

割り込み（VBLANK/MFP）由来のハング・入力不能も本節で扱う（§4.2、§4.7 から相互参照）。

1. ドライバ／ソフトが期待するレジスタに書いているか、確認する。I/Oレジスタは `watchpoint`（peekによる値変化検出。OPMデータレジスタ等の書き込み専用レジスタは変化を検出できない場合がある）か、書き込み命令へのBPで調べる。`ram_watch`（書込元PC）はメインRAM上のワーク／バッファ限定で、I/Oへの書き込みは記録されない。
2. 割り込みベクタの飛び先を `read_memory` で確認し、`breakpoint(add)` でハンドラ入口に張り、`debug_control(continue, wait=True)` で到達の有無（1回止まるか）を確認する。`hits` は停止した回数で、黙って数えるカウンタではない（停止すると割り込みのタイミング自体が変わる）。回数そのものは取れない。傾向は `diag_log(pctrace1024)`（1024命令ごとのサンプリング）で見る。ハンドラが IOCS を呼ぶ場合に限り `trace_iocs` で集計できる。
3. 実装状態は `/hw-impl`（FDC/MFP等）を参照する。

### 4.5 画面が崩れる・一部レイヤーが見えない

1. `screenshot(layers="display")` で実表示を、`layers="tvram"`/`"gvram"`/`"sprite"` でレイヤー別に確認する（§6.3）。
2. 非表示レイヤーは黒が返る。生データは停止して `read_memory` で読む。
3. レイヤーのON/OFFレジスタ（TON/SON/DISP/GS等）を `watchpoint` で監視し、変化を検出してから撮る。
4. 実装状態は `/arch`、`/addrmap` を参照する。

### 4.6 常駐ドライバ導入後に挙動が変わる

1. IOCSベクタ（`$400+4*fn`）と `$BC` が差し替わっていないか `read_memory` で確認する。
2. IOCSのBPは `breakpoint(iocs=<fn>)`（§8.1）。ROM番地のBPが `hits=0` のままなら差し替えを疑う。
3. 差し替え後のフォントで `read_bitmap_text` が文字を誤認する場合は、`tools-x68k/fntadr/` の `FNTADR.X` をゲストで実行して実フォントアドレスを報告させる（#157。報告はリセットで失われるので、リセット後は再実行する）。
4. 切り分けのため、独立した第2インスタンス（headless・独立ソケット・独立ディスク）で再現するのも有効（#156。標準配布の HUMAN302.XDF も CONFIG.SYS で IOCS.X 等を常駐させるため、第2インスタンスでもベクタは差し替わっている前提で `iocs=` を使う。手順は `tools-host/mcp/MCP_TOOLS.md` の「複数インスタンスの起動」）。

### 4.7 OSやソフトが無限ループする・暴走する

1. 先に `diag_log(pcring)` を有効化して再現する（既定OFF）。
2. 停止: `debug_control(action="break")` → `status`。止まらないときは §7.2 のチェックリスト。
3. `dump_ring(kind="pc")`・`dump_ring(kind="call")`・`get_registers`・`disassemble` で経路を特定する。
4. ゼロ埋め領域への暴走は `diag_log(badori)`、不正ジャンプは `badjump`/`badmovea`。
5. `exception_break(vectors=[2,3,4,8])` を併用すると例外の発生点で止まる。
6. MMU環境（NetBSD等）のSIGSEGV相当は `faultring` を先に見る。
7. 割り込み停止が疑われるなら §4.4。

## 5. 基本ワークフロー

### 5.1 症状を再現可能な操作列にする

- `disk(action="status")` → `run_command("DRIVE")` → ドライブレターで前提を確定する。
- 再現操作（キー・コマンド・ディスク挿入）を固定し、毎回同じ手順で起動する。起動デバイスやFD0強制起動は参照資料の該当節を使う。
- 起動デバイス判定などの前提は、画面の雰囲気でなく `DIR` のボリューム名・ドライブレターで一次確認する。

### 5.2 まず非侵襲に観測する

`screenshot` / `read_bitmap_text` → `get_registers` → `read_memory`（既定の `PEEK_MEM`）。この間CPUは止めない。

### 5.3 停止して状態を採取する

停止後は次を順に採取し、結果を記録する（§9）。

```python
debug_control(action="status")                 # STOP理由・PC・全レジスタ
get_registers()
disassemble(address=<PC>, count=8)             # PCから先を読む（可変長命令のため遡っての逆アセンブルは不正確。直前の経路は dump_ring で取る）
read_memory(<SP>, 32)                          # スタック
read_memory(<疑わしい番地>, 64)
screenshot(layers="display")                   # 画面
dump_ring(kind="call", count=32)               # 呼び出し経路（pc/reg/fault は事前に有効化済みなら）
```

### 5.4 仮説に応じてBP／WP／トレースを追加する

- 「ここを通るか」→ `breakpoint`、「いつ書き換わるか」→ `watchpoint`、「誰が書いたか（メインRAM）」→ `ram_watch`、「例外か」→ `exception_break`、「どこから来たか」→ リング。
- 推測でハンドラ内部へBPを張る前に、リングとログで経路を絞る。

### 5.5 修正後に同じ操作列で再検証する

§5.1 で固定した操作列を同じ手順で再実行し、症状が消えたこと・副作用が無いことを確認する。実装変更を伴う場合は `/build` の手順でビルド・テストも行う。

## 6. ツール別の使い分け

### 6.1 停止する

- **`debug_control(break)`**: 症状発生中に任意の時点で止める。
- **ブレークポイント**: 単発は `breakpoint(wait)`。ヒットしてもpausedのまま残り、自動再開しない（再開は `debug_control(continue)`）。複数仕込みは `add` → `continue(wait=True)`。条件・パスカウントを併用できる。
- **`breakpoint(iocs=...)`**: 常駐ドライバを越えてIOCS実体で止める。
- **ウォッチポイント**: 値の変化で止める（エッジ検出）。
- **例外ブレーク**: `[2,3,4,8]`（バスエラー/アドレスエラー/ILLEGAL/特権違反）が暴走調査の基本。
- **`debug_control(step, count=n)`**: 停止後に n 命令だけ進める。実行後は自動的に停止状態。
- **白帯停止**: §4.1。

```python
breakpoint(action="wait", address=0xFF93FC, timeout_ms=5000)
# 条件付き: breakpoint(action="wait", address=0xFF93FC, condition="D0==12345678")
breakpoint(action="add", iocs=0x2F)               # _B_PUTMES の実体
debug_control(action="continue", wait=True, timeout_ms=5000)
watchpoint(action="add", address=0x000807, size=1)
exception_break(action="set", vectors=[2, 3, 4, 8])
```

### 6.2 状態を壊さずに観察する

- **`MEM PEEK` と副作用付き読出し**: §2 原則2。探索的な読み出しは既定のまま。
- **レジスタ・逆アセンブル**: `get_registers` と `disassemble`（非侵襲、最大64命令）。停止時はPC付近とスタックを併せて読む。
- **`read_memory` の address は整数で渡す**（ガイド中の `0x` は可読性のための表記で、ツール呼び出しには整数値を渡す。hex は `int("FF0000", 16)` で変換）。

### 6.3 画面を分けて見る

- `screenshot(layers="display")` は表示状態そのもの。レイヤー別（`tvram`/`gvram`/`sprite`、`+`結合可）は各レイヤーのON/OFF状態を反映し、非表示なら黒。フレーム内の一瞬しか立たない可視性ビットは外すことがあるため、`watchpoint` で値変化を検出してから撮る。
- 白帯・TVRAMの描画は **TVRAM**（GVRAMではない）。
- `read_bitmap_text` はTVRAMをCGROM ANKフォントと照合する。**読めるもの**: ANK文字、反転表示（`_B_PUTMES` 引数から復元、画面に残る間のみ）。**読めないもの**: 漢字（`?`）、IOCS `$16` を差し替える常駐ドライバ下のフォント（誤認する。§4.6）。
- 文字列そのものを非停止で取りたいときは `read_text_events` / `text_probe`、プログラム標準出力の差分は `read_stdout_text`（呼ぶたびにバッファが消える）。`read_text_events` は既定で `streams`（probeごとの結合済みテキスト）を返すため、通常は `streams[*].text` を読む。個々のイベント情報が必要な調査だけ `detail="events"` を指定し、`fields` を絞る。続きだけ取得するときは前回の `next_seq` を `since_seq` に渡す。

### 6.4 履歴と診断ログ

- **PC／レジスタ／フォールトリング**: `diag_log(kind=...)` で記録を有効化（既定OFF）してから再現し、`dump_ring` で取得する。ON/OFFとダンプは別操作。`call` のみ常時記録。起動時オプションと詳細は参照資料の該当節。
- **IOCSトレース**: `trace_iocs`（stderrへ全出力）。頻度の集計に向く。`_ABORTRST` で自動OFFになる（§3）。
- **BADJUMP／BADMOVEA／BADADDR／BADORI**: 暴走の兆候をstderrへ出す。`badaddr` は大量出力しうる。
- **ログ量の制御**: 必要な間だけONにし、不要になったら必ずOFFにする（§7.1）。

### 6.5 外部デバッガ `ix68k-debugger`（Issue #186）を併用する

レジスタ・逆アセンブル・メモリ・BP/WPを人間が常時見たいときの GUI。Claude Code と同じ MCP ソケットに接続する（操作の詳細は `tools-host/debugger/MANUAL.md`、概要は `tools-host/debugger/README.md`）。

```bash
python3 tools-host/debugger/ix68k_debugger.py   # ix68k 起動後に別ターミナルで。起動直後は未接続、Refresh ON で取得開始
```

- **併用**: 更新は短命接続（1接続で同期・読み取り専用要求を送って切断）なので、Claude Code やスクリプトと交互に使える。別クライアントが `DBG WAIT` / `KEY WAIT` 中は GUI が busy 表示で再試行する。GUI自身は待機コマンドを送らない。
- **BP/WP/例外設定は共有状態**: GUI で張ったBPは Claude Code 側の `breakpoint(action="list")` にも見える。逆も同様。
- **GUI の `MEM READ` は副作用付き**: `Read once` を明示操作したときだけ使われる（§2 原則2と同じ考え方）。通常表示は `MEM PEEK`。
- **`調査終了`ボタン**: BP/WP/例外/`ram_watch`/PCRING・REGRING・BADORI/whiteband/実行モード(AUTO)を解除する（§7.1 の一部）。`trace_iocs`・BADJUMP・BADMOVEA・BADADDR・FAULTRING・PCTRACE1024 はOFFにしない。停止中CPUの再開（continue）もしない。他クライアントの設定も消えるため、実行コマンドの確認が出る。
- **既知の制約**: macOS では F11 が「デスクトップを表示」に取られて届かない（Shift+F11 を使う）。ix68k の WP は書き込み命令の完了直後（次命令のPC）で停止する。Linux/Windows は未検証。

## 7. 後片付けと禁忌

### 7.1 後片付け（次回セッションに残さない）

```python
breakpoint(action="clear")
watchpoint(action="clear")
exception_break(action="set", vectors=[])
whiteband(action="off")          # 白帯停止中なら先に continue
ram_watch(enable=False)
trace_iocs(enable=False)
diag_log(kind="badjump", enable=False)   # 有効化した kind ごと（badmovea/badaddr/badori/regring/faultring/pctrace1024）
diag_log(kind="pcring", enable=False)    # 他機能と共有。自動停止しないため明示的にOFFにする
debug_control(action="mode", mode="AUTO")
debug_control(action="continue", wait=False)   # pausedのまま放置しない
debug_control(action="status")                  # WB=OFF・CAP=NONE・EXEC=NORMAL を確認
text_probe(action="clear")                      # 使った場合
```

- `diag_log(pcring)` は他機能と共有されるため、他機能の終了で自動停止しない。
- `ix68k-debugger` の `調査終了` は一部だけ解除する（§6.5）: BP/WP/例外/`ram_watch`/PCRING・REGRING・BADORI/whiteband/実行モード。`trace_iocs`・BADJUMP・BADMOVEA・BADADDR・FAULTRING・PCTRACE1024 はOFFにしない。停止中CPUの再開（continue）もしない。

### 7.2 停止できないとき・やってはいけない操作

停止できないとき:
1. `debug_control(action="status")` の `WB=` でwhitebandがarmedか確認する。
2. 待ちは `run_command(...)` か `debug_control(continue, wait=True)` を使う（`paste_text` 直後の `status` は必ず running）。
3. IOCSのBPはROM番地でなく生ベクタ（`iocs=`）。

やってはいけない:
- `send_interrupt`（NMI）の乱用。
- ファイルを開いたままの `reset()`（見かけ上のディスクエラー、#140/#141）。
- ROMへの `write_memory`（無効）。
- 動作中のNetBSD等ゲストOSを、ゲスト側のシャットダウン無しに終了すること。

## 8. 実例

### 8.1 IOCSベクタ差し替えでROMのBPが命中しなかった例（Issue #156）

- 症状: `_B_PUTMES`（ROM `$00FF76DC`）にBPを張っても `hits=0` のまま。
- 真因: Human68k常駐ドライバ（`"@IOCS"` 等）が `$400+4*fn` と `$BC` をRAMへ差し替えており、ROM側エントリは実行されていなかった。
- 対処: `breakpoint(iocs=fn)`、または `read_memory(0x400+4*fn, 4)` で実体を確認して `address` に渡す。
- 再発防止: `WB=ON/OFF` を stopinfo に追加（whiteband のarmed状態を確認できるようにした）。`paste_text` 直後の status は必ず running という観測手順の落とし穴も判明。
- 一般性: HIOCS PLUS でも同じ構造（追加コメントで傍証）。

### 8.2 白帯の原因がエミュレータではなくディスクだった例（Issue #30）

- 症状: `DIR B:` で「無効なメディア」の白帯。当初は `_B_DRVCHK` mode8/9 の非収束とエミュレータ側の不具合を疑った（誤診）。
- 真因: macOSのFATマウント経由でXDFへ混入したVFATロングファイル名エントリ（属性 `$0F`、10個）。Human68kのファイル名変換がNULLを不正文字として弾き `$EE` を返していた。
- 誤診の判明: IOCS頻度集計で `_B_DRVCHK` は63回のみ、`_B_KEYSNS` が20761回と分かり、「無限振動」が見間違いと判明した。
- 教訓: エミュレータのバグと決める前にディスクイメージ側を疑う。読み込み層の正当性はイメージとのバイト比較で切る。対策はホストのFATマウント経由でイメージへコピーしないこと（XDF作成は mtools でなく `hu68disk`）。属性 `$0F` の10エントリを無効化した版で `DIR B:` が成功したことが決定的な証明になった。

### 8.3 白帯の文言キャプチャが画面クリア後も残っていた例（Issue #155）

- 症状: プロンプトに戻った後も、`run_command` / `read_bitmap_text` の戻り値に古い白帯の文言が混ざった。
- 真因: `_B_PUTMES` 引数のキャプチャ（反転表示を読む仕組み）の無効化が、同じ (row, col) への再描画でしか行われていなかった。
- 対処: TVRAMの現在内容とセル単位で突き合わせ、一致しないセルのキャプチャを失効させる。
- 教訓: テキスト取得の結果は `screenshot` で裏を取る。

## 9. 調査結果の記録テンプレート

Issueコメントや調査ログには次の4項目で記録する。

### 9.1 再現手順
- 起動オプション・ディスク構成（`disk(action="status")` の出力）・ドライブレター
- 操作列（キー／コマンド）

### 9.2 観測結果
- 停止理由（`STOP=`）・PC・主要レジスタ・逆アセンブル・スタック・画面（`screenshot` のパス）
- 有効化したリング／ログと、その抜粋

### 9.3 仮説と反証
- 仮説ごとに「確認した手段」と「結果（支持／反証）」を並べる。反証された仮説も残す。

### 9.4 修正と回帰確認
- 変更ファイル・コミット、§5.1 の操作列での再検証結果、実行したビルド・テスト

## 運用注記

### 前提（接続確認）

```bash
ls /tmp/ix68k_mcp.sock  # エミュレータ起動中なら存在する（既定パスの場合。環境変数
                         # IX68K_MCP_SOCKで変更している場合はそちらを確認する）
```

Windowsのみ AF_UNIX ではなく TCP ループバックで待ち受ける（Issue #185）:

```powershell
netstat -ano | findstr 26800  # 127.0.0.1:26800 が LISTENING なら起動中（既定ポートの
                                # 場合。環境変数 IX68K_MCP_PORT で変更している場合は
                                # そちらのポート番号を確認する）
```

### 注意

- MCP ソケットが切断している場合はエミュレータを再起動する。`ls /tmp/ix68k_mcp.sock` が無ければ
  意図しない多重起動で古いプロセスが取り残されている可能性がある（`pgrep -fl build/ix68k` で
  確認して `kill` する）。同じソケットパスで2台目を起動した場合は、新しいプロセス側が
  「MCP socket already in use」を出して自分の MCP サーバを起動しないガードが入っている
  （Issue #8、`src/mcp/server.c` の `mcp_socket_is_live()`）。
  **Windowsのみ** AF_UNIX ではなく TCP（`127.0.0.1:26800`）のため、この
  `mcp_socket_is_live()` ガードは通らない。同じポートで2台目を起動した場合は
  `bind()` 失敗がそのままシグナルになり、「ポートが既に使用中です」というログを
  出して2台目の MCP サーバは起動しない（Issue #185、`netstat -ano | findstr 26800`
  で確認できる）。
- 意図的に複数インスタンスを同時に動かしたい場合は、環境変数 `IX68K_MCP_SOCK` でインスタンスごとに
  別のソケットパスを指定する（Issue #144、詳細は `tools-host/mcp/MCP_TOOLS.md` の
  「複数インスタンスの起動」節）。**Windowsのみ** 環境変数 `IX68K_MCP_PORT` でポート番号を
  分ける（Issue #185、同ファイルの「Windows版」節）。
- `/tmp` はサンドボックスオーバーレイで見えないことがある → Bash の `dangerouslyDisableSandbox: true` が必要な場合あり。

### 参照資料（別ファイル）

個別手順・一覧は `docs/ix68k-internals/mcp_debug_reference.md` に分離している。全文は読まず、
`rg -n '^#'` で見出し位置を引き、必要な節だけ offset/limit で読む。

- 利用可能なツール一覧（表）
- ドライブレター割り当て／ドライブ変更前のメディア確認（白帯回避）
- FD0からの強制起動テスト（OPT.1相当）
- PC履歴リング（PCRING）／レジスタトレース（REGRING）／フォールト履歴リング（FAULTRING）／PC周期トレース（PCTRACE1024）
- キーコード

**ドライブレター付きパスを含むコマンドは、実行前に `disk(action="status")` と
`run_command("DRIVE")` で対象ユニットが空でないことを確認する**（空だと白帯が出て
`A`/`R` での復帰操作が要る）。
