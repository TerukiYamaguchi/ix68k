# X68000 エミュレータ MCP サーバー

Claude Code から X68000 エミュレータを操作するための MCP (Model Context Protocol) サーバー。

## セットアップ

エミュレータ本体を起動した後、Claude Code が自動的に MCP サーバーに接続します。

```bash
# エミュレータ起動（プロジェクトルートで）
./build/ix68k
```

`.mcp.json` が設定済みのため、Claude Code 起動時に `x68k-emulator` サーバーが自動接続されます。

---

## ツール一覧

### `breakpoint(action, address=0, condition="", pass_count=0, id=-1, enable=True, timeout_ms=5000, iocs=-1)`

アドレス比較型ブレークポイントの管理。実際にCPUを止め、
条件式・パスカウント・複数同時登録に対応する（通過するだけのアドレスも
取りこぼさない）。

> **IOCSルーチンにBPを張るときはROM逆アセンブルのアドレスをそのまま使わない**。
> Human68k は常駐ドライバ（`"@IOCS"` 等）で `$400+4*fn` のIOCS
> ベクタ表と `$BC`（TRAP#15）をRAMへ差し替えるため、ROM側のエントリ（例:
> `_B_PUTMES` = `$00FF76DC`）はそもそも実行されず、BPは `hits=0` のまま命中しません。
> `iocs=<fn>` でその場のベクタから実体アドレスを解決するか、
> `read_memory(0x400 + 4*fn, 4)` で実体アドレスを確認してから `address` に渡すこと。
>
> ```
> read_memory(0x4BC, 4)                  # IOCS $2F(_B_PUTMES) の現在の飛び先
> breakpoint(action="add", iocs=0x2F)    # 同じことをツール側で解決する
> ```

- `action`:
  - `"add"` — `address`にブレークポイントを追加します。`condition`で条件式
    （例: `"D0==12345678"`、`"[000807].b!=00"`、`&&`でAND結合のみ）、
    `pass_count`でパスカウント（最初のn回は無視）を指定できる（併用可、
    `"&& #n"`として合成される）。ヒットしてもCPUはpaused状態のまま残る。
    状態確認・再開は`debug_control()`を使う（複数箇所を仕込んで様子見
    したい場合に向く）。
  - `"wait"` — `address`に一時ブレークポイントを追加して即座に実行を再開し、
    到達（または`timeout_ms`経過）までブロックします。`condition`/
    `pass_count`も`"add"`と同様に指定できます。ヒットしてもCPUはpaused状態の
    まま残る（内部的には`BP ADD`→`DBG CONT`→`DBG WAIT`を合成しており、
    一時ブレークポイントはヒット後・タイムアウト後いずれも自動的に削除
    される）。単発で「ここまで進めて止めたい」用途に向きます。再開は`"add"`と
    同じく`debug_control()`を使います。
  - `"remove"` — `id`のブレークポイントを削除する
  - `"enable"` — `id`のブレークポイントの有効/無効を`enable`で切り替える
  - `"clear"` — 登録済み全ブレークポイントを削除する
  - `"list"` — 登録中のブレークポイント一覧を返す
- `address`: `action="add"`/`"wait"`で登録するアドレス（整数）
- `condition`: `action="add"`/`"wait"`の条件式（省略時は無条件）
- `pass_count`: `action="add"`/`"wait"`のパスカウント（省略時0=無視しない）
- `id`: `action="remove"`/`"enable"`で対象とするブレークポイントID
- `enable`: `action="enable"`で有効化するか（Falseで無効化）
- `timeout_ms`: `action="wait"`のタイムアウト（ミリ秒、最大25000）
- `iocs`: `action="add"`/`"wait"`でIOCSコール番号（0-255）を指定すると、その場の
  IOCSベクタ表`$400+4*fn`を読んで実体アドレスを解決し、`address`の代わりに使う
  （省略時-1は`address`をそのまま使う）
- 戻り値: `action="add"`は`"ID=<id>"`（`iocs`指定時は解決したアドレスを併記）、
  `"wait"`は`debug_control(action="continue")`
  と同じ`"STATE=paused STOP=... MODE=... CAP=... EXEC=... WB=... PC=... D0=...D7=... A0=...A7=... SR=... USP=...
  SSP=..."`形式、`"list"`は`"id=0,addr=...,en=...,hits=...,cond=...;..."`形式の
  一覧文字列、それ以外は簡潔な確認メッセージ

---

### `debug_control(action, count=1, wait=True, timeout_ms=5000, mode="AUTO")`

デバッガの実行制御（ツール数抑制のため`action`引数でグルーピング）。

- `action`:
  - `"status"` — 現在の状態を返す
  - `"continue"` — 停止を解除し実行を再開します。`wait=True`なら次の停止まで
    ブロックしてその結果を返す（`wait=False`なら即座に返る）
  - `"step"` — `count`命令だけ同期的に実行する（実行後は自動的に停止状態になる）
  - `"break"` — 次のチャンク境界での停止を要求する（応答は要求時点の状態を
    即座に返します。実際に停止したかは`action="status"`/`"continue"`で確認する）
  - `"mode"` — `mode`で指定した実行経路に切り替える
- `count`: `action="step"`で実行する命令数（デフォルト 1）
- `wait`: `action="continue"`で次の停止まで待つか（デフォルト True）
- `timeout_ms`: `wait=True`時の最大待機ミリ秒（デフォルト 5000、最大 25000）
- `mode`: `action="mode"`で指定する実行経路。`AUTO`(既定。ブレークポイント/
  ウォッチポイント登録時のみ低速なデバッグ実行経路を使う) / `ON`(常にデバッグ実行
  経路を使う) / `OFF`(常に通常実行を強制する)
- 戻り値: `STATE=running MODE=... CAP=... EXEC=... WB=...` または
  `STATE=paused STOP=... MODE=... CAP=... EXEC=... WB=... PC=... D0=...D7=... A0=...A7=... SR=... USP=... SSP=...`
  （`action="mode"`のみ `DBG_MODE=<mode>` を返す）。`WB`は白帯検出HALTのarmed状態
  `ON`/`OFF`（`whiteband(action="on"/"off")`）

---

### `diag_log(kind, enable=True)`

診断ログ・トレース記録のON/OFF。

- `kind`:
  - `"badjump"` — 上位8bitが`$00`/`$FF`以外へのジャンプを検出時にstderrへ出力
  - `"badmovea"` — 上位8bitが`$04`-`$FE`の不正ポインタ値をAnに積んだ操作を検出時にstderrへ出力
  - `"badaddr"` — word/longアクセスの上位8bitが`$00`/`$FF`以外ならstderrへ出力するX68000固有の24-bitアドレス診断。アクセスごとに大量出力されうる。PCリングは有効化しない
  - `"badori"` — opcode `0x0000`（`ORI.B #0,D0`、ゼロ埋め領域への暴走で頻出）の実行を検出時にstderrへ出力。実行結果は変えない（挙動を変えたい場合は起動時`-ori-illegal true`で同opcodeをILLEGAL例外化する、本kindとは独立の別機能）
  - `"pcring"` — PC履歴リングバッファへの記録を有効化する（`dump_ring(kind="pcring")`で中身を取得する前提。記録のON/OFFとダンプ取得は別操作）
  - `"regring"` — レジスタ履歴リングバッファへの記録を有効化する（`dump_ring(kind="regring")`で中身を取得する前提。記録のON/OFFとダンプ取得は別操作）
  - `"faultring"` — フォールト（group0例外）履歴リングバッファへの記録を有効化する（`dump_ring(kind="faultring")`で中身を取得する前提。記録のON/OFFとダンプ取得は別操作）
  - `"pctrace1024"` — 1024命令ごとのPCをstderrへ周期出力する（実行傾向の把握用）
- `enable`: `True`で有効化、`False`で無効化。いずれも既定オフ（正常起動でも大量に出るタイトルがありノイズになるため）。

---

### `disassemble(address, count=1)`

指定アドレスから命令を非侵襲に逆アセンブルする（最大 64 命令）。

- `address`: 開始アドレス（整数）
- `count`: 逆アセンブルする命令数（デフォルト 1、最大 64）
- 戻り値: `アドレス  生バイト(16bitワード単位)  ニーモニック` 形式の複数行文字列

```
disassemble(0xFF0000, 8)
```

---

### `disk(action, media="fd", index=0, path="")`

FD/SCSIディスクイメージの挿入・排出・ライトプロテクトを管理します。

- `action`:
  - `"insert"` — `media`で指定したユニットに`path`のイメージを挿入する（`path`必須）
  - `"eject"` — `media`で指定したユニットからイメージを排出する（`path`は指定しない）
  - `"status"` — 現在装着中のイメージ一覧を返す（`media`/`index`/`path`は無視）。セッションを跨いで「今どのドライブに何が入っているか」を確認するのに使います。ライトプロテクト中のFD/SCSI(MO)は末尾に「（ライトプロテクト）」が付く
  - `"protect"` — `media`で指定したユニットのディスクをライトプロテクトします。ホストのイメージファイルのパーミッション（owner writeビット）を実際に変更する（実機のプロテクトノッチ操作に相当）。`fd`: 全ドライブ対応。`scsi`: リムーバブルのMO（デバイスパラメータ`$0F`(EJ)`!=0`のHDD/MOイメージ、CD-ROM除く）のみ対応。固定HDD・CD-ROMは`ERR BADARG`
  - `"unprotect"` — 同ユニットのライトプロテクトを解除する
