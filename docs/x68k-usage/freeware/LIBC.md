---
exe: -（実行ファイルではなくヘッダ・ライブラリ集）
種別: freeware
作者: DON / 立花えり子（原作: Project LIBC Group）
版: LIBC 1.1.32A ぱっち DON版 その4 (2001-05-05)
出典: 添付README.DOC/USAGE.DOC/LIBRARIES/OPTION。配布元
   http://retropc.net/x68000/software/develop/lib/libcdon/ （DON版パッチ本体）、
   http://retropc.net/x68000/software/develop/lib/libc1132a/ （原版、libgnu.a/libbas.a補完用）、
   http://retropc.net/x68000/software/develop/c/gcc2/ （libgcc.a補完用、GCC2の基本キット）
判別: -（配布アーカイブ libcd4.tgz + libc32b.tgz + G3_20.LZH 収録版のみ確認）
検証状態: ✅ [[GCC.X]]から実際にコンパイル・リンク・実行して動作確認済み
最終更新: 2026-08-19
---
# LIBC（Cライブラリ、ヘッダ+`.a`アーカイブ集）

> X68000用フリーCライブラリ。オリジナルLIBC 1.1.32Aの不具合修正・改良を加えたDON氏パッチ版。stdio/stdlib/string等のANSI C標準関数に加え、DOSコール・IOCSコールのCラッパー、マルチバイト文字列処理等を含みます。[[GCC.X]]（真里子版GCC）のリンク先ライブラリとして導入。

---

## リファレンス（一次資料要約）

### 主要ライブラリの内容

各`.a`が何をまとめたものか（同梱`LIBRARIES`文書・`lib.tgz`の内訳参照）:

| ライブラリ | 内容 |
|-----------|------|
| `libc.a` | 標準Cライブラリ本体（stdio/stdlib/string/ctype/time等） |
| `libdos.a` | DOSコールをCから呼ぶためのラッパー（`_dos_*`系関数） |
| `libiocs.a` | IOCSコールをCから呼ぶためのラッパー（`_iocs_*`系関数） |
| `libsuper.a` | スーパーバイザモード関連（`_superman`） |
| `libscsi.a` | SCSIコマンドを直接発行する関数群（`_scsi_*`系） |
| `libsignal.a` | シグナル処理（`signal`/`sigaction`等） |
| `libw.a` | ワイド文字関連 |
| `libmb.a` | マルチバイト文字列処理（`mbs*`系関数） |
| `libcplus.a` | C++の静的コンストラクタ/デストラクタ対応。`.ctor`/`.dtor`を使う場合は`-lcplus`でのリンクが必須で、忘れると`Using ctor/dtor without .doctor/.dodtor.`エラーになる |
| `libtz.a` | タイムゾーン関連 |
| `libprof.a` | プロファイリング関連 |
| `libgnu.a` | GCCのランタイムサポート（整数⇔浮動小数点変換ヘルパーの`fixdfsi.o`/`floatsidf.o`等、C++の`new`/`delete`用`bltin_new.o`/`bltin_del.o`）。原版1.1.32Aのみに同梱でDON版パッチには含まれない（本HDSでの補完手順は後述） |
| `libbas.a` | 原版のみに同梱（内容は未調査、DON版パッチには含まれないため補完導入） |
| `libgcc.a` | GCC2から補完したもの。`___ltdf2`など`libgnu.a`にない浮動小数点比較ヘルパーを含み、`printf`等の書式変換で必要になる（`-lgcc`指定時のみリンクされます。詳細は後述） |

スタートアップルーチン（`_start`/`_main`/`_exit`等）は`libc.a`に含まれているため、
Unix系ツールチェインのような別建ての`crt0.o`は不要（`libc.a`をリンクするだけで
スタートアップまで解決する構成）。

---

## ix68kでの利用

### 入手先

http://retropc.net/x68000/software/develop/lib/libcdon/ から`libcd4.tgz`
としてダウンロード可能（その4、2001年版）。
**`arc/libcd4.tgz`にローカル保存済み**（2026-08-12取得）。再検証・再インストール時は
retropc.netへ再度アクセスせず、まずこちらを使うこと。

