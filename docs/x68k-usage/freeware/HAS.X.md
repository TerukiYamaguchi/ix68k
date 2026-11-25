---
exe: HAS.X
種別: freeware
作者: M.Kamada（原作: Y.Nakamura「HAS.X v3.09」）
版: HAS060.X v3.09+91 (2023-07-07)。HAS06091.LZH配布形態は"HAS060.X"だが、
   本HDSでは"HAS.X"にリネームして導入（ユーザー指示）
出典: 添付README.DOC/HAS060.DOC。配布元 http://retropc.net/x68000/software/develop/as/has060/
   基本コマンドラインスイッチ(-8/-b/-c/-d/-e/-f/-g/-i/-l/-m/-o/-p/-s/-t/-u/-w/-x)は
   HAS.X v3.09本来の仕様（HAS060.DOCには差分のみ記載、本体の説明は
   Data Crystalの「X68k/Assembler」wikiを参照）
判別: 実行ファイル名"HAS.X"はオリジナルのHAS.X v3.09（中村祐一氏作）とバイナリが異なります。
   本HDSに導入したのはHAS060.X（68060/ColdFire対応・バグ修正・機能拡張版）をHAS.Xに
   リネームしたもの。オリジナルHAS.X v3.09バイナリは本HDSに別途導入していない
検証状態: ✅ 起動・アセンブル動作確認済み（オプション個別検証は未着手）
最終更新: 2026-08-19
---
# HAS.X (has060.x をリネーム)

> M68Kアセンブラ。HAS.X v3.09（中村祐一氏作）をベースに68060/ColdFire命令対応・最適化強化・不具合修正を行った改良版（M.Kamada氏）。AS.Xと比較して高速・高機能。

---

## リファレンス（一次資料要約）

### 起動・コマンドラインオプション

書式（HAS.X v3.09本来の仕様。HAS060.DOCは差分のみ記載のため、基本形は
Data Crystalの「X68k/Assembler」wikiより）:

```
HAS [-スイッチ...] ソースファイル名
```

出力先を`-o`で指定しない場合、オブジェクトファイルはソースファイル名の拡張子を
`.o`に変えた名前で作られます。

v3.09由来の基本スイッチ（Data Crystalの「X68k/Assembler」wiki参照）:

| スイッチ | 効果 |
|-----------|------|
| `-8` | シンボル名の識別に使う長さを8バイトに制限する |
| `-b` | ロングワードのPC間接分岐・ブランチを、絶対ロング指定＋2命令の形に変換する（68000/010向け） |
| `-c` | v2.x互換の最適化のみ行い、新しい最適化を無効化する |
| `-d` | すべてのシンボルを外部定義として扱う |
| `-e` | 外部参照のオフセットを常にロングワードにする（結果として68020以降専用の出力になる） |
| `-f[p,m,w,l]` | リストファイルの出力書式を指定する（既定は`-f1,0,136,56`） |
| `-g` | SCD.X用のデバッグ情報を出力する |
| `-i<パス>` | インクルードファイルの検索パスを追加する |
| `-l` | アセンブル実行時にタイトルを表示する |
| `-m<cpu>` | 対象CPUを指定する（`-m68000`/`-m68010`/`-m68020`/`-m68030`/`-m68040`） |
| `-o<ファイル名>` | オブジェクトファイル名を指定する |
| `-p[<ファイル名>]` | リストファイルを作成する（省略時は拡張子`.prn`が付く） |
| `-s<シンボル>[=定数]` | シンボルを定義する（`.set`相当） |
| `-t<パス>` | テンポラリファイルの作成先パスを指定する |
| `-u` | 未定義のシンボルをすべて外部参照として扱う |
| `-w[レベル]` | 警告表示を抑制する（既定はすべて表示、`-w0`ですべて抑制） |
| `-x[<ファイル名>]` | シンボル情報を出力する |

HAS060.X独自の追加スイッチ（`README.DOC`/`HAS060.DOC`参照、v3.09からの主な追加分）:

