---
exe: GCC.X
種別: freeware
作者: 吉野智興（真里子版）（原作: Free Software Foundation, GCC 1.42）
版: 1.30 Tool#2(X680x0) based on GCC 1.42 (2023-10-27時点の配布)
出典: 添付GCC.DOC/GCC.HLP/CHANGE.DOC/VERUP.DOC。配布元
   http://retropc.net/x68000/software/develop/c/gcc_mariko/
判別: -（配布アーカイブ GCC142.LZH 収録版のみ確認）
検証状態: ✅ 起動・コンパイル→アセンブル→リンク→実行をix68k確認済み（GCC_NO_XCLIB発見、下記参照）
最終更新: 2026-08-19
---
# GCC.X (gcc / gcc_cc1 / gcc_cpp) 「真里子版」

> X68k用フリーCコンパイラ。GNU CC 1.42ベース、X68000向けにインラインアセンブラ・割り込み処理拡張・SCD.Xデバッグ情報出力等を追加。`gcc.x`（ドライバ）・`gcc_cc1.x`（コンパイラ本体）・`gcc_cpp.x`（プリプロセッサ）の3本構成。

---

## リファレンス（一次資料要約）

### 起動・コマンドラインオプション

書式（`GCC.HLP`より）:

```
GCC [-オプション...] ソースファイル
```

`gcc.x`はコンパイラドライバであり、内部で`gcc_cpp.x`（プリプロセス）→`gcc_cc1.x`
（コード生成）と処理を渡した後、外部のアセンブラ（既定または`GCC_AS`で指定したもの）・
リンカ（既定または`GCC_LINK`で指定したもの）を順に呼び出して最終的な実行ファイルを作る。

主なオプション（`GCC.HLP`参照）:

| オプション | 効果 |
|-----------|------|
| `-S` | アセンブラを起動せず、アセンブリソース（`.s`）の生成で止める |
| `-c` | リンカを起動せず、オブジェクトファイル（`.o`）の生成で止める |
| `-E` | プリプロセッサの出力だけを標準出力へ流す |
| `-O` | 最適化を有効にする |
| `-I<path>` | インクルードファイルの検索パスを追加する |
| `-o<file>` | 出力ファイル名を指定する |
| `-l<lib>` | リンクするライブラリを指定する |
| `-g` | SCD.X形式のデバッグ情報付きでコンパイルする |
| `-Wall` | 警告表示を最大レベルにする |
| `-ansi` / `-pedantic` | ANSI規格への準拠（`-pedantic`はより厳密なチェック） |
| `-traditional` | K&R第1版のC文法として扱う |
| `-m68881` | 68881(FPU)向けのコードを生成する |
| `-mshort` | `int`を16bit扱いにする（XC標準とは非互換になる） |
| `-fomit-frame-pointer` | 可能な箇所ではスタックフレームの構築を省略する |
| `-fpcc-struct-return` | 構造体の返却方式をXC(Pcc)互換にする |

このほかX68000固有の拡張として、SX-Window向けコード生成の`-SX`、呼び出し方式を
`bsr`/`jsr`のどちらかに固定する`-fall-bsr`/`-fall-jsr`、各コンパイルフェーズの
スタックサイズを指定する`-cc1-stack=N`/`-cpp-stack=N`などがある（詳細は`GCC.HLP`・
`GCC.DOC`参照）。

環境変数によるビルド制御（`GCC.DOC`§7）:

| 変数 | 役割 |
|------|------|
| `GCC_AS` | 呼び出すアセンブラの実行ファイル名（例: `HAS`） |
| `GCC_LINK` | 呼び出すリンカの実行ファイル名（例: `HLK`。既定のリンカより高速） |
| `GCC_LIB` | ライブラリの拡張子（`.a`または`.l`。使用するリンカの形式に合わせる） |
| `GCC_OPTION` | 常に有効にしておきたいスイッチ（例: `Wall`で`-Wall`相当） |
| `真里子` | 拡張機能の個別有効化（割り込み処理・2進数リテラル等をA〜Fの文字で選択） |
| `TEMP` | 作業ファイルの置き場所。メインメモリ2MB機ではHDDやRAMディスクの指定が推奨されている |

`-z-heap`/`-z-stack`オプションは、リンカとして`hlk.x`系を`GCC_LINK`に指定している
場合にのみ効果を持ちます。

---

## ix68kでの利用

### 入手先