- `media`: `"fd"`（フロッピー、`index` 0–3）または `"scsi"`（SASI/SCSI、`index` 0–6）
- `index`: ユニット番号（既定0）
- `path`: `action="insert"`時のイメージファイル絶対パス

FD: 実機同様、対象ドライブに既にディスクが入っている場合は先に`action="eject"`で排出しないと失敗する（排出なしで差し替えるとHuman68kがメディア交換を検知できずFAT/ディレクトリキャッシュが古いままになるため）。eject は実機の `$E94005` イジェクト相当で、Human68k には抜去の抜き挿し割り込みが通知されます。

SCSI: デバイスパラメータ `$0F`(EJ)`!=0`（`hu68disk mkfs`の`--removable`）で作成したリムーバブルメディア(MO等)イメージは、既に起動済みのドライブに対してもreset無しでメディア交換/切断として即座に認識される（Unit Attention対応）。`EJ=0`の固定ディスク相当イメージは実機同様reset無しでは反映されません。

ライトプロテクト: WPはユニット単位のフラグで、挿入時にホストのファイル書き込み権限から自動判定される（非永続化、排出でクリア）。`protect`/`unprotect`はホストのファイルの実パーミッションを変更するため、同一ファイルを複数ドライブ/SCSI IDへ挿入している場合は挿入中の全ユニットへ効果が及びます。アクセス中（FDCの転送中、またはSCSI選択中）は`ERR NOTREADY`で拒否されます。SCSI側は固定HDD・CD-ROMへの`protect`/`unprotect`が`ERR BADARG`になる点がFDと異なります。

```
disk(action="insert", media="fd", index=0, path="/path/to/disk.xdf")
disk(action="eject", media="fd", index=0)
disk(action="insert", media="scsi", index=0, path="/path/to/disk.hds")
disk(action="protect", media="fd", index=0)
disk(action="protect", media="scsi", index=1)  # MOのみ対応。固定HDD/CD-ROMはERR BADARG
disk(action="status")  # -> "FD0: /path/to/disk.xdf（ライトプロテクト）" ...
disk(action="unprotect", media="fd", index=0)
```

---

### `dump_ring(kind, count=32)`

PC/レジスタ/コール/フォールトの履歴リングバッファをダンプします。暴走直前の実行履歴解析、MMUデマンドページングのフォールト解析用。

- `kind`:
  - `"pcring"` — PCRING。PC履歴（記録自体は既定オフのため、事前に`diag_log(kind="pcring")`で有効化が必要）
  - `"regring"` — REGRING。レジスタ履歴（記録自体は既定オフのため、事前に`diag_log(kind="regring")`で有効化が必要）
  - `"callring"` — CALLRING。BSR/JSR/RTSの呼び出し・復帰履歴（常時記録）
  - `"faultring"` — FAULTRING。group0例外（バスエラー/アドレスエラー）の履歴。PC/フォールトした論理アドレス/SSW/PMMU変換情報を記録（記録自体は既定オフのため、事前に`diag_log(kind="faultring")`で有効化が必要）
- `count`: ダンプする件数（0で全件、既定32）
- 戻り値: ダンプテキスト（古い順）

---

### `exception_break(vectors=None, action="set")`

例外ブレークマスクの設定・取得。`mc68k_debug_run()`経由の
実行中、指定した例外ベクタが発生した時点でCPUを停止します。`Line-F(11)`や
`TRAP(32-47)`はX68000のOS/API呼び出し機構として頻繁に使われるため、安易に
含めると通常のOS動作中に止まり続ける点に注意（既定は全ビット0=無効。暴走の
兆候に絞るなら例えばバスエラー(2)・アドレスエラー(3)・ILLEGAL(4)・特権違反(8)）。

- `action`:
  - `"set"` — `vectors`で指定したベクタ番号(0-47)の集合をマスクとして設定する
    （`vectors=[]`または`None`で全解除）
  - `"get"` — 現在設定されているベクタ番号のリストを返す（`vectors`は無視）
- `vectors`: `action="set"`で有効化する例外ベクタ番号のリスト（例: `[2,3,4,8]`）
- 戻り値: 現在有効なベクタ番号のリストを表す文字列

---

### `get_registers()`

CPU レジスタ（D0–D7, A0–A7, PC, SR, USP, SSP）を取得します。

```
D0=00000001  D1=00000000  ...
A0=00001234  A1=00002000  ...
PC=00032418  SR=2700  USP=00001000  SSP=00002000
```

---

### `get_mmu_regs()`

PMMU / access-control レジスタ（TC/CRP/SRP/TT0/TT1/MMUSR）を取得します。

- 68030: 全フィールドをそのまま返します。CRP/SRP は64bitのまま（下位32bitに切り詰めない）。
  ```
  MODEL=68030 TC=80F04445 CRP=8000000012340000 SRP=0000000000000000 TT0=00000000 TT1=00000000 MMUSR=0000
  ```
- 68EC030: TT0/TT1/MMUSR がハードウェア上 AC0/AC1/ACUSR に相当するため、その名前で返します。
  常に0の契約の TC/CRP/SRP は出しません。
  ```
  MODEL=68EC030 AC0=00000000 AC1=00000000 ACUSR=0000
  ```
- 68000/68010/68020: PMMU非搭載のため対象外を明示する（0を返すと有効な値と区別できなくなるため）。
  ```
  MODEL=68000 MMU=UNSUPPORTED
  ```

---

### `kbd_state()`

MFP USART TX（UDR書き込み）でキーボードへ送られたコマンドの保持状態を返します。`_LEDCTRL`/`_KEYDLY`/`_KEYREP`/`_KEY_INIT` の送信が実際に届いたかの確認用。

- 戻り値: `LED=<hex> BRIGHT=<hex> DELAY=<hex> RATE=<hex> TX=<送信バイト累計>`
  - `LED`: bit=1で点灯（bit0かな 1ローマ字 2コード入力 3CAPS 4INS 5ひらがな 6全角）
  - `DELAY`/`RATE`: コマンド下位4bitの生値（ms換算しない）。`TX`はMFPリセットでも消えない累計。

---

### `hostfile(action, macos_path="")`

macOS と Human68k 間で HOSTFILE MMIO 裏口経由のファイル転送を行います。

GET/PUT は send/scp 等と同じく「自分から見た自分の動作」を表す（put=自分から送り出す、get=自分が受け取る）。

- `action`:
  - `"put"` — `macos_path`のファイルを Human68k 側へ送る（`macos_path`必須）。この後 Human68k 側で `HOSTFILE GET <出力ファイル名>` を実行すると実ドライブにファイルが作られる（エミュレータ再起動不要）。ストリーミング方式のため転送サイズに上限はありません。
  - `"get"` — 68k側が `HOSTFILE PUT <入力ファイル名>` で送ったファイルを `macos_path` へ受け取る（`macos_path`必須）。先に Human68k 側で `HOSTFILE PUT` を実行しておくこと。
  - `"status"` — 転送のハンドシェイク状態を取得する（`macos_path`は無視）。put/getが相手側の操作待ちで進まないときの確認用。戻り値: `"state=<0-2> (<日本語ラベル>), len=<バイト数>"`（state: 0=空、1=GET待ち、2=PUT待ち）
- `macos_path`: `action="put"/"get"`で使うmacOS側ファイルの絶対パス

---

### `key_wait(action="wait", timeout_ms=5000)`

CPU が IOCS `B_KEYINP`(fn=0) でキー入力待ち（キューが空）かを調べます。

COMMAND.X 等がプロンプトで次の入力を待っている瞬間を検出でき、固定 `time.sleep` より速く確実に `paste_text`/`key(action="press")` を送れる（早すぎる送信による文字欠落や、待ちすぎによる時間浪費を防ぐ）。`B_KEYINP` / `B_KEYSNS` は、TRAP入口だけでなくIOCSベクタ先を直接実行するROM待機ループも即時に検出するため、従来のフレーム単位ポーリング判定を待ちません。

- `action`:
  - `"wait"` — キー入力待ちになるまでブロックする（既定）。既にキー入力待ちなら即座に返る。`timeout_ms`を過ぎたら例外
  - `"check"` — 今その状態かを即座に返す（ノンブロッキング、`timeout_ms`は無視）
- `timeout_ms`: `action="wait"`の最大待機ミリ秒（デフォルト 5000、最大 25000）
- 戻り値: `"WAITING=1"` / `"WAITING=0"`（`"WAITING=0"` は `action="check"` のみ返りうる）

> 注: IOCSベクタ先がROM以外へ差し替えられたプログラムは、従来のフレーム単位ヒューリスティックへフォールバックします。

---

### `joystick(action, index, up=False, down=False, left=False, right=False, trigger_a=False, trigger_b=False)`