| スイッチ | 効果 |
|-----------|------|
| `-s<n>` | 数字だけのローカルラベルで許容する最大桁数（最大4桁）を指定します。上の`-s<シンボル>`とは別系統のオプション |
| `-k[0/1]` | 68060のエラッタ対策を有効/無効にする |
| `-1` | 絶対ロング指定をPC間接へ変換する |
| `-b2`/`-b4` | PC間接をすべて絶対ロングへ変換する追加バリエーション |
| `-c0`〜`-c4` | 最適化の度合いを段階指定する（0=すべて禁止、4=追加最適化まで含めすべて実施） |
| `-c<mnemonic>[=6]` | ソフトウェアエミュレーション命令（FScc/MOVEP等）の展開方法を指定する |
| `-j[n]` | `.SET`/`.OFFSYM`によるシンボル上書きを禁止する条件を指定する |
| `-y[0/1]` | 定義済みシンボル（`CPU`/`__HAS__`/`__HAS060__`/`__DATE__`/`__TIME__`）を有効にするか切り替える |

より詳細な説明は`arc/HAS06091.LZH`同梱の`HAS060.DOC`（差分説明）・`DEBUGGED.DOC`・
`KNOWNBUG.DOC`を参照。

---

## ix68kでの利用

### 入手先

http://retropc.net/x68000/software/develop/as/has060/ から`HAS06091.LZH`（バイナリセット、v3.09+91）としてダウンロード可能です。
**`arc/HAS06091.LZH`にローカル保存済み**（2026-08-12取得）。再検証・再インストール時は
retropc.netへ再度アクセスせず、まずこちらを使うこと。

ソースセット`HASSRC91.LZH`・資料セット`HASDAT91.LZH`は未取得（本体実行のみに必要な
バイナリセットのみ導入）。

### インストール（SYSTEM_HD.HDSへの導入手順）

CONFIG.SYS登録不要。PATHの通ったディレクトリ（`\BIN`等）に本体を置くだけで使えます。
配布名`HAS060.X`だが、**ユーザー指示によりホスト側で`HAS.X`にリネームしてから導入**。

```sh
# 1. ダウンロード（arc/HAS06091.LZHにローカル保存済みならそれを使い、再ダウンロードしない）
cp arc/HAS06091.LZH ./HAS06091.LZH
# 無ければ: curl -LO http://retropc.net/x68000/software/develop/as/has060/HAS06091.LZH

# 2. 展開（unar、要 brew install unar。LZH形式）
unar -o ./has060_extracted HAS06091.LZH
# → ./has060_extracted/HAS06091/{README.DOC, HAS060.DOC, HAS060.X, DEBUGGED.DOC,
#    KNOWNBUG.DOC, HISTORY.DOC, PROJECTS.DOC, HANOI.S, K_MACRO.MAC}

# 3. HAS060.X → HAS.X にリネーム（ホスト側）
cp ./has060_extracted/HAS06091/HAS060.X ./HAS.X

# 4. HAS.XをHDSの\BINへコピー（hu68disk、エミュレータ停止中に実行）
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./HAS.X /BIN

# 5. ドキュメントはHDSの\DOC\HASへコピー（ix68k上で参照用）
tools-host/hu68disk/hu68disk mkdir disk/SYSTEM_HD.HDS /DOC/HAS
for f in README.DOC HAS060.DOC DEBUGGED.DOC KNOWNBUG.DOC HISTORY.DOC PROJECTS.DOC; do
  tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS "./has060_extracted/HAS06091/$f" /DOC/HAS
done
```

`\DOC\HAS`配下（2026-08-12導入）:`README.DOC`（最初にお読み下さい）・`HAS060.DOC`
（動作マニュアル、v3.09からの差分）・`DEBUGGED.DOC`（修正された不具合）・
`KNOWNBUG.DOC`（既知の不具合）・`HISTORY.DOC`（更新履歴）・`PROJECTS.DOC`
（改良構想メモ）。サンプル（`HANOI.S`/`K_MACRO.MAC`）は実行に不要のためHDSへは
コピーしていません。

### MCP アクション（実行例）

