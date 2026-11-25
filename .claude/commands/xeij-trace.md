# XEiJ 差分デバッグ（参照実装との突き合わせ）

うちのエミュレータの挙動が実機/ROMの仕様通りか分からないとき、参照実装 XEiJ で
同じ操作を再現し、IOCS呼び出しトレースやレジスタ挙動を突き合わせて原因を切り分ける手法。
過去に EXG 命令バグ・MOVEM バグ・Issue #15（グラフィック IOCS 委譲漏れ）がこの方法で解決した。

## いつ使うか

- IOCS コール（TRAP #15）の戻り値・副作用がうちのエミュレータだけ期待と違う
- 「サンプルプログラムが悪いのか、うちの実装が悪いのか」を切り分けたい
- ROM 委譲済みのはずの処理が実機と違う挙動をする

**先に疑うべきもの**: まず自作テストプログラムの引数・呼び出し順序を疑う
（IOCS の仕様は一次資料 X68k IOCS Data Crystal で確認）。
それでも解決しなければ本手法で参照実装と比較する。

## 環境

計装済み（IOCS/TRAP/例外トレース入り）ビルドが用意済み:

```
<XEIJ_TRACE_DIR>/
```

**iCloud Drive（`~/Library/Mobile Documents/...`）配下には置かないこと**（2026-07-29に移設済み）。
`class/`配下に2000超のコンパイル済み`.class`ファイルがあり、iCloudのオンデマンドダウンロードが
ファイル単位で発生するため、起動のたびに数分の遅延や`NoClassDefFoundError`（例:
`MemoryMappedDevice$10`）による起動失敗が発生した。もう一台のマシンでも同じ
`<XEIJ_TRACE_DIR>/`に配置する運用にしている。

- `xeij/XEiJTrace.java` — トレースヘルパー本体（`-Dxeij.trace=true` で有効化）
- `xeij/MC68000.java` の以下3箇所から呼ばれている:
  - `XEiJTrace.onVsync()` — VSYNC 毎（`-Dxeij.trace.frames` で自動終了フレーム数指定、既定300=5秒@60fps）
  - `XEiJTrace.onIocs(fn, pc, d1)` — TRAP #15 (IOCS) ディスパッチ直前
  - `XEiJTrace.onTrap(num, pc, d0, a7)` — TRAP #0-14 ディスパッチ直前
  - `XEiJTrace.onException(vecNum, savePC)` — BUS_ERROR/ADDR_ERROR/ILLEGAL/autovector 割り込み
- 既に `class/` にコンパイル済み（再ビルド不要、そのまま使える）
- 設定ファイル `~/XEiJ.ini` はこのマシン共通（トレース版・無印版を分けて使う場合も共有される）。

## 起動方法

**重要: 出力を必ずファイルへ直接リダイレクトする。`| head` 等のパイプを挟むと
`head` 側が終了した時点で以降の書き込みが SIGPIPE で黙って失われる
（実際にこれで最初のトレースを取りこぼした事故があった）。**

```bash
cd "<XEIJ_TRACE_DIR>"
nohup java -Dxeij.trace=true -Dxeij.trace.frames=1200 \
  -cp "class:jSerialComm-2.11.0.jar" xeij.XEiJ \
  -cgrom=data/CGROM_XEiJ.DAT \
  > /tmp/xeij_trace.txt 2>&1 &
```

素の起動（トレース無し、GUI比較用途）は `./run.sh` を使う（`-fd0=<path>` 等をそのまま追記可）。

- `xeij.trace.frames=1200` で約20秒後に自動終了（`XEiJTrace.onVsync` が上限に達すると `System.exit(0)`）。
  検証したい操作の長さに応じて調整する。
- GUI ウィンドウはこの Mac の実ディスプレイに表示される。**この Claude セッションから
  GUI 操作はできない**（computer-use ツールなしの場合）。ユーザーに実際にキー入力
  （起動待ち→コマンド実行）してもらう必要がある。
- fd0 に何も指定しなければ `~/XEiJ.ini` の設定（前回起動時の状態）がそのまま使われる。
  特定のディスクイメージでテストしたい場合は事前にユーザーへ「無印 XEiJ で disk0 に
  ○○.XDF をセットしておいてください」と依頼するか、`-fd0=<path>` を起動時に付ける。

### 060turbo / 50MHz で SYSTEM_HD.HDS から起動する（2026-10-29 確認済み）

`~/XEiJ.ini` に保存されたメモリ・SRAM の状態（`memorydata=`、`sramdata=`）が残っていると、
IPLROM 1.6 が「エラーが発生しました。リセットしてください。」で止まる。`-memorysave=off` や
`sramdata=` の削除では直らなかったので、`~/XEiJ.ini` を読ませない空のホームで起動する。

```bash
S=<scratchpad>   # 空のホームの置き場所
mkdir -p $S/xeijhome
cd <XEIJ_TRACE_DIR>
nohup /opt/homebrew/opt/openjdk/bin/java -Duser.home=$S/xeijhome \
  -cp "class:jSerialComm-2.11.0.jar" xeij.XEiJ -cgrom=data/CGROM_XEiJ.DAT \
  -model=060turbo -clock=50 \
  -sc0=<XEIJ_DIR>/disk/SYSTEM_HD.HDS -boot=sc0 \
  > $S/xeij.log 2>&1 &
```

- `-model=060turbo` は MC68060 50MHz で、IPLROM は常に 1.6（`-iplrom16=on` は不要）。
- 起動中の XEiJ は先に `pkill -f xeij.XEiJ` で止める（多重起動すると ini と SRAM を取り合う）。
- 空のホームで起動すると ini の設定（ディスク履歴・キー割当等）は使われず、終了時の保存先も空のホームになる。
- ログに `Human68k version 3.02 にパッチをあてました`、`PCM8A.X 1.02 にパッチをあてました` が出れば
  CONFIG.SYS のドライバ読込を過ぎている。画面は XEiJ の GUI でしか見えない（ログにエラーは出ない）。