ジョイスティックの状態をMCPから注入・解除します。

- `action`:
  - `"set"` — 状態を注入する（実コントローラより優先）。次に`action="release"`を呼ぶまで押しっぱなし状態が維持される（レベル入力のため、`mouse_click`のような自動離上はしない）。`up`/`down`/`left`/`right`/`trigger_a`/`trigger_b`を全て`False`にしてもMCP注入自体は解除されない（実コントローラへは戻らない）。解除は必ず`action="release"`を使うこと。
  - `"release"` — MCP注入を解除し、実コントローラの読み取りへ戻す（`up`以下の引数は無視）。
- `index`: `0`=ジョイスティック#1、`1`=ジョイスティック#2。
- `up`/`down`/`left`/`right`: 方向。`trigger_a`/`trigger_b`: トリガボタンA/B（`action="set"`のみ使用）。

---

### `key(action, key, hold_ms=0)`

X68000 の特殊キー入力を送信します。

キー名は大文字・小文字不問。対応キー: `F1`–`F10`, `XF1`–`XF5`, `HELP`, `HOME`, `INSERT`, `DELETE`, `ROLLUP`, `ROLLDOWN`, `UNDO`, `BREAK`, `COPY`, `UP`, `DOWN`, `LEFT`, `RIGHT`, `RETURN`, `ENTER`, `ESCAPE`, `BACKSPACE`, `TAB`, `SPACE`, `SHIFT`, `CTRL`, `OPT1`, `OPT2`, `KANA`, `ZENEI` 等。

- `action`:
  - `"press"` — 1回押します。`hold_ms`が既定の0なら押下→離上を間を置かず送る
    （メニュー操作等の単純タップ用）。0より大きい値を指定すると
    `"down"`→待機→`"up"`の3手順に切り替わり、実際の指の押下時間を模擬できます。
  - `"down"` — 押下する（離上は送らない、`"up"`で別途送ること。呼び出し側が
    必ず`"up"`を呼ぶ責任を持つ）。修飾キー(SHIFT等)を押しっぱなしにして
    別キーと組み合わせたい場合に使います。
  - `"up"` — `"down"`で押下したキーを離します。
- `hold_ms`: `action="press"`でのみ使う（down/upでは無視）。

```
key(action="press", key="F1")
key(action="press", key="RETURN")
```

---

### `mouse_click(button="left")`

マウスボタンをクリックする（押下→50ms待機→離上）。

ROMは約60Hzでボタン状態をポーリングするため、間に待機を挟んで確実に押下状態を1回はサンプリングさせる（キーボード`paste_text`のペーシングと同じ理由）。

- `button`: `"left"` または `"right"`。

---

### `mouse_move(dx, dy)`

マウスを相対移動させる（SCC ch B経由）。

ROMが約60Hzでポーリングするタイミングで消費されるため、即時反映ではありません。

- `dx`: X方向の相対移動量（右が正）。
- `dy`: Y方向の相対移動量（下が正）。

---

### `paste_text(text, wait="iocs")`

テキストをキーボード入力として注入します。

- UTF-8 → Shift-JIS 変換済み
- `\n` で改行（リテラル `\n` も実際の改行も両方受け付ける）
- Shift-JIS で表現できない文字は `?` に置換
- `\x01`–`\x1A` のうち BS/TAB/LF/CR を除く制御文字は、対応する
  `Ctrl+A`–`Ctrl+Z` として送信する（例: `\x1A` = Ctrl+Z）。
- 1行は改行込みで512バイトまで。超えると送信前に `ValueError` にします。
- `wait` は各行を送った後の待ち方:
  - `"iocs"`（既定）: IOCS のキー入力待ちに安定して戻るまで待つ（Human68k 向け）。
    IOCS を使わないゲスト（NetBSD 等）では毎回タイムアウトし、複数行は1行目で止まる。
    エラーには停止した行番号が入る。
  - `"none"`: IOCS の入力待ちは確認せず、PASTE キューの送出完了と、ゲストが
    MFP USART RX を読み切ったこと（`KEY PASTE`）だけを確認します。NetBSD 等向け。
    保証するのは「ゲストに届いた」ことまでで、前のコマンドの完了や、入力を捨てる
    プログラム（パスワード入力等）が受け取ったことは保証しません。MFP USART RX FIFO
    （32バイト）の溢れで破棄されたバイトがあれば戻り値に件数を書きます。

```
paste_text("echo HELLO\n")
paste_text("dir\n")
paste_text("COPY CON TEST.TXT\nhello\n\x1A\n")  # Ctrl+ZでCOPY CONを終了
```

`key`でも英字キー `A`–`Z` を指定できるため、修飾キーを保持した入力も可能:

```python
key(action="down", key="CTRL")
try:
    key(action="press", key="C")
finally:
    key(action="up", key="CTRL")
```

> **注意**: コマンド実行には `run_command` の使用を推奨。`paste_text` は Enter 送信後の応答待ちを行いません。

---

### `ram_watch(address=0, length=1, enable=True)`

指定範囲へのRAM書き込みをエミュレータのstderrログへ記録する（書き込みアドレス・旧値・新値・PCを`[RAMWATCH]`行で出力）。デバッグ調査用、ビルドし直さずON/OFF切り替え可能。同時に監視できるのは1範囲のみ（再度`enable=True`で呼ぶと範囲を上書き）。

- `address`: 監視開始アドレス（例: `0x000807`）
- `length`: 監視バイト長（デフォルト1）
- `enable=False`で監視を無効化（`address`/`length`は無視）

---

### `performance_counter(action="status", start_pc=0, end_pc=0)`

ホスト壁時計に依存しない、仮想CPUサイクルの区間計測（性能チューニング用）。開始・終了PCへの到達時に64-bitカウンタをラッチします。ブレークポイント・ウォッチポイント・debug_runは使わず、通常実行のまま測る。

- `action="arm"`: 計測区間を登録する（「計測を待機状態にする」操作。ここでは測らない）。`start_pc`/`end_pc`（偶数・相異なる）を登録し、前回の結果を破棄します。登録後にCPUが開始PCを通ってから終了PCを通るまでのサイクル数を、エミュレータが自動で記録する
- `action="status"`: 設定・ラッチ・差分を返す
- `action="clear"`: 登録を解除する（累積カウンタは保持）
- 戻り値: `SEQ=<世代> STATE=<IDLE|WAIT_START|RUNNING|COMPLETE|INVALID> START_PC=… END_PC=… CPU_START/END/DELTA=… ELAPSED_START/END/DELTA=… CPU_TOTAL=… ELAPSED_TOTAL=…`。サイクル値は10進数
- `CPU_*` は命令実行とモデル化済みバスウェイト（最適化の主指標）、`ELAPSED_*` はSTOP/HALT中の待ちを含む仮想経過サイクル
- 値は各PCの命令の直前で採取します。差分は開始命令を含み終了命令を含まないため、ゲスト側は開始・終了位置に専用の`nop`を置く
- `DELTA` は `STATE=COMPLETE` のときだけ有効（それ以外は `NA`）
- TEXT PROBE有効中、およびデバッグ実行経路（`debug_control(action="mode", mode="ON")`・ブレークポイント・ウォッチポイント・例外ブレーク）では`arm`できません。測定中にこれらを有効化すると`STATE=INVALID`になる
- 実行リセットで`IDLE`に戻る。同時に測れる区間は1つだけ
- 使い方は3手順: (1) `arm` で区間を登録する → (2) ゲストの処理を動かす（キー入力など）→ (3) `status` で `STATE=COMPLETE` の `CPU_DELTA` を読みます。ストップウォッチで言えば、(1) どこからどこまで測るか決める、(2) 走る、(3) タイムを見る
- `start_pc`/`end_pc` は10進整数で渡します。16進のアドレスを手で換算すると間違えやすいので、`arm` の戻り値の `START_PC=`/`END_PC=`（16進表示）が意図したアドレスか必ず確認する
- 開始・終了PCは区間の中で何度通っても、最初に通った1回分だけを記録します。ループの中の1回ごとの処理は、1回目しか測れない（ループ全体を測るなら、ループの前後に`nop`を置く）
- 実行ファイル（.x）のロードアドレスは実行時に決まるので、ELFのシンボルアドレスにそのまま使えない（実行時アドレス = ELF上のアドレス + ロードベース）。ロードベースの調べ方は次のどちらか
  - Human68kの`PROCESS.X`で調べる（再ビルド不要）: 測定対象を起動する前にHuman68kのプロンプトで`PROCESS`を実行し、`PROCESS.x`自身の「開始」を読みます。`PROCESS.x`は終了すると解放されて常駐しないので、その後に起動する測定対象も同じ空き領域の先頭に載る。ロードベースは「開始 + 0xF0」（`PROCESS.x`の開始が`168C00`、同じ環境で実測したt_drawのロードベースが`0x168CF0`でした。`+0xF0`はこの1組からの経験値で、他のプログラムでの確認は未了）
  - ゲスト側で`nop`だけの関数（マーカー）を作り、そのアドレスを`printf`で表示させて開始・終了PCに使う（再ビルドが要る。ベースの確実な確認はこちら）