配布は入れ子tar.gz形式（`libcd4.tgz`の中に`doc.tgz`/`include.tgz`/`lib.tgz`/`man.tgz`/
`src.tgz`）。`src.tgz`（ソース一式、ライブラリのビルドに使用）は未取得（本体利用に
不要なため、他ツールと同じ方針で導入していない）。

### ランタイムライブラリ`libgnu.a`（2026-08-12 追加導入・解決済み）

`README.DOC`（配布アーカイブ同梱）は「C言語やObjective-Cで使用する場合、
`libgcc version x.xx patchlevel 6`が必要」と明記していたが、`libgcc`という名前の
ファイルはretropc.net上ではGCC2/GCC 2.95.2系列（`g2as.x`/`g2lk.x`という別アセンブラ・
リンカ必須、本HDSの[[HAS.X]]/[[HLK.R]]とは非互換）にのみ存在し、[[GCC.X]]（真里子版、
1.42ベース）用には別配布されていませんでした。

調査の結果、`GCC.DOC`が要求する実体は`gnulib.a`＝**`libgnu.a`**（DON版パッチには
含まれず、無印の`LIBC 1.1.32A`原版のみに同梱）と判明。`libgnu.a`の中身を確認した
ところ`fixdfsi.o`/`floatsidf.o`/`fxunsdfdi.o`等のGCC生成コード用整数⇔浮動小数点
変換ヘルパー、`bltin_new.o`/`bltin_del.o`（C++ new/delete）を含み、GCCランタイム
サポートライブラリそのものと確認できた。原版にのみ存在した`libbas.a`と合わせて
`\LIB`へ追加導入済み（他のライブラリはDON版パッチの方が新しいためそちらを優先）。

```sh
# 無印LIBC 1.1.32A原版（libc32b.tgz）からlibgnu.a/libbas.aのみ抜き出す
cp arc/libc32b.tgz ./libc32b.tgz
# 無ければ: curl -LO http://retropc.net/x68000/software/develop/lib/libc1132a/libc32b.tgz
mkdir libc32b_extracted && tar xzf libc32b.tgz -C libc32b_extracted
chmod -R u+rwX libc32b_extracted   # アーカイブ内ディレクトリのパーミッションが実行不可のため
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS libc32b_extracted/lib/libgnu.a /LIB
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS libc32b_extracted/lib/libbas.a /LIB
```

C++で使う場合はさらに`HAS060 v3.09+85+14[g2as]`・`HLK evolution v3.01+12[g2lk]`・
改造版`gcc 2.6.3`が必要（本HDSの[[HAS.X]]/[[HLK.R]]はバージョンが異なる可能性があり
未確認。C++は今回の導入目的外）。

#### `libgnu.a`だけでは不足していた分（`libgcc.a`をGCC2から補完、2026-08-12）

ix68kで[[GCC.X]]から`printf`を使うプログラムをコンパイル・リンクしたところ、
`_doprnt.o`（`libc.a`内、printf系列の実装）が浮動小数点比較ヘルパー`___ltdf2`を
要求し、これは`libgnu.a`（1993年版、変換ヘルパーのみ収録）には含まれていませんでした。

`___ltdf2`はGCC2（2.6.3ベース、`http://retropc.net/x68000/software/develop/c/gcc2/`、
`G3_20.LZH`同梱の`libgcc.a`）に収録されていることを確認。GCC2は本来
`g2as.x`/`g2lk.x`という別ツールチェイン向けだが、アーカイブのコンテナ形式
（先頭バイト列`d1 00 00 00 00 02`のHAR形式）が[[LIBC]]の`.a`ファイルと同一であり、
実際に[[HLK.R]]で`libc.a`の後に指定してリンクしたところ問題なく解決できた
（客観的なオブジェクト形式の互換性をix68kテストで確認）。