```
run_command("HAS -m68000 HANOI.S")     # 68000向けアセンブル
run_command("HAS -o TEST.O TEST.S")    # 出力ファイル名指定
```

`\BIN`がPATH外の場合はフルパス指定（`C:\BIN\HAS ...`）で呼び出します。

### 動作確認の再現手順（HAS+HLK連携テスト）

HAS.X（アセンブル）→ [[HLK.R]]（リンク）→ 実行、の一連の動作を最小構成で確認する
手順。2026-08-12にこの手順で検証済み（`\WORK\HASTEST`に成果物が残っている）。
使用頻度が低いため詳細はここに一次情報として残し、Claudeの自動メモリには登録しません。

#### 1. テストソースをホスト側に作成

[[LIBC]]導入後は`\INCLUDE\doscall.mac`が使えるため、DOSコールは`DOS __PRINT`/
`DOS __EXIT`マクロで書く（`doscall.mac`未導入時はF-line命令`.dc.w $ff09`/`.dc.w $ff00`
を直接埋め込む旧方式で代用可、2026-08-12当初はその方式で検証していた）。

```sh
cat > HELLO.S << 'EOF'
        .include        doscall.mac

        .text
start:
        pea     msg(pc)
        DOS     __PRINT
        addq.l  #4,sp
        DOS     __EXIT

msg:
        .dc.b   'HAS.X + HLK.R test OK',$0d,$0a,0
        .even

        .end    start
EOF
```

`.include`はHASの環境変数`include`（`C:\INCLUDE`に設定済み、`freeware/LIBC.md`参照）
から`doscall.mac`を自動検索します。`DOS`マクロは`doscall.mac`内で`.dc callname`に展開
されるだけなので、`DOS __PRINT`は旧方式の`.dc.w $ff09`と同じF-line命令を生成する
（実際に`HELLO.o`のファイルサイズが両方式で同一(110byte)であることを確認済み）。

#### 2. HDS内にディレクトリを作りソースを配置

**HOSTDRV(D:)経由が簡単**（エミュレータ起動中でも使える、エミュレータ停止不要）:

```
# ホスト側 tests/work/ に書いたファイルはエミュレータ内でD:¥として直接見える
# (エミュレータ起動時ログの「HOSTDRV: <path>を公開します」がマウント元)
```

```
run_command("COPY D:\HELLO.S C:\WORK\HASTEST\HELLO.S")
```

従来通りhu68disk経由（エミュレータ停止中のみ）でも可:

```sh
tools-host/hu68disk/hu68disk mkdir disk/SYSTEM_HD.HDS /WORK/HASTEST
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./HELLO.S /WORK/HASTEST
```

#### 3. エミュレータを起動（未起動の場合）

```sh
./build/ix68k &
```

#### 4. MCPツール（`run_command`）で現在ディレクトリ確認→移動→アセンブル・リンク・実行

```
run_command("CD")              # 現在ディレクトリを確認してから移動する（決め打ちしない）
run_command("CD \WORK\HASTEST")
run_command("HAS HELLO.S")     # → HELLO.O が生成される
run_command("HLK HELLO.O")     # → HELLO.X が生成される
run_command("HELLO")           # 実行
```

期待される最終出力（`HELLO`実行時）:

```
HAS.X + HLK.R test OK
```

`DIR`で`HELLO.S`/`HELLO.o`(110byte)/`HELLO.x`(98byte)の3ファイルが揃っていることも
合わせて確認できます。

#### IOCSコール版（`iocscall.mac`使用）

`\INCLUDE\iocscall.mac`を使うと`IOCS <name>`マクロ（`moveq #番号,d0 / trap #15`に展開）
でIOCSコールを呼べます。DOSコール（`doscall.mac`）と同時に使う例:

```sh
cat > IOCSTST.S << 'EOF'
        .include        doscall.mac
        .include        iocscall.mac

        .text
start:
        moveq   #3,d1           * text palette 3 = white
        IOCS    __B_COLOR

        moveq   #0,d1           * column 0
        moveq   #1,d2           * row 1
        IOCS    __B_LOCATE

        lea     msg(pc),a1
        IOCS    __B_PRINT

        DOS     __EXIT

msg:
        .dc.b   'IOCS B_PRINT test OK',$0d,$0a,0
        .even

        .end    start
EOF
```