- `PROCESS`の方法は、`PROCESS`と測定対象の間に環境が変わる操作（常駐物の追加、`SET`など）を挟まない前提。ベースがずれると、区間を登録しても`STATE=WAIT_START`のまま`COMPLETE`になりません。最初に1区間だけ登録して`COMPLETE`になることで、ベースを確認する

---

### `read_bitmap_text()`

現在の TVRAM 画面をテキストとして返します。

CGROM ANK フォント（8×16px）と照合してテキスト化します。日本語（漢字等）は `?` になります。各行末の空白は除去されます。

ただし反転表示（白帯・エラーダイアログ等）はビットパターンが CGROM グリフと一致しないため、IOCS `_B_PUTMES` の呼び出し引数からそのまま復元して返します（このため白帯の漢字だけは `?` にならず読めます）。復元が使われるのは表示が画面に残っている間だけで、スクロールや上書きで消えれば自動的に失効し、以後は返りません。

HIOCS PLUS 等の常駐ドライバが IOCS `$16`(`_FNTADR`) を差し替えて ANK フォントを実行時に変更している場合、既定では ROM 固定オフセットの CGROM と照合するため文字を誤認識します。フォントを切り替えたら `tools-x68k/fntadr/`（`make`でビルドする`FNTADR.X`）をゲスト側で実行し、実際のフォントアドレスを報告させてください。以後 `read_bitmap_text()`/`run_command()` はその実アドレスを基準に照合します。報告はリセットで失われ、ROM 固定オフセット参照に戻ります。

---

### `read_memory(address, length=16, side_effects=False)`

指定アドレスからメモリを読み出す（最大 256 バイト）。

- `address`: 読み出し開始アドレス（整数）
- `length`: 読み出しバイト数（デフォルト 16、最大 256）
- `side_effects`: True にするとI/Oレジスタのread-clear等の副作用を許容する実読み出し
  （`READ_MEM`）を使います。既定 False は非侵襲peek（`PEEK_MEM`）を使い、探索的な読み出しで
  FDC結果FIFO等の内部状態を壊さない
- 戻り値: アドレスとバイト値のダンプ文字列（16進 + ASCII表示）

```
read_memory(0xED0018, 2)
```

---

### `read_stdout_text()`

`B_PUTC` / `B_PRINT` で出力されたテキストを取得する（差分バッファ＝プログラムの標準出力に相当）。

呼び出しのたびにバッファがクリアされ、前回以降の出力が返ります。コマンド出力を逐次取得したい場合に使います。画面に見えている文字そのものが欲しい場合は `read_bitmap_text()`（TVRAMのピクセルをCGROMと照合する別経路）を使います。

**HIOCS.X または IOCS.X（SHARP 純正）を組み込んだ構成では、コマンド実行以降の出力（`PROCESS`、`ECHO`、`TYPE` など）は本ツールで取れません**（HIOCS.X・IOCS.X の組み込みより前の起動メッセージは取れます。組み込み後に DOS 経由で出る起動中の出力は取れません）。どちらも DOS コール（`_PUTCHAR`/`_INPOUT`/`_PRINT` など）を差し替え、コンソール出力を IOCS（TRAP #15）を経由せず `_B_PUTC` 本体へ直接つなぐため、TRAP #15 のハンドラで取る本ツールに届きません。取得するには `read_text_events()` を使います。出力採取点は、MCP が自動で有効にします（`text_probe()` の「出力採取点の自動有効化」の項を参照）。HIOCS.X・IOCS.X を外した構成では、本ツールでそのまま取れます。

---

### `read_text_events()`

IOCS（`B_PUTC`/`B_PRINT`）および有効な `text_probe()` プローブの出力を、非停止で
取得します。エミュレータから取得済みのイベントはサーバ側で直近
1 MiBのrawペイロードを保持するため、後から条件を変えて検索できます。

既定の `detail="text"` は、同じ `probe` のrawバイトを連結してからデコードした
`streams` を返します。1文字1イベントのログでも、LLMに渡すのは少数の文字列だけになります。
Shift-JISのマルチバイト文字がイベント境界で分断されても復元します。

```python
read_text_events(
    probe=None, source=None, since_seq=None, max_events=None,
    after=None, until=None, contains=None, regex=False,
    include_match=True, detail="text", fields=None,
)
```

- `probe` / `source`: 一致するイベントだけを対象にします。`source` を指定すると、同じ
  テキストを二重に出すプローブ経路を除外できます。
- `since_seq`: 前回の `next_seq` を渡して続きだけ読みます。`max_events` で切った場合の
  `next_seq` は最後に返したイベントの次のseq、切らなかった場合は取得済み末尾の次のseq。
- `after`、`until`、`contains`: 連結後のprobeごとのテキストへこの順に適用します。
  `after` と `until` は見つからなければそのstreamを返しません。`contains` は一致するとき
  だけ返します。`regex=True` なら3つの値を正規表現として扱います。`include_match=False` は
  `after` / `until` の一致文字列を除きます。
- `detail`: `"text"`（既定）は `streams`、`"chunks"` は要約イベント、`"events"` は
  生イベントを返します。`fields` は `detail="events"` のときだけ使え、
  `seq`、`epoch`、`pc`、`x`、`y`、`attr`、`flags`、`source`、`probe`、`encoding`、
  `raw`、`raw_hex`、`text` から必要な列だけを選べます。

共通の戻り値には `next_seq` と、`next_seq` まで欠けずに連続して読める範囲の先頭
`available_from_seq` が入る。`since_seq` がこれより小さければ、その間のイベントは
失われています。エミュレータ側リングの取りこぼしがあった場合は、その呼び出しで
取りこぼした件数だけが `dropped` に入る（累積ではない）。バッファ上限や取りこぼしの前の
イベントは、残っている限り検索できるが連続ではありません。エミュレータの再起動・リセット
（`epoch` の変化や seq の巻き戻り）を検出すると、サーバ側バッファは旧イベントごと破棄します。

```json
{
  "streams": [{"probe": "netbsd-console", "text": "NetBSD/x68k ...\\nlogin: "}],
  "next_seq": 18422,
  "available_from_seq": 18100
}
```

#### 旧APIからの移行

以前のバージョンの戻り値はイベントdictの配列でした。通常のテキスト取得は、旧来の
`events[i]["text"]` の走査ではなく `streams[i]["text"]` を使います。個々のイベントを
調べる必要がある場合だけ `detail="events"` を指定します。連続読み出しでは、前回の
`next_seq` を次回の `since_seq` に渡します。MCPサーバーは再起動後に新しいスキーマを公開します。

---

### `wait_for()`

テキスト出力にパターンが現れるまで待ちます。約50ms間隔で `TEXT EVENTS` を読み、
`read_text_events()` と同じサーバ側バッファに追記しながら、probeごとの連結テキストを
増分検索します。1回の呼び出しで「`login:` まで待つ」が完結し、待機中の中間結果は返しません。

```python
wait_for(
    pattern, timeout_ms=10000, regex=False, since_seq=None,
    probe=None, source=None, include_match=True, max_chars=4000,
)
```

- **探し始める位置。** `since_seq` 省略時は、直前の入力（`paste_text` / `key`（press・down）/
  `run_command`）を送る直前、または直前の `wait_for` のマッチ末尾の直後から探します。
  `paste_text` が戻った時点でコマンド出力が終わっていても取りこぼさず、同じ出現に二度
  マッチもしません。位置はイベント内のバイト位置まで持つので、1つのイベントに続く2つの
  パターンを順に待てます。タイムアウトでは位置が動かないので、引数なしで呼び直せます。
  `since_seq` を明示するとそちらを優先します。サーバ起動直後は位置が未設定で、呼び出し
  時点の末尾から探します。エミュレータの再起動・リセットでバッファが破棄されたときは、新しい
  バッファの先頭から探し、戻り値に `restarted: true` を付けます。
- **マッチ。** `regex=True` は `re.search` で、`^` は起点にだけ一致する（行頭には `(?m)`）。
  ポーリング時点までのデータで判定するので、`login:\s*` のような貪欲なパターンは途中で
  確定します。複数probeが同時に一致したら、マッチ末尾のseqが最小のものを1件返します。同じ
  テキストを二重に出すprobeは `source` で絞る。不正な正規表現は待機前に `ValueError`。
- **タイムアウト。** `timeout_ms` の上限は25000。`key_wait` と違い、タイムアウトは
  例外にせず `matched: false` で返します。待機中は他のツール呼び出しが直列化され、キャンセル
  通知も待機の終了まで処理されません。`timeout_ms <= 0` は1回だけ読んで照合します。
  エミュレータとの通信が切れても期限まで再試行する（再起動中を待てる）。

戻り値:

```json
{
  "matched": true, "probe": "netbsd-console", "text": "NetBSD/x68k ...\nlogin:",
  "next_seq": 18422, "available_from_seq": 18100, "since_seq_used": 18100,
  "elapsed_ms": 3120
}
```