http://retropc.net/x68000/software/develop/c/gcc_mariko/ から`GCC142.LZH`
（バイナリセット）としてダウンロード可能です。
**`arc/GCC142.LZH`にローカル保存済み**（2026-08-12取得）。再検証・再インストール時は
retropc.netへ再度アクセスせず、まずこちらを使うこと。

ソースセット`GCC142SR.LZH`（1.3MB）は未取得（本体実行のみに必要なバイナリセットのみ導入）。

### ランタイムライブラリ・環境変数（2026-08-12 導入・ix68k検証済み）

`GCC.DOC`（§5 動作環境、§6 インストール）が前提としていた依存物は以下の通り対応済み:

- **Cライブラリ**: Sharp純正XCではなく[[LIBC]]（LIBC 1.1.32A ぱっち DON版 + 原版から
  補完した`libgnu.a`。詳細は`freeware/LIBC.md`参照）を`\INCLUDE`・`\LIB`へ導入。
  `libc.a`はstartup関数`_start`/`_main`等も内包します。
- **アセンブラ/リンカ**: [[HAS.X]]・[[HLK.R]]を使用。`VERUP.DOC`に「HAS Ver 3では
  プログラムカウンタ相対を自前で解決」する旨の記載がありHAS Ver3系との組み合わせは
  文書上サポートされています。ix68k検証でも問題なく動作。
- **環境変数**（`AUTOEXEC.BAT`に設定済み）:

  ```
  SET include=C:\INCLUDE
  SET lib=C:\LIB
  SET GCC_AS=HAS
  SET GCC_LINK=HLK
  SET GCC_LIB=.a
  SET DOSEQU=C:\INCLUDE\doscall.equ
  SET GCC_NO_XCLIB=1
  ```

#### ⚠️ `GCC_NO_XCLIB`（gcc.xバイナリのstrings解析で発見、GCC.DOC/GCC.HLPに記載なし）

ix68k検証で最初`gcc -O test.c`を実行すると`Not found : clib.a` / `Not found : gnulib.a`
で失敗しました。`gcc.x`本体にはリンク仕様が2系統埋め込まれており、既定では
**XC純正の命名規則**（`clib.a`・`gnulib.a`・`doslib.a`のように「名前+lib+拡張子」の順、
`C library for X68000 XC`という埋め込み文字列あり）を前提にします。環境変数
`GCC_NO_XCLIB`をセットすると**Unix風の命名規則**（`libc.a`・`libgnu.a`・`libdos.a`の
ように「lib+名前+拡張子」の順）に切り替わり、[[LIBC]]（Project LIBC Group版）が
そのまま使えるようになる（`_use_unix_libname`という内部シンボル名が該当ロジックに
対応と推測）。**`CLIB.A`はXC専用の別ライブラリであり、[[LIBC]]の`libc.a`にリネームして
代用することはできない**（ABI・提供関数が異なる可能性が高く未検証。`GCC_NO_XCLIB`側の
命名規則を使うのが正解）。

#### ⚠️ `printf`等の浮動小数点書式を使う場合は`-lgcc`が追加で必要

[[LIBC]]の`libc.a`内`_doprnt.o`（`printf`系列の内部実装）は浮動小数点比較ヘルパー
`___ltdf2`を参照するが、`libgnu.a`（1993年版）にこの関数は含まれていない
（`freeware/LIBC.md`「GCC2由来のlibgcc.a追加」参照）。`puts`等floatを経由しない
出力関数のみなら`-lgcc`は不要です。

```
GCC -O test.c -lgcc -o test.x
```

`GCC_LINK`環境変数に`-lgcc`を埋め込んでの恒久化は**動作しなかった**
（`Not found : libgcc.a`。`lib`環境変数によるパス解決はコマンドライン上の`-l`
経由でのみ機能し、`GCC_LINK`側に追記した引数はそのまま素通しされるだけで
パス解決されない模様）。都度`-lgcc`をコマンドラインに指定すること。

### インストール（SYSTEM_HD.HDSへの導入手順）

CONFIG.SYS登録不要。PATHの通ったディレクトリ（`\BIN`等）に3本の実行ファイルを
置くだけで使える（動作に必要な依存ライブラリは別途、上記参照）。