```sh
cp arc/G3_20.LZH ./G3_20.LZH
# 無ければ: curl -LO http://retropc.net/x68000/software/develop/c/gcc2/G3_20.LZH
unar -o ./g3_20_extracted G3_20.LZH
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./g3_20_extracted/G3_20/libgcc.a /LIB
```

使用時は`GCC`コマンドラインに`-lgcc`を追加する（`GCC_LINK`環境変数への埋め込みでは
機能しませんでした。詳細は`freeware/GCC.X.md`「`printf`等の浮動小数点書式を使う場合は
`-lgcc`が追加で必要」参照）。`libgccsx.a`（同梱、SX-Window版）は未使用・未導入。

### インストール（SYSTEM_HD.HDSへの導入手順）

CONFIG.SYS登録不要。ヘッダ・ライブラリを所定の場所に置き、環境変数で参照先を
指定する（後述）。

```sh
# 1. ダウンロード（arc/libcd4.tgzにローカル保存済みならそれを使い、再ダウンロードしない）
cp arc/libcd4.tgz ./libcd4.tgz
# 無ければ: curl -LO http://retropc.net/x68000/software/develop/lib/libcdon/libcd4.tgz

# 2. 展開（入れ子tar.gz。ホスト側でtar/gzip、または`unar`で一括展開可）
mkdir libcd4_extracted && tar xzf libcd4.tgz -C libcd4_extracted
cd libcd4_extracted
for f in include lib man doc; do mkdir -p "$f" && tar xzf "$f.tgz" -C "$f"; done
# → include/libc-d4/include/*.h（115本）+ include/libc-d4/include/sys/*.h（43本）
#   + doscall.mac/iocscall.mac/doscall.equ
# → lib/libc-d4/lib/*.a（libc.a等11本）
# → man/libc-d4/man/<category>/*.3（19カテゴリ、計120本）
# → doc/libc-d4/doc/*（USAGE.DOC/LIBRARIES/OPTION等24本）
# 注: アーカイブ内のディレクトリはパーミッションが実行不可(drw-r--r--)で
#   固められているため、`chmod -R u+rwX`で読み取り可能にしてから中を辿ること

# 3. ヘッダをHDSの\INCLUDEへコピー（hu68disk、エミュレータ停止中に実行）
tools-host/hu68disk/hu68disk mkdir disk/SYSTEM_HD.HDS /INCLUDE
tools-host/hu68disk/hu68disk mkdir disk/SYSTEM_HD.HDS /INCLUDE/sys
for f in libcd4_extracted/include/libc-d4/include/*.h \
         libcd4_extracted/include/libc-d4/include/*.mac \
         libcd4_extracted/include/libc-d4/include/*.equ; do
  tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS "$f" /INCLUDE
done
for f in libcd4_extracted/include/libc-d4/include/sys/*.h; do
  tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS "$f" /INCLUDE/sys
done

# 4. ライブラリをHDSの\LIBへコピー
tools-host/hu68disk/hu68disk mkdir disk/SYSTEM_HD.HDS /LIB
for f in libcd4_extracted/lib/libc-d4/lib/*.a; do
  tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS "$f" /LIB
done

# 5. ドキュメント・manをHDSの\DOC\LIBCへコピー
tools-host/hu68disk/hu68disk mkdir disk/SYSTEM_HD.HDS /DOC/LIBC
for f in libcd4_extracted/doc/libc-d4/doc/*; do
  tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS "$f" /DOC/LIBC
done
tools-host/hu68disk/hu68disk mkdir disk/SYSTEM_HD.HDS /DOC/LIBC/MAN
for d in libcd4_extracted/man/libc-d4/man/*/; do
  cat=$(basename "$d" | tr '[:lower:]' '[:upper:]')
  tools-host/hu68disk/hu68disk mkdir disk/SYSTEM_HD.HDS "/DOC/LIBC/MAN/$cat"
  for f in "$d"*; do
    tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS "$f" "/DOC/LIBC/MAN/$cat"
  done
done
```

### ディレクトリ構成（本HDS導入結果）