- `text`: 探し始めた位置からマッチ末尾まで（`include_match=False` ならマッチ直前まで）。
  `max_chars` を超えるときは末尾側を残し、`truncated: true` を付ける（マッチ文字列は残す）。
- `next_seq`: `read_text_events()` と同じくイベント単位で、マッチ末尾を含むイベントの次。
  タイムアウト時は探し始めた位置のまま。
- `matched: false` のときの `text` は、直近に出力のあったprobeの末尾。
- `dropped`: 待機中にエミュレータ側リングで取りこぼした合計。`warning` に、取りこぼし、
  起点が保持範囲より前、seqの欠落、通信失敗の再試行が入る。マッチを見逃した可能性があります。

---

### `reset(force_fd0=False)`

エミュレータをソフトリセットする（SRAM は保持）。

- `force_fd0`: `True`なら実機のOPT.1キーを押しながらリセットする操作に相当し、
  FD0からの起動を強制します。既定は`False`。

Human68k が再起動します。

---

### `run_command(command, timeout=10.0)`

Human68k のコマンドを実行し、その出力だけを返します。

- `command`: 実行するコマンド文字列（改行不要）
- `timeout`: 最大待機秒数（デフォルト 10 秒）

```
run_command("dir")
run_command("echo HELLO")
run_command("cd bin")
```

送信前後で `WAIT_KEYIN`（キー入力待ち検出）を使い、早すぎる送信による文字欠落やタイムアウトの浪費を防ぎます。スクロールで画面外に出た出力は取得できません。

---

### `screenshot(layers="display", output_path="/tmp/x68k_screenshot.png", inline=False)`（Windowsでは既定値が`%TEMP%\x68k_screenshot.png`）

現在の画面をスクリーンショットとして保存します。

- `layers`: `"display"`（既定）は実際の表示状態をそのまま返します。`"tvram"`, `"gvram"`, `"sprite"` またはそれらを `+` 区切りで結合した文字列（例: `"tvram+gvram"`）を指定すると、指定レイヤーだけに絞り込んで合成し直します。ただしTON/SON/DISP/GS4-0等の各レイヤー自身のON/OFF状態はそのまま反映するため、非表示中のレイヤーを指定すると黒が返る。表示状態と無関係にレイヤーの生データを見たい場合はCPUを一時停止して`read_memory`で直接読むこと。
  - **注意**: `display`はメインループが毎フレーム更新するキャッシュ済みバッファをそのまま返すのに対し、非`display`の各レイヤーはコマンドを受けた瞬間に非同期で再合成します。可視性ビットがフレーム内の一瞬しか立たない実装のゲームでは、`display`に写っていてもタイミングを外して黒が返ることがある（実例: 男弾のスプライトはDISP/CPUビットが短時間しか1にならず、多くの試行で黒だった）。確実に捉えたいときは対象レジスタに`watchpoint`を張って値変化を検出してから撮ること。
- `output_path`: 保存先パス。Pillow があれば PNG、なければ PPM で保存。
- `inline`: `True` にすると画像そのものを返す（ファイルを開き直さずにそのまま見られます。Pillow必須）。既定の `False` は従来どおり保存先パスを返します。

---

### `power_switch(on=False)`

前面電源スイッチを操作します。`on=False`（既定）でOFF。OFFにするとMFP GPIP2割り込みが入り、IPLROMの電源断処理がシステムポート#6 ($E8E00F) へ `$00,$0F,$0F` を書くと ix68k が終了します。スイッチOFFから終了まではエミュレーション時間で数秒（10MHz概算）かかる。Human68k が DOS 実行中なら後回しになるため、COMMAND.X のプロンプトで実施します。

---

### `send_interrupt()`

NMI（レベル7割り込み）を発生させ、実行中のプログラムを中断します。

Human68k では BREAK キー相当の動作になります。

---

### `text_probe(action="status", profile=None, binary_path=None)`

プロファイル駆動の非停止テキスト出力プローブ（`mc68k_set_entry_probes()`）を管理します。
有効化されたプローブのヒットは `read_text_events()` で取得します。

- `action`:
  - `"status"`: 登録件数・有効状態と、自動有効化の状態（`AUTO=`、下記）を返す（`profile`/`binary_path`は指定不可）
  - `"clear"`: 全プローブ定義を破棄し無効化する（`profile`/`binary_path`は指定不可）。自動有効化も、次のリセットまで止める
  - `"enable"`: `profile`（`tools-host/mcp/text_profiles/*.toml`のプロファイル名）を読み込み、
    対象バイナリのSHA-256を`binary_path`と照合したうえで定義を投入・有効化する
- `profile`: `action="enable"`で使うTOMLプロファイル名
- `binary_path`: `action="enable"`で照合するホスト側の実行ファイルパス。対象バイナリが
  ゲスト内ディスクイメージにしかない場合（NetBSD等）は省略できない制約が残っており、
  その場合は`tools-host/mcp/text_profiles/README.md`の手動投入手順を使う

```
text_probe(action="enable", profile="otoko-rebuilt", binary_path="/path/to/OTOKO.X")
```

#### 出力採取点の自動有効化（HIOCS.X / IOCS.X）

HIOCS.X または IOCS.X（SHARP 純正、排他）が常駐すると、DOS 出力は TRAP #15 を通らず、各ドライバの `_B_PUTC` 本体へ直接つながる。MCP サーバは、次のツール呼び出しのタイミングで、本体の入口を自動で特定して TEXT PROBE を有効にします。`binary_path` も SHA-256 照合も要りません。結果は `read_text_events()` に `probe: B_PUTC_body`（`source` は `hiocs` / `iocsx`）として出ます。

- 確認するタイミング: `paste_text()` と `run_command()` の送信前、`wait_for()` の待機中（約 1 秒ごと）、`text_probe(action="status")`。読み取り専用のツール（`read_text_events()`、`read_stdout_text()`）は、自動有効化をしません。
- 特定の方法: DOS コール表の `_PUTCHAR`（`$1808`）が指す先の入口署名で HIOCS.X / IOCS.X を判別し、`$1808` の値から `_B_PUTC` 本体の入口を逆算する（HIOCS.X は `−0x30AC`、IOCS.X は `−0x2A9E`）。本体の署名も確認して、一致したときだけ有効にします。CONFIG.SYS・コマンドラインのどちらの組み込みでも、CONDRV 併用でも同じ。
- 常駐位置が変わったら張り替え、常駐解除（`/R`）で `$1808` が元に戻ったら、自動で有効にしたプローブだけを無効にします。手動で有効にしたプローブは触りません。
- `text_probe(action="clear")` を呼ぶと、次のリセットまで自動有効化を止めます。`performance_counter()` はTEXT PROBEが有効な間は区間登録できないので、測定の前に `clear` します。
- `text_probe(action="status")` の末尾の `AUTO=`: `IDLE`（対象のドライバなし）、`hiocs@<本体アドレス>` / `iocsx@<本体アドレス>`（有効）、`SUPPRESSED`（`clear` で停止中）、`ERROR(<理由>)`（有効化に失敗。次の確認でやり直す）。
- 無効にするには、エミュレータの設定 `text_auto_probe = false`（`.cfg`）、または起動オプション `-text-auto-probe false` を指定する（既定は `true`）。MCP サーバはエミュレータに `CONFIG GET TEXT_AUTO_PROBE` で問い合わせて値を知る。
- 起動中の出力は取りこぼす場合があります。有効にするのは、ドライバが常駐したあとの最初の確認のタイミングなので、それ以前の出力は採れません。組み込み後の DOS 経由の出力は TRAP #15 にも出ないので、どちらでも採れない（HIOCS.X の組み込み直後に手動で有効にしても、TwentyOne や AUTOEXEC の出力が採れなかったことがあります。原因は未確認）。起動後のコマンド出力は採れます。
- 本体プローブは、TRAP #15 経由の呼び出し（IOCS の直接呼び）も拾うので、`read_text_events()` で TRAP #15 の `B_PUTC` と併せて読むと、CR/LF など一部の文字が重複します。`source`（`hiocs` / `iocsx`）か `probe` で絞って読みます。
- IOCS.X の高速経路は、標準出力がコンソールのときだけ使われる（リダイレクト中は元のハンドラへ進む）ので、リダイレクトされた出力は採れません。

#### `hiocs` プロファイル（HIOCS.X 組み込み時のコマンド出力。手動で有効にする場合）

HIOCS.X（version 1.10+16）を組み込んだ構成では、コマンド出力が `read_stdout_text()` で取れません。`hiocs` プロファイルは、HIOCS.X 内の `_B_PUTC` 本体の入口で D1 の下位 1 バイトを採取します。DOS コール経由の高速経路（`BSR` で本体を直接呼ぶ）も、IOCS ベクタ `$0480` 経由の `JSR (A0)` も、必ずここを通る。

```
text_probe(action="enable", profile="hiocs", binary_path="/path/to/HIOCS.X")
read_text_events(since_seq=<前回の next_seq>)
```