```sh
# 1. ダウンロード（arc/GCC142.LZHにローカル保存済みならそれを使い、再ダウンロードしない）
cp arc/GCC142.LZH ./GCC142.LZH
# 無ければ: curl -LO http://retropc.net/x68000/software/develop/c/gcc_mariko/GCC142.LZH

# 2. 展開（unar、要 brew install unar。LZH形式）
unar -o ./gcc142_extracted GCC142.LZH
# → ./gcc142_extracted/GCC142/{CHANGE.DOC, COPYING, GCC.DOC, GCC.HLP, VERUP.DOC,
#    gcc.x, gcc_cc1.x, gcc_cpp.x}

# 3. 本体3本をHDSの\BINへコピー（hu68disk、エミュレータ停止中に実行）
for f in gcc.x gcc_cc1.x gcc_cpp.x; do
  tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./gcc142_extracted/GCC142/$f /BIN
done

# 4. ドキュメントはHDSの\DOC\GCCへコピー（ix68k上で参照用）
tools-host/hu68disk/hu68disk mkdir disk/SYSTEM_HD.HDS /DOC/GCC
for f in CHANGE.DOC COPYING GCC.DOC GCC.HLP VERUP.DOC; do
  tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./gcc142_extracted/GCC142/$f /DOC/GCC
done
```

`\DOC\GCC`配下（2026-08-12導入）:`GCC.DOC`（本体マニュアル）・`GCC.HLP`
（コマンドオプション早見表）・`CHANGE.DOC`（GCC X68000 tool#1からの変更点）・
`VERUP.DOC`（バージョンアップ内容）・`COPYING`（GPLライセンス条文）。ソース
（`GCC142SR.LZH`）は未取得のためHDSへのコピーもなし。

### MCP アクション（ix68k検証済み）

```
run_command("GCC -O -S TEST.C")         # アセンブラ出力まで（-Sでリンク・アセンブル省略）
run_command("GCC -O TEST.C")            # フルコンパイル（floatを使わない出力関数のみなら-lgcc不要）
run_command("GCC -O TEST.C -lgcc")      # printf等float書式を使う場合はこちら
```

`\BIN`がPATH外の場合はフルパス指定（`C:\BIN\GCC ...`）で呼び出します。

### 動作確認の再現手順（GCCフルパイプラインテスト）

`\WORK\GCCTEST`に成果物が残っている（2026-08-12検証）。

#### 1. テストソース

```c
/* puts()版（-lgcc不要） */
#include <stdio.h>
int main(void) {
    puts("Hello, GCC world! (puts)");
    return 0;
}

/* printf()版（-lgcc必要） */
#include <stdio.h>
int main(void) {
    printf("Hello, GCC world!\n");
    return 0;
}
```

#### 2. HDS配置

```sh
tools-host/hu68disk/hu68disk mkdir disk/SYSTEM_HD.HDS /WORK/GCCTEST
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./HELLO.C /WORK/GCCTEST   # printf版
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./HELLO2.C /WORK/GCCTEST # puts版
```

#### 3. MCP実行

```
run_command("CD \WORK\GCCTEST")
run_command("GCC -O HELLO2.C -o HELLO2.X")   # puts版、追加オプション不要
run_command("HELLO2")                         # → "Hello, GCC world! (puts)"

run_command("GCC -O HELLO.C -lgcc -o HELLO.X") # printf版、-lgcc必須
run_command("HELLO")                           # → "Hello, GCC world!"
```

いずれも`GCC_NO_XCLIB=1`が`AUTOEXEC.BAT`で有効になっている前提（本HDSは設定済み）。

### 検証メモ

- 2026-08-12: `arc/GCC142.LZH`から`gcc.x`/`gcc_cc1.x`/`gcc_cpp.x`をHDS(`\BIN`)へ導入。
- 2026-08-12: ix68kでフルパイプライン（GCC→HAS→HLK→実行）を検証。
  `GCC_NO_XCLIB=1`の発見・`-lgcc`追加が必要と判明した経緯は上記セクション参照。
  `puts`版・`printf`版とも正常動作を確認。
- 出典アーカイブ: `GCC142.LZH`（329.7KB、`CHANGE.DOC`/`COPYING`/`GCC.DOC`/`GCC.HLP`/
  `VERUP.DOC`/`gcc.x`/`gcc_cc1.x`/`gcc_cpp.x`を同梱）。
- 未検証: 個別コマンドラインオプションの網羅的な動作確認（`-S`/`-c`/`-E`等の基本動作は
  今回の一連の作業中に間接的に使用し問題なし、それ以外は`GCC.HLP`記載のみ）。