| パス | 内容 | ファイル数 |
|------|------|-----------|
| `\INCLUDE` | ヘッダ（`stdio.h`等）+ `doscall.mac`/`iocscall.mac`/`doscall.equ` | 115+3 |
| `\INCLUDE\sys` | `sys/`配下ヘッダ（`sys/types.h`等） | 43 |
| `\LIB` | ライブラリ`.a`（DON版10本+原版補完2本+GCC2補完1本: `libc.a`/`libdos.a`/`libiocs.a`/`libsuper.a`/`libprof.a`/`libscsi.a`/`libcplus.a`/`libw.a`/`libsignal.a`/`libtz.a`/`libmb.a`/`libgnu.a`/`libbas.a`/`libgcc.a`） | 14 |
| `\DOC\LIBC` | 導入解説文書（`USAGE.DOC`=関数索引、`LIBRARIES`=実装状況、`OPTION`=ビルドオプション、`COMPILE.DOC`等） | 24 |
| `\DOC\LIBC\MAN` | man形式リファレンス（19カテゴリ、`CTYPE`/`STDIO`/`STDLIB`/`STRING`/`SYS_DOS`/`SYS_IOCS`等のサブディレクトリ） | 120 |

`\DOC\LIBC\MAN`はオリジナルLIBCから変更・新規追加された関数のみのマニュアルであり
（`README.DOC`に明記）、C標準関数の網羅的なリファレンスではない点に注意。

### 環境変数（[[GCC.X]]・[[HAS.X]]・[[HLK.R]]から参照される設定、`AUTOEXEC.BAT`に設定済み）

```
SET include=C:\INCLUDE
SET lib=C:\LIB
SET GCC_LIB=.a
SET DOSEQU=C:\INCLUDE\doscall.equ
```

`include`はHASのインクルード検索先、`lib`はHLKの`-l<lib>`使用時の検索先、
`GCC_LIB=.a`はGCCドライバがライブラリ名を`lib<name>.a`形式で解決するための指定、
`DOSEQU`はHuman68KモードでのアセンブルにDOSコールシンボルを使う際の定義ファイル
指定（詳細は`freeware/GCC.X.md`参照、[[HLK.R]]の設定と併せて記載）。

### 検証メモ

- 2026-08-12: `arc/libcd4.tgz`から`\INCLUDE`（158ファイル）・`\LIB`（11ファイル）・
  `\DOC\LIBC`（24ファイル）・`\DOC\LIBC\MAN`（19カテゴリ、120ファイル）を導入済み。
- 2026-08-12: `README.DOC`が要求する`libgcc`相当の実体が`libgnu.a`（DON版パッチには
  なく原版1.1.32Aのみに同梱）と判明。`arc/libc32b.tgz`から`libgnu.a`・`libbas.a`を
  追加導入し`\LIB`が13ファイルに（Web検索でnote.com記事「X68000エミュ（３）GCC
  真里子編」の実運用手順と符合することも確認）。
- 2026-08-12: ix68kで[[GCC.X]]から`puts`版・`printf`版の2種類のCソースを
  コンパイル・[[HAS.X]]でアセンブル・[[HLK.R]]でリンクし、それぞれ実行して正常動作を
  確認。`printf`版は`libgcc.a`（GCC2から補完）の追加が必要と判明（`-lgcc`指定）。
  再現手順は`freeware/GCC.X.md`「動作確認の再現手順」参照。
- 出典アーカイブ: `libcd4.tgz`（461.4KB、`doc.tgz`/`include.tgz`/`lib.tgz`/`man.tgz`/
  `src.tgz`と`README`/`README.DOC`/`README.TXT`/`CHANGELOG.TXT`/`NEWS.TXT`/`Makefile`/
  `.cvsignore`を同梱）、`libc32b.tgz`（145.9KB、原版1.1.32Aバイナリセット。
  `libgnu.a`/`libbas.a`の抽出元としてのみ使用、他ファイルはDON版パッチが新しいため不使用）、
  `G3_20.LZH`（256.1KB、GCC2基本キット。`libgcc.a`の抽出元としてのみ使用）。