- `binary_path` には、ホスト側へ取り出した HIOCS.X を渡す（SHA-256 照合のため）。`tools-host/hu68disk/hu68disk extract <HDS> /SYS/HIOCS.X <出力先>` で取り出せます。
- ロードベースは入口署名（`1038099066260cb8`）を `0x50000`〜`0x70000` で走査して実行時に求める（HIOCS.X のファイル内 `0x420` が実行時 `0x5A14A` に当たる構成で確認）。
- 結果は `read_text_events()` に `probe: B_PUTC_body` として出ます。日本語（Shift_JIS）もバイト単位で採取されるため、連結後に正しく読めます。D1.W 全体（`size = 2`）で採ると ASCII に `\u0000` が付き、日本語が壊れるため、`size = 1` にしています。
- HIOCS.X のバージョンが変わったら、プロファイルの SHA-256 と入口署名を更新します。
- このプロファイルは、CONFIG.SYS で組み込んだ位置（`0x50000`〜`0x70000`）と、入口が書き換わっていない構成にしか合いません。コマンドラインからの組み込み（ユーザーメモリに載る）や CONDRV 併用（本体の先頭が `BRA.W` に変わる）では、署名探索が失敗します。これらの構成では、自動有効化を使います。
- `text_probe()` が有効な間は `performance_counter()` の区間登録ができません。測定の前に `text_probe(action="clear")` で無効にします。

---

### `trace_iocs(enable=True)`

IOCS/TRAP 呼び出しの全トレースをエミュレータの stderr ログへ出力します。

- `enable`: `True` で有効化、`False` で無効化。デバッグ調査用。

---

### `watchpoint(action, address=0, size=1, id=-1, enable=True)`

値変化検出型ウォッチポイントの管理。書き込みアクセスその
ものではなく「値が変化したこと」を検出するエッジトリガ方式で、実際にCPUを
停止させる（`mc68k_debug_run()`経由の実行でのみ機能する）。書き込み元PCを
知りたい・同じ値の書き戻しも検出したい場合は代わりに `ram_watch()`（バス層
write hook、ログ出力のみでCPUは止めない）を使います。両者は併存し、非推奨化は
しません。

- `action`:
  - `"add"` — `address`に`size`(1/2/4)バイトのウォッチポイントを追加する
  - `"remove"` — `id`のウォッチポイントを削除する
  - `"enable"` — `id`のウォッチポイントの有効/無効を`enable`で切り替える
  - `"clear"` — 登録済み全ウォッチポイントを削除する
  - `"list"` — 登録中のウォッチポイント一覧を返す
- `address`: `action="add"`で監視するアドレス（整数）
- `size`: `action="add"`の監視サイズ（1/2/4バイト、デフォルト1）
- `id`: `action="remove"`/`"enable"`で対象とするウォッチポイントID
- `enable`: `action="enable"`で有効化するか（Falseで無効化）
- 戻り値: `action="add"`は`"ID=<id>"`、`"list"`は`"id=0,addr=...,size=...,
  en=...,hits=...,last=...;..."`形式の一覧文字列、それ以外は簡潔な確認メッセージ

---

### `whiteband(action)`

白帯（Human68k エラー）検出による CPU 自動停止の管理。

白帯（`_ABORTRST` $FD）検出時に CPU を停止する機能は既定オフ。有効化しないと `"continue"` は使えない（停止していない CPU を再開する対象が無いため）。

停止はブレークポイント等と同じ**デバッガの停止**として扱われます。`debug_control(action="status")` は `STATE=paused STOP=WHITEBAND` を返し、`step` で先へ進めることもできます。再開は `whiteband(action="continue")` でも `debug_control(action="continue")` でも同じ。

有効/無効はエミュレータのプロセス状態で、**再起動すると `"off"` に戻る**。現在のarmed状態は `debug_control(action="status")` の `WB=ON`/`WB=OFF` で確認できます。

停止を待つときは `run_command(...)` か `debug_control(action="continue", wait=True)` を使います。`paste_text` は20ms/文字でキューに積むだけなので、**直後の `debug_control(action="status")` は必ず `STATE=running` を返す**（白帯が出ないのではなく、まだ実行されていない）。

```
whiteband(action="on")
run_command("ADDRERR", timeout=15)   # 白帯を出すコマンドを実行する
debug_control(action="status")       # → STATE=paused STOP=WHITEBAND MODE=AUTO CAP=... EXEC=... WB=ON ...
```

- `action`:
  - `"on"` — 白帯検出で CPU を停止する機能を有効化する
  - `"off"` — 上記を無効化する
  - `"continue"` — `"on"` 中の白帯検出で停止した CPU の実行を再開します。停止時は直前の IOCS 呼び出し履歴がエミュレータのログにダンプされます。状態（registers/memory）を調べ終えたら呼ぶこと。

---

### `write_memory(address, data)`

指定アドレスにバイト列を書き込む（最大 256 バイト）。

- `address`: 書き込み開始アドレス（整数）
- `data`: 書き込むバイト値のリスト（0–255）

```
write_memory(0xED0018, [0xA0, 0x00])
```

---

## 内部プロトコル

エミュレータ本体と、既定で `/tmp/ix68k_mcp.sock`（Unix ドメインソケット）で通信します。**Windowsのみ** `127.0.0.1:26800` のTCPループバックで通信します（python.org配布のWindows版Pythonが `socket.AF_UNIX` を持たないため。macOS/Linuxは引き続きAF_UNIX）。上記ツールはこのコマンドの薄いラッパーです。ソケットパスは環境変数 `IX68K_MCP_SOCK`（macOS/Linux）、ポート番号は `IX68K_MCP_PORT`（Windows）で変更できます（複数インスタンス起動時の使い方は本ファイル末尾の「複数インスタンスの起動」節を参照）。

### 書式

リクエスト・レスポンスとも **`<uint32 BE payload_len><payload>`**（4バイト長プレフィックス＋本体）のフレーミングです。`payload` は **`<tag> <本文>\n` + 生バイト列**（本文が無いコマンド・応答は末尾の生バイト列が0バイト）。

- `tag` はクライアントが生成する任意トークン（空白なし）。サーバは受け取ったtagをそのまま応答の先頭へエコーするだけで、サーバ側に状態は持ちません（1コマンド1接続で応答を待つ現行クライアントでは単なるやまびこですが、将来パイプライン化したときにリクエストと応答を対応付けられるようにするための予約です）。
- リクエストのヘッダ行（tagの後）は **`<GROUP> [<ACTION>] [args...]`** の2段構造です。C側はグループ表→アクション表のテーブル駆動だけでディスパッチします（`src/mcp/commands.c`。`if`/`strncmp` の連鎖は書きません）。MCPツール1つがワイヤのグループ1つに対応するので、ツールを増やすときに「どこに足すか」が一意に決まります。tagの切り出し・フレーミングは `src/mcp/server.c` の責務で、`commands.c` はtagの存在を一切知りません。
- 生バイト列本体を使うのは、リクエスト側は `MEM WRITE`（書き込むバイト列）だけ、レスポンス側は旧base64応答だった `RING` / `MEM READ` / `MEM PEEK` / `DISASM` / `DISK STATUS` / `TEXT STDOUT` / `TEXT BITMAP` / `SCREENSHOT` です（base64は全廃）。

応答のヘッダ行は成功が `OK` または `OK <値>`、失敗が **`ERR <CODE> <message>`** です。CODE は機械可読で、次の意味を持ちます。

| CODE | 意味 |
|------|------|
| `UNKNOWN` | 未知のグループまたはアクション |
| `BADARG` | 引数の形式・範囲が不正（再試行しても無駄） |
| `NOTREADY` | 相手側の操作待ちで今は応じられない（リトライ可） |
| `TIMEOUT` | 待ちが期限切れ（異常ではなく「まだ来ていない」の意） |
| `NOCPU` | CPU未初期化 |
| `FULL` | 登録数上限 |
| `IO` | ファイル・デバイスの入出力失敗 |
| `OOM` | メモリ確保失敗 |
| `INTERNAL` | 実装内部の異常（例: stopinfo応答がバッファ上限を超えて切り詰まった場合） |

### バージョン交渉

接続したクライアントは最初に `HELLO` を送り、`OK PROTO=<n> BUILD=<日付> <時刻>` の `PROTO` が自分の期待値と一致することを確認します。**一致しないまま使うと全コマンドが `ERR UNKNOWN` になり、「エミュレータのビルドが古い」ことに気付けません。** 現在のプロトコル版は **5**（`src/mcp/commands.c` の `MCP_PROTO_VERSION`）。ワイヤ書式・グループ名・応答書式を互換性なく変えたら上げること。

v2以前（改行区切りテキスト、base64、tagなし）のクライアントは、この長さプレフィックス化と互換性がありません。旧クライアントが送る最初の数バイトは長さとして誤読され、サーバ側でフレーム長異常として即座に接続が切られます。

生ソケットのクライアントは3本あり、共通の `tools-host/mcp/ix68k_wire.py` が HELLO、endpoint、tag照合を担当します。ワイヤを変えるときは全クライアントと共有層を同時に直してください。

