# X68000アプリをネイティブGCC.Xでビルドするノウハウ

X68000上には実際に動くネイティブGCC（`freeware/GCC.X`、真里子版GCC 1.42、
`docs/x68k-usage/freeware/GCC.X.md`）が導入済みで、Cソースを X68000 上で直接
ビルドできます（GhostEDで検証済み）。クロスビルドとの切り替え手順は
[ネイティブビルドとクロスビルドを切り替える](switching_build_environments.md) を参照してください。

以下は、ネイティブGCC.Xでビルドする際の固有要件であり、ix68k本体のビルド手順ではありません。

## 1. DOSCALLヘッダ

ネイティブビルドでは `#include <doslib.h>` を使います。`doslib.h` は内部で
`sys/dos.h`をインクルードし、一部の関数名を差し替えるマクロを持ちます（例:
`_dos_open`→`TSOpen`）。
IOCSコール（`_iocs_*`）を使う場合のヘッダ名は本検証では未確認
（`iocslib.h`と推測されるが、実際に使う場合は要確認）。

ヘッダ以外のCソースコード自体は**無修正**で通った（GhostEDの658行、`printf`/
`sprintf`/`malloc`/`strtol`等の標準ライブラリ関数を含む）。

## 2. 文字コード変換（UTF-8 → CP932）

真里子版GCC 1.42は1990年代のツールで、UTF-8のマルチバイトシーケンスを正しく
扱えません。日本語コメント・文字列リテラルを含むソースは事前にCP932
（Shift-JIS）へ変換する必要があります。

```sh
iconv -f UTF-8 -t CP932 source_utf8.c > SOURCE.C
```

**落とし穴: 波ダッシュ「〜」(U+301C) がCP932の標準マッピングに存在せず変換エラー
になる**（`iconv: iconv(): Illegal byte sequence`）。これはUnicode⇔Shift-JISの
よくある表記ゆれ問題で、Microsoft社のCP932マッピングでは同じ見た目の
全角チルダ「～」(U+FF5E) だけが変換対象になっています。事前に置換してから
変換すること:

```python
text = text.replace('〜', '～')  # U+301C → U+FF5E
```

（`iconv`単体の`//TRANSLIT`オプションでは解決しません。実際に確認済み。）

## 3. ビルドコマンド

```
GCC -O <ファイル名>.C -lgcc -ldos -o <出力名>.X
```

- ソースファイル名は8.3形式・大文字（例: `GHOSTED.C`）。
- `-lgcc`: `printf`を使う場合は（floatを使わない書式でも）必須
  （`docs/x68k-usage/freeware/GCC.X.md`「動作確認の再現手順」参照。`puts`のみ
  なら不要）。
- `-ldos`: `_dos_*`関数（`libdos.a`）を使う場合は必須。忘れると
  `Undefined symbol(s) in ...`で`__dos_open`等（Cシンボルの前にコンパイラが
  付与する先頭アンダースコアが二重になった形）が列挙されてリンクエラーになります。

## 4. ビルド・配置場所

ビルドはMCPの`run_command`でX68000上のGCC.Xを直接呼び出して行います。ソース転送・
成果物の回収は**HOSTDRV**（`F:\WORK\`等、`ix68k.cfg`の`hostdrv_dir`）を使うと
リセット不要で速い（固定HDSへ`hu68disk`で直接書き込む方式は、稼働中の
Human68kにreset無しでは反映されないため非推奨、`disk`ツールの制約と同様）。

## 実例

`tools-x68k/ged/native/`（GhostED本体`../ghosted.c`のネイティブビルド版
`GHOSTED.C`）を参照。
