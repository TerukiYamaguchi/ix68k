---
exe: MAKE.X
種別: freeware
作者: 立花えり子（GNU Make本体: Free Software Foundation）
版: GNU Make 3.79 human68k-1.1
出典: 配布元 http://retropc.net/x68000/software/develop/make/gmake379/ （`MAKEHU11.ZIP`）
判別: 同名の公式`MAKE.X`（Sharp/Hudson、XCコンパイラ付属、📖 `official/MAKE.X.md`）とは別物。
  こちらはGNU Make本家のHuman68k移植版で、`?=`等GNU Make固有の書式に対応する
検証状態: ✅ SYSTEM_HD.HDSの`\BIN`へ導入完了（2026-08-12）。起動確認は未実施
最終更新: 2026-08-19
---
# MAKE.X (GNU Make 3.79 human68k-1.1)

> GNU Make のHuman68k移植版。`mint`（`extern/mint/src`）等、GNU Make形式のMakefileを前提に書かれたソースをビルドするために導入。

---

## リファレンス（一次資料要約）

### 起動・コマンドラインオプション

書式:

```
MAKE [引数] [Makefile内ターゲット]
```

オプション体系はGNU Make標準のもの（`-f`/`-C`/`-n`/`-B`等、Sharp純正MAKE.Xの`/`形式では
なく`-`形式）をそのまま踏襲しています。詳細は同梱`make.info-*`参照（今回取得したアーカイブ
には同梱されておらず未取得）。

### UNIX版との既知の差異

`README.X68`が説明しているHuman68k移植版特有の挙動:

- 使用するシェルは、Makefile中のマクロ`SHELL`→環境変数`MAKE_SHELL`→`/bin/sh`の順で
  決定される（本家UNIX版と異なり環境変数`SHELL`は参照しない）
- `.x`拡張子向けの暗黙ルールが追加されている
- 標準マクロの既定値がHuman68k環境向けに変更されている:

  | マクロ | UNIX版の既定 | Human68k版の既定 |
  |--------|-----------|----------------|
  | AR | ar | oar |
  | ARFLAGS | rv | -rv |
  | AS | as | has |
  | CC | cc | gcc |
  | CXX | g++ | gcc2 |
  | LD | ld | hlk |
  | LEX | lex | flex |
  | YACC | yacc | bison -y |

  この既定値は`extern/mint/src/Makefile`が`AS=has060`/`LD?=hlk`/`CC=gcc2`と明示的に
  上書きしていることと矛盾しない（元々この環境を前提にした既定値になっている）
- アーカイブメンバ指定（`lib(obj.o)`形式）には対応しません。並列実行オプション（`-j`/`-l`）
  は意味を持ちません。Shift_JISの2バイトコードを扱えます。作業ファイルの置き場所は環境変数
  `temp`（未定義なら`/tmp`）で決まる。`d:/file`形式のドライブ名付きパスを指定できます。
  CRLF改行のMakefileも読み込める
- Makefile中で定義したマクロは、既定では子プロセスに環境変数として引き継がれない
  （引き継がせるには`export`指令が必要。GNU Make 3.63以降の標準的な挙動）

---

## ix68kでの利用

### 入手先

http://retropc.net/x68000/software/develop/make/gmake379/ から入手可能です。

- `MAKEHU11.ZIP`（104.8KB）— human68k-1.1本体（`make.x`・`README.X68`・`ChangeLog.X68`・`make.dif`）
- `MAKEHU12.ZIP`（4.9KB）— 1.1→1.2の差分（`make.bfd`＋`make.dif`。適用には`bup.x`が必要）

**`arc/MAKEHU11.ZIP`・`arc/MAKEHU12.ZIP`にローカル保存済み**（2026-08-12取得）。

#### 1.2差分は未適用（意図的）

`make.dif`の内容を確認した結果、1.1→1.2の変更は**リンクする`libc`を`libc-1.2.20`から
`libc-1.2.20+`へ差し替えただけ**（`ChangeLog.X68`より）で、make自体の機能・挙動に変更は
ありません。`bup.x`（別途入手が必要な二進差分適用ツール）を用意する手間に見合わないため、
1.1の`make.x`をそのまま採用しました。

### インストール（HDSへの導入手順）

```sh
cp arc/MAKEHU11.ZIP ./MAKEHU11.ZIP    # 無ければ: curl -LO http://retropc.net/x68000/software/develop/make/gmake379/MAKEHU11.ZIP
unar -o ./out MAKEHU11.ZIP            # → out/make-3.77-human-1.1/{make.x, README.X68, ChangeLog.X68, make.dif}

tools-host/hu68disk/hu68disk copy <対象HDS> out/make-3.77-human-1.1/make.x /BIN
tools-host/hu68disk/hu68disk mkdir <対象HDS> /DOC/MAKE
tools-host/hu68disk/hu68disk copy <対象HDS> out/make-3.77-human-1.1/README.X68 /DOC/MAKE
tools-host/hu68disk/hu68disk copy <対象HDS> out/make-3.77-human-1.1/ChangeLog.X68 /DOC/MAKE
```

導入済みHDS: `disk/SYSTEM_HD.HDS`（`\BIN\MAKE.X`、`\DOC\MAKE\`）。

### 検証メモ

- 検証: `\BIN\MAKE.X`・`\DOC\MAKE\`への配置のみix68k上で確認（2026-08-12）。
  実際の起動・ビルド動作は未検証（`extern/mint/src`のビルドに`has060`/`hlk`/`gcc2`が別途必要で、
  これらは現時点で未導入のため単体起動確認のみ先行）