- `tools-host/mcp/ix68k_mcp_server.py`（MCPサーバ本体）
- `tools-host/ix68k-hostfile/ix68k-hostfile`（人間向けCLI、`HOSTFILE` グループのみ使用）
- `tools-host/debugger/ix68k_debugger.py`（人間向け外部GUI。起動方法は[README](../debugger/README.md)）

外部GUIの通常更新は、1接続内でHELLOを検証してから、`DBG STATUS`、`REGS`、`DISASM`、`MEM PEEK`、BP/WP、ringの同期・読み取り専用要求だけをパイプライン送信します。`DBG WAIT` と `KEY WAIT` は接続を占有するため、GUIは送信しません。timeoutは他クライアントが待機中である可能性を示すbusyとして表示し、短命接続で再試行します。

### グループ一覧

| コマンド | 応答 |
|---------|------|
| `HELLO` | `OK PROTO=<n> BUILD=<date> <time>` |
| `RESET [FD0]` | `OK`（`FD0` でOPT.1相当の強制FD0起動） |
| `INTERRUPT` | `OK` |
| `POWER <OFF\|ON>` | `OK`（前面電源スイッチ操作） |
| `WHITEBAND <ON\|OFF\|CONT>` | `OK`（`CONT` は白帯停止からの再開） |
| `DIAG <kind> <ON\|OFF>` | `OK` / `ERR BADARG`。kind = `IOCS` `BADJUMP` `BADMOVEA` `BADADDR` `BADORI` `PCRING` `REGRING` `FAULTRING` `PCTRACE1024` |
| `RING <PCRING\|REGRING\|CALLRING\|FAULTRING> [count]` | `OK` + 本文（テキストブロック。履歴リングのダンプ取得。記録のON/OFFは `DIAG` 側） |
| `WATCH ON <addr> <len>` / `WATCH OFF` | `OK` |
| `MEM READ <addr> <len>` | `OK` + 本文（生バイト列） |
| `MEM PEEK <addr> <len> [SUPER]` | `OK` + 本文（生バイト列。非侵襲、I/Oの副作用なし）。`SUPER` は現行SRに依らずスーパーバイザプログラム空間（FC=6）で読みます。それ以外の第3引数は `ERR BADARG` |
| `TEXT EVENTS` | `OK` + `TEV1`本文（IOCSおよび入口プローブのイベント。成功時に消費） |
| `TEXT PROBE ADD <pc> <STRING\|CHAR\|CURSOR> <D0\|A0\|S+off> <size> <source> <probe> <encoding> [SUPER]` | `OK` / `ERR BADARG`。定義だけを蓄積します。`SUPER` はSR.S=1のヒットだけ採取する（ユーザーモード中の同一PC実行を除く） |
| `TEXT PROBE ENABLE` / `CLEAR` / `STATUS` | `OK`。`ENABLE` が蓄積済み定義を一括有効化し、`CLEAR` は解除・破棄する |
| `MEM WRITE <addr>` + 本文（書き込むバイト列、最大256） | `OK` / `ERR BADARG` |
| `REGS` | `OK D0=... PC=... SR=...` |
| `CONFIG GET TEXT_AUTO_PROBE` | `OK TEXT_AUTO_PROBE=<0\|1>`（起動時設定 `text_auto_probe` の読み取り専用の問い合わせ。他のキーは `ERR BADARG`） |
| `MMU GET` | `OK MODEL=<68030\|68EC030> TC=... CRP=...(64bit) SRP=...(64bit) TT0=... TT1=... MMUSR=...`（68EC030はTT0/TT1/MMUSRをAC0/AC1/ACUSRの名前で返し、TC/CRP/SRPは出さない）。68000/68010/68020は`OK MODEL=<model> MMU=UNSUPPORTED` |
| `DISASM <addr> [count]` | `OK` + 本文（テキストブロック。非侵襲） |
| `BP ADD <addr> [cond]` | `OK <id>` / `ERR FULL` / `ERR BADARG` |
| `BP DEL <id>` / `BP ENABLE <id> <0\|1>` / `BP CLEAR` | `OK` |
| `BP LIST` | `OK <n> id=0,addr=...,en=...,hits=...,cond=...;...` |
| `WP ADD <addr> <1\|2\|4>` | `OK <id>` / `ERR BADARG` / `ERR NOTREADY` |
| `WP DEL <id>` / `WP ENABLE <id> <0\|1>` / `WP CLEAR` | `OK` |
| `WP LIST` | `OK <n> id=0,addr=...,size=...,en=...,hits=...,last=...;...` |
| `DBG STATUS` | `OK <stopinfo>` |
| `DBG CONT` | `OK`（即座に返す） |
| `DBG STEP [n]` | `OK <stopinfo>` |
| `DBG BREAK` | `OK <stopinfo>`（次チャンク境界での停止要求、即応答は現在状態） |
| `DBG MODE <ON\|OFF\|AUTO>` | `OK` / `ERR BADARG` |
| `DBG WAIT <timeout_ms>` | `OK <stopinfo>` / `ERR TIMEOUT` |
| `EXC SET <hex64>` / `EXC GET` | `OK` / `OK <hex64>` |
| `DISK INSERT <FD\|SCSI> <index> <path>` | `OK` / `ERR NOTREADY`（FDは要eject） / `ERR IO` |
| `DISK EJECT <FD\|SCSI> <index>` | `OK` / `ERR BADARG` |
| `DISK STATUS` | `OK` + 本文（テキストブロック。`FD<n>=<path>` / `SCSI<n>=<path>` の行、パスが空ならメディア無し。挿入中のFDについては`FD<n>.WP=<0\|1>`行、メディア挿入中かつremovableなSCSI_UNIT_DISK(MO)についてのみ`SCSI<n>.WP=<0\|1>`行が続く） |
| `DISK WP <FD\|SCSI> <index> <ON\|OFF>` | `OK` / `ERR BADARG` / `ERR NOTREADY`（未挿入 or FDCビジー/SCSIバス選択中） / `ERR IO`。SCSIは対象がリムーバブルのMOでない場合（固定HDD・CD-ROM）`ERR BADARG` |
| `HOSTFILE PUT <path>` / `HOSTFILE GET <path>` | `OK <size>` / `ERR IO` / `ERR NOTREADY` |
| `HOSTFILE STATUS` | `OK <state> <len>` |
| `KEY SCAN <hex>` / `KEY DOWN <hex>` / `KEY UP <hex>` / `KEY CLEAR` | `OK` |
| `KEY CHECK` | `OK 1` / `OK 0`（ノンブロッキング） |
| `KEY WAIT <timeout_ms>` | `OK 1` / `ERR TIMEOUT` |
| `MOUSE MOVE <dx> <dy>` / `MOUSE BUTTON <left> <right>` | `OK` |
| `JOY SET <index> <hex>` / `JOY RELEASE <index>` | `OK` |
| `PASTE <hexbytes>` | `OK` |
| `KEY KBDSTATE` | `OK LED=<hex> BRIGHT=<hex> DELAY=<hex> RATE=<hex> TX=<送信バイト累計>` |
| `KEY PASTE` | `OK <PASTEキュー残りバイト数> <MFP USART RX未読 0\|1> <FIFO破棄累計バイト数>` |
| `TEXT STDOUT` / `TEXT BITMAP` | `OK` + 本文（生バイト列） |
| `PERF ARM <start_pc> <end_pc>` | `OK SEQ=… STATE=WAIT_START …` / `ERR BADARG`（奇数・同値・非16進） / `ERR NOTREADY`（TEXT PROBE有効中・debug_run経路） / `ERR NOCPU` |
| `PERF STATUS` | `OK SEQ=… STATE=… START_PC=… END_PC=… CPU_START/END/DELTA=… ELAPSED_START/END/DELTA=… CPU_TOTAL=… ELAPSED_TOTAL=…`（10進。未確定の値は `NA`） |
| `PERF CLEAR` | `OK` |
| `SNAPSHOT SAVE` | `OK SLOT=VALID`。停止中の単一スロットへ保存します。 |
| `SNAPSHOT LOAD` | `OK STATE=paused STOP=SNAPSHOT ... RINGS=CLEARED ...`。保存状態を復元し、履歴リングを空にします。 |
| `SNAPSHOT STATUS` | `OK SLOT=EMPTY` / `OK SLOT=VALID` / `OK SLOT=INVALID REASON=MEDIA_CHANGED`。実行中でも照会できます。 |
| `SCREENSHOT [mask]` | `OK` + 本文（PPMデータ。maskは1–7のレイヤービット） |

### stopinfo（PROTO v4）

`DBG STATUS`、`DBG STEP`、`DBG BREAK`、`DBG WAIT` の成功応答は、`OK ` に続く単一空白区切りのstopinfoを返します。実行中はレジスタを含めず、正確に次の順序です。

```text
STATE=running MODE=<AUTO|ON|OFF> CAP=<NONE|PC|REG|PC,REG> EXEC=<NORMAL|DEBUG> WB=<ON|OFF>
```