`_B_COLOR`($22、d1.w=表示属性)・`_B_LOCATE`($23、d1.w=桁/d2.w=行)・`_B_PRINT`
($21、a1.l=文字列アドレス)の3コールを使用。実行するとカーソルが(0,1)に移動してから
`IOCS B_PRINT test OK`が表示される（`B_LOCATE`でカーソル位置を動かすため、直前の
コマンド入力エコーとは別の位置に出力される点に注意）。

再現手順はDOSコール版と同じ（HOSTDRV(D:)経由でホスト側から配置→`HAS`→`HLK`→実行）。

**HOSTDRVの不具合の記録（8文字ちょうどのベース名で個別ファイルアクセス失敗。2026-08-12に修正済み、以下は修正前の記録）**:
ホスト側のファイル名を`IOCSTEST.S`（拡張子除くベース名が8文字ちょうど）にすると、
`DIR D:`の一覧には表示されるのに`TYPE D:\IOCSTEST.S`・`COPY D:\IOCSTEST.S ...`・
`HAS IOCSTEST.S`が軒並み「ファイルがありません」で失敗する（`DIR D:\IOCSTEST.S`単体
指定でも0件）。ベース名を7文字の`IOCSTST.S`に変更したところ問題なく読み書きできた。
HOSTDRV.SYSまたはエミュレータ側`hostdrv`実装の8.3ファイル名境界バグだった（2026-08-12に`src/peripheral/hostdrv_remote.c`の`namests_read_pattern()`を修正して解決）。
修正前の回避策: HOSTDRV経由で転送するホスト側ファイル名は拡張子を除く
ベース名を8文字ちょうどにしない（7文字以下または実際にX68000側へコピーする際の
最終ファイル名を先に8文字未満にリネームしておく）。

### 検証メモ

- 2026-08-12: `arc/HAS06091.LZH`から`HAS060.X`を`HAS.X`にリネームしHDS(`\BIN`)へ導入済み。
- 2026-08-12: ix68kで起動・アセンブル動作を確認。`\WORK\HASTEST\HELLO.S`
  （当初はDOSコール`_PRINT`/`_EXIT`を直接`.dc.w`埋め込みした最小プログラム）を
  `HAS HELLO.S`でアセンブルし`HELLO.O`（110バイト）を生成、`HLK HELLO.O`でリンクした
  `HELLO.X`を実行して文字列表示を確認（[[HLK.R]]との組み合わせ検証を兼ねる）。
- 2026-08-12: [[LIBC]]導入後、`doscall.mac`使用版（`DOS __PRINT`/`DOS __EXIT`）に
  書き換えて再検証。HOSTDRV(D:)経由でホスト側から直接ファイル転送する方法も確認
  （`COPY D:\HELLO.S C:\WORK\HASTEST\`）。同一の`HELLO.O`サイズ(110byte)・同一動作を
  確認し、マクロ展開が旧方式のF-line直接埋め込みと等価であることを実証。
- 2026-08-12: `iocscall.mac`版（`IOCS __B_COLOR`/`IOCS __B_LOCATE`/`IOCS __B_PRINT`)
  を`\WORK\HASTEST\IOCSTST.S`として作成・ix68k検証。`HAS`→`HLK`→実行で
  `IOCS B_PRINT test OK`がカーソル移動先(0,1)に正しく表示されることを確認。
  検証中にHOSTDRV(D:)の8文字ちょうどベース名バグを発見（詳細は上記セクション）。
- 出典アーカイブ: `HAS06091.LZH`（106.4KB、`README.DOC`/`HAS060.DOC`/`HAS060.X`/
  `DEBUGGED.DOC`/`KNOWNBUG.DOC`/`HISTORY.DOC`/`PROJECTS.DOC`/`HANOI.S`/`K_MACRO.MAC`を同梱）。
- 未検証: 全スイッチ、`doscall.mac`/`iocscall.mac`の他のコール定義（今回使用したもの以外）。