- XEiJ 側の `SYSTEM_HD.HDS`（`XEiJ_0260608/disk/`）は CONFIG.SYS の `HOSTDRV.SYS` 行が有効。
  HOSTDRV.SYS は ix68k 以外ではバスエラーを捕捉して「ix68k 以外では組み込めません。」と表示し、
  組み込まずに起動を続ける（2026-10-30 確認済み。旧版ではバスエラー SR=$2004 PC=$0006A084）。
  旧版・コメントアウト版は `SYSTEM_HD.HDS.bak_hostdrv` に残っている。
  `disk/SYSTEM_HD.HDS`（ix68k 用）とは別ファイルで、CONFIG.SYS の内容も違う。
- CONFIG.SYS の編集は `tools-host/hu68disk/hu68disk` の `extract` / `del` / `copy`（`/CONFIG.SYS`）。
  Shift-JIS・CRLF のままバイト列で書き換える。

## トレースの読み方

```bash
grep -c "fn=94\|fn=B1 \|fn=B5 " /tmp/xeij_trace.txt   # 目的の fn が何回呼ばれたか概数を確認
grep -n "fn=94\|fn=B1 \|fn=B5 " /tmp/xeij_trace.txt   # 行番号付きで抽出
```

- ログは `[IOCS] fn=XX PC=XXXXXXXX D1=XXXXXXXX` 形式。`fn` は2桁16進、大文字。
- **D1 の値は鵜呑みにしない**。IOCS 関数によっては D1 を引数として使わない
  （実引数は a1 が指すパラメータブロック経由）ため、D1 は直前の呼び出しの
  残骸（stale value）であることが多い。D1 が意味を持つかは対象 fn の仕様を
  `X68k_IOCS_Data_Crystal.html` で確認してから判断する。
- トレースは非常に長大（起動だけで数十万行）になりうる。`grep -c` で概数を
  確認してから `grep -n` で絞り込む、が定石。ファイル全体を Read しない。
- ブート中の同じ命令列が延々ループする箇所（キー入力待ちのポーリング等）は
  正常なアイドル状態。何行あっても無視してよい。

## ROM の実際の状態フラグ・分岐条件を突き止める（IPLROM 逆アセンブル併用）

XEiJ のトレースだけでは「ROM 内部で何が起きているか」までは分からない
（IOCS ディスパッチの入口しか記録していないため）。ROM 内部のワーク変数や
分岐条件まで追う場合は `/rom-grep` と併用する。ただし**通常の `IPLROMXV.S` は
アドレスコメントが付いていないため、分岐先の正確なアドレスを手で数えるのは
非常に間違えやすい**（実際にこれで誤った行を辿ってしまった事故があった）。

**アドレス付きで再生成してから使うこと**:

```bash
cd "<DIS_DIR>/disasm/iplromxv"
export dis_include=../../include
echo "y" | ../../src/dis -zfe0000,fe0000 -gIPLROMXV.lab -TIPLROMXV.TAB -a1 \
  IPLROMXV.DAT IPLROMXV_addr.S
```

- `-a1` で毎行アドレスコメント（`;ffae74` 形式）が付く。既存の `IPLROMXV.S`
  （アドレスなし）は上書きしない別名で出力すること。
- 生成後は通常どおり `grep -anE ';ffae74'` のようにアドレスコメントで直接引ける。
- XEiJ の実装（`xeij/CRTC.java` 等）にレジスタビット配置の正確なコメントが
  あることが多い。ROM 逆アセンブルより先に**まず XEiJ のソースを grep する**方が
  誤読が少なく速い（実際、CRTC R20 のビット配置は逆アセンブルの手作業推測では
  誤り、XEiJ ソースのコメントで正確な配置が一発で判明した）。

## 実機（うちのエミュレータ）側の突き合わせ

XEiJ 側で「成功する」ことを確認したら、うちのエミュレータで同じ操作をして
`get_registers`/`read_memory`（`/mcp-debug` 参照）で state を比較する。

**`breakpoint(action="wait", address=...)` は CPU がそのアドレスに到達するまで
ブロックする点に注意**（ヒットしてもCPUはpausedのまま残る。再開が要らない
単純な到達確認だけなら`debug_control(action="continue")`を呼ばず結果だけ
見ればよい）:
- 対象アドレスに実際にコードが到達しないと（委譲漏れ等でそもそも ROM に
  ジャンプしていない場合）、指定 timeout で必ず失敗する。これ自体が
  「委譲されていない」ことの有力な傍証になる（実際 Issue #15 はこれで発覚した）。
- 遅延実行したいキー入力は `paste_text` で先に送ってから
  `breakpoint(action="wait", ...)` を呼ぶ（`run_command` は完了を待ってしまう
  ため、ブレークポイントを仕掛ける前に対象コマンドが終わってしまう）。
- アドレスは10進数で渡す（16進 `0xFFAE74` → Python 等で `int("FFAE74",16)` に変換
  してから渡すこと。ここでの変換ミスで無駄なタイムアウトを繰り返した実例あり）。

## この手法で解決した実例

- EXG 命令バグ（AND/OR の ea 登録範囲が広すぎて EXG を上書きしていた）
- Issue #15: `fn=0x90 _G_CLR_ON` が ROM 委譲リストから漏れ、グラフィック使用可否
  フラグ（ROM 内部ワーク変数 `$964`）が初期化されず全描画 IOCS が失敗していた