停止中は `STATE=paused STOP=<kind> MODE=... CAP=... EXEC=... WB=...` の後に停止詳細（BP/WP/VEC）、さらに `PC`、`D0`–`D7`、`A0`–`A7`、`SR`、`USP`、`SSP` をこの順序で返します。`CAP` は最後にGUI等が送ったDIAG操作ではなく、PCRING/REGRINGの実効ON/OFFです。`EXEC=DEBUG` は次の通常実行チャンクが命令単位debug経路を使うことを示し、BP/WP/例外マスク/MODE ONで有効になります。ring記録だけでは`EXEC`は変わらず、`MODE=OFF`はBP等が残っていても`NORMAL`です。CPU未初期化時は `CAP=NONE EXEC=NORMAL` です。

`SNAPSHOT LOAD` 成功時の停止種別は `STOP=SNAPSHOT` で、レジスタ部の前に
`RINGS=CLEARED` が付きます。`SNAPSHOT SAVE` / `LOAD` は、CPUが未初期化なら
`ERR NOCPU no cpu`、メモリ不足なら `ERR OOM snapshot`、それ以外の準備不足は
`ERR NOTREADY <reason>` を返します。`reason` は `RUNNING`、`NO_SLOT`、`SLOT_INVALID`、
`MEDIA_MISMATCH`、`CPU_CLOCK_MISMATCH`、`CONFIG_MISMATCH`、`EXTERNAL_RESOURCE`、
`FD0_OVERRIDE` のいずれか。`EXTERNAL_RESOURCE` には原因コードが続く
（`ERR NOTREADY EXTERNAL_RESOURCE SCSI0_WRITABLE_NO_OVERLAY` 等）。コードは
`SERIAL_ACTIVE`、`AUTO_INPUT_PENDING`、`HOSTFILE_TRANSFER`、`HOSTDRV`、`ADPCM_RECORDING`、
`FD<n>_WRITABLE`、`FD<n>_NO_FINGERPRINT`、`SCSI<n>_WRITABLE_NO_OVERLAY`、
`SCSI<n>_NO_FINGERPRINT`。保存後に媒体を変更したスロットは `STATUS` で
`REASON=MEDIA_CHANGED` となります。読み取り専用媒体、または `scsi_overlayN` を明示指定した
固定HDDで使用し、入力・シリアル通信を停止してから保存・復元すること。COW差分ではload時に
保存時点のディスク内容へ戻る。SRAM は終了時にload後の内容で書き出されます。

### 非同期待ち

`KEY WAIT` と `DBG WAIT` は受信時に応答せず、後続フレームの poll で完了を判定してから応答します。両者は同じ共通機構（`mcp_context_t` の `wait_kind`/`wait_deadline` と `commands.c` の `s_wait_ops`）を通るので、新しい待ち系コマンドを足すときも状態機械を書き足す必要はありません（完了判定と応答生成の2関数を `s_wait_ops` に追加するだけ）。

### 起動時オプションとの対応

`DIAG` の各 kind は `main.c` 起動時の `-pcring true`/`-regring true`/`-pctrace1024 true`/`-badjump-log true`/`-badmovea-log true`/`-badaddr-log true`/`-badori-log true`、`WHITEBAND ON` は環境変数 `X68K_WHITEBAND=1` でも起動時から有効化できます（`ori_illegal` だけは `DIAG` とは別の `-ori-illegal true` 起動オプション専用で、opcode `0x0000` の実行を実際にILLEGAL例外化する挙動変更機能のためMCPからの実行時トグルはありません。`faultring` には対応する起動時オプションが無く、MCPからの `DIAG FAULTRING ON` のみで有効化します）。

## 複数インスタンスの起動

エミュレータ本体（C）とこのMCPサーバ（Python）は別プロセスであり、同じ待ち受け先で
待ち合わせる必要があります。MCPサーバはClaude Code等が子プロセスとして起動するため、
人間がPython側にCLI引数を渡す機会は無い。よって、両プロセスへ確実に同じ値を届けられる
唯一の経路である**環境変数**で切り替える——macOS/Linuxは`IX68K_MCP_SOCK`（ソケットパス）、
**Windowsは`IX68K_MCP_PORT`（ポート番号）**（WindowsのみTCPループバック
`127.0.0.1:26800`で待ち受けるため）。以下はmacOS/Linuxの手順。Windowsの手順は
本節末尾の「Windows版」を参照。

### 推奨: Claudeセッションごとに専用インスタンス

Claude Codeはstdio MCPサーバを自分の子プロセスとして起動するため、Claude Code自身の
環境変数が継承されます。Bashツールで起動したエミュレータにも同じ環境が引き継がれます。

```bash
# ターミナル1
IX68K_MCP_SOCK=/tmp/ix68k_mcp_a.sock claude
# ターミナル2
IX68K_MCP_SOCK=/tmp/ix68k_mcp_b.sock claude
```

各セッション内から `./build/ix68k ...` を起動すれば、そのセッションのMCPサーバとだけ
繋がる。`.mcp.json` の変更は不要です。

起動スクリプト例（`-sram` も分けること。既定の `sram.dat` を2プロセスで共有すると
書き戻しが競合します。同じディスクイメージを書き込み用に2インスタンスで開くのも避ける）:

```bash
#!/bin/sh
# 例: ./run_instance.sh a -fd0 disk/WORK.XDF
name="$1"; shift
export IX68K_MCP_SOCK="/tmp/ix68k_mcp_${name}.sock"
exec ./build/ix68k -sram "sram_${name}.dat" "$@"
```

環境変数を設定しなければ挙動は今までと完全に同一（既定 `/tmp/ix68k_mcp.sock`）なので、
1台だけ動かす通常運用では何も変更しなくてよい。

### 1つのClaudeセッションから2台へ同時接続したい場合

`.mcp.json`（gitで追跡されている）を常用しないなら編集せず、ローカルスコープで
2台目のサーバ定義を追加する:

```bash
claude mcp add -s local x68k-emulator-b \
  -e IX68K_MCP_SOCK=/tmp/ix68k_mcp_b.sock \
  -- uv run --project tools-host/mcp tools-host/mcp/ix68k_mcp_server.py
```

エミュレータ側は `IX68K_MCP_SOCK=/tmp/ix68k_mcp_b.sock ./build/ix68k ...` で2台目を
起動します。ツールは `mcp__x68k-emulator-b__*` として見えます。ただし `.claude/settings.local.json`
の許可リストをサーバ名ごとに複製する必要があり、モデルから見て2つのサーバのツールが
区別しにくく誤操作のリスクもあるため、常用しない一時的な用途に留めること。

### ix68k-hostfileコマンド（CLI）の場合

`tools-host/ix68k-hostfile/ix68k-hostfile` は `--sock` オプションを持つ人間向けCLIなので、
`--sock`（明示指定）＞`IX68K_MCP_SOCK`＞プラットフォーム既定、の優先順位で決まる。

```bash
IX68K_MCP_SOCK=/tmp/ix68k_mcp_a.sock tools-host/ix68k-hostfile/ix68k-hostfile status
tools-host/ix68k-hostfile/ix68k-hostfile --sock /tmp/ix68k_mcp_b.sock status
```

### Windows版

WindowsのみTCPループバック（既定`127.0.0.1:26800`）で待ち受けるため、上記の
`IX68K_MCP_SOCK`／`--sock`の代わりに**環境変数`IX68K_MCP_PORT`／`--port`**でポート番号を
切り替えます。手順の構造は同じ（`IX68K_MCP_SOCK`を`IX68K_MCP_PORT`に、ソケットパスを
ポート番号に読み替えるだけ）。

```powershell
# ターミナル1
$env:IX68K_MCP_PORT = "26800"; claude
# ターミナル2
$env:IX68K_MCP_PORT = "26801"; claude
```

各セッション内から `.\build_win\ix68k.exe ...` を起動すれば、そのセッションのMCPサーバと
だけ繋がる。ウィンドウタイトルは `ix68k [:26801]` のように表示され、複数ウィンドウを
見分けられます。

`claude mcp add` でローカルスコープに2台目を追加する場合も `-e IX68K_MCP_PORT=26801` を
指定する点以外は同じ。`tools-host/ix68k-hostfile/ix68k-hostfile` は `--port` オプションを持つので、
`--port`（明示指定）＞`IX68K_MCP_PORT`＞既定26800、の優先順位で決まる:

```powershell
$env:IX68K_MCP_PORT = "26800"; python tools-host\ix68k-hostfile\ix68k-hostfile status
python tools-host\ix68k-hostfile\ix68k-hostfile --port 26801 status
```

不正なポート値（数字以外・範囲外・全角数字等）はC側・Python側どちらでも既定値へ
フォールバックせず起動時にエラーとして扱う（別インスタンスへ気付かず接続する事故を
防ぐため）。

**セキュリティ上の注意**: TCPループバックには認証機構が無い。ループバック限定bindで
ネットワーク越しの露出は防げるが、同一マシン上の任意のローカルユーザー・任意の
プロセスが `MEM WRITE` や `HOSTFILE GET/PUT` を実行できてしまう（AF_UNIX版はソケット
ファイルが`%TEMP%`配下のユーザー別ディレクトリに置かれるため、ACLによるアクセス制限が
期待できた）。個人開発機での利用を前提とします。
