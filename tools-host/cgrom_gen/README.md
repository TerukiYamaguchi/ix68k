# cgrom_gen — 互換CGROM生成ツール

バージョン 1.0（`--version` で表示）。

X68000実機のCGROM（キャラクタジェネレータROM）は著作権上フリー配布されていない。
本ツールは、ユーザー手元のゴシック体・明朝体フォント（TTF/OTF/TTC）から実機互換の
CGROMバイナリ（768KB）をその場で合成する。**フォント自体はこのリポジトリに同梱しない**
（ライセンス上の理由。配布するのは本スクリプトのみで、フォントは各自のマシンにある
ものを指定する）。

## クリーンルームであることについて

アドレス配置・ビットパッキング仕様は『インサイドX68000』（アスキー出版局）4章
「CGROM」（p.218-226、図39〜図46）から直接書き起こした。WinX68k/XM6Util等、既存の
CGROM合成ツールのソースコードは一切参照・移植していない（ライセンス不明瞭なコードを
混入させないため）。JIS X0208→Unicode変換もPython標準ライブラリの`euc_jp`コーデックを
使っており、他ツールの変換テーブルには依存しない。

## 使い方

```sh
# フォント省略時はOS標準フォントを自動検出する
python3 tools-host/cgrom_gen/cgrom_gen.py --output rom/CGROM_GEN.DAT

# 明示指定したい場合
python3 tools-host/cgrom_gen/cgrom_gen.py \
  --gothic "<ゴシック体フォントのパス>" \
  --mincho "<明朝体フォントのパス>" \
  --output rom/CGROM_GEN.DAT
```

リリースZIPには、PyInstallerで固めた`ix68k-cgrom-gen`（Windowsは`.exe`）を入れる。Pythonは要らず、
引数は上と同じ（`ix68k-cgrom-gen --output rom/CGROM_GEN.DAT`）。

出力は必ず786432バイト（`0xC0000`）。数秒〜十数秒で完了する。

### フォントの自動検出

`--gothic`/`--mincho`を省略すると、OS別の既知パスを優先順に探し、最初に実在した
ものを使う（XEiJ本体`PrinterPort.java`のFNT_GOTHIC_FAMILIES/FNT_MINCHO_FAMILIESと
同じ発想の優先順位リストを、Pillow用にファイルパス直接指定へ書き換えたもの。
`cgrom_gen.py`冒頭の`FONT_CANDIDATES`参照）。すべて見つからなければ`fc-match`
（fontconfig、主にLinuxで利用可）を試し、それも失敗したらエラーで止まる。

| OS | ゴシック体の既定候補 | 明朝体の既定候補 |
|----|---------------|-----------|
| macOS | `/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc` | `/System/Library/Fonts/ヒラギノ明朝 ProN.ttc` |
| Windows | `C:\Windows\Fonts\msgothic.ttc`ほか | `C:\Windows\Fonts\msmincho.ttc`ほか |
| Linux | IPAゴシック/VLゴシック/Takaoゴシック/Noto Sans CJKの順に探索 | IPA明朝/Takao明朝/Noto Serif CJKの順に探索 |

Linuxで既定パスがすべて無い場合は、フリーの日本語フォント（IPAフォント・原ノ味
フォント等、いずれもJIS X0208を全域カバーし再配布可能なライセンス）を導入するか
`--gothic`/`--mincho`で明示指定する（例: Debian/Ubuntu系なら
`fonts-ipafont-gothic`/`fonts-ipafont-mincho`パッケージ）。

TTC（TrueType Collection、複数書体が1ファイルに同梱された形式）はPillowが最初の書体を
自動選択する。特定の書体を選びたい場合はTTF/OTF単体のファイルを指定すること。

### 推奨フォント（読みやすさ重視の場合）

自動検出されるOS標準フォント（ヒラギノ角ゴ/明朝、MS ゴシック/明朝等）はアウトライン
フォントのため、CGROMのような極小ピクセルサイズに縮小すると輪郭がぶれ、実機ROMより
読みにくくなる傾向がある（`local/investigation/cgrom_font_comparison/`に比較screenshot
あり）。**手打ちのドットビットマップフォントを使うと、実機に近い読みやすさになる**:

| フォント | 特徴 | 入手先 |
|---------|------|--------|
| **東雲フォント**（`JF-Dot-Shinonome16.ttf`/`JF-Dot-ShinonomeMin16.ttf`） | 16x16専用設計、パブリックドメイン | [自家製フォント工房](http://jikasei.me/font/jf-dotfont/) の一括アーカイブ内 |
| **jiskan16/24**（`JF-Dot-jiskan16.ttf`/`JF-Dot-jiskan24.ttf`） | EIAJ制作のJIS標準ビットマップフォント。16/24とも専用設計、全角部分パブリックドメイン | 同上 |

どちらも比較の結果ほぼ同等に読みやすく、上記のヒラギノ/MS系より明らかにくっきり
表示される。ダウンロードしたzipから該当ファイルだけ取り出して指定すればよい:

```sh
python3 tools-host/cgrom_gen/cgrom_gen.py \
  --gothic JF-Dot-Shinonome16.ttf --mincho JF-Dot-ShinonomeMin16.ttf \
  --output rom/CGROM_GEN.DAT
```

**美咲フォント**（門真なむ氏、M+ FONTS LICENSE、[littlelimit.net/misaki.htm](https://littlelimit.net/misaki.htm)）
は8x8専用設計のため、16x16漢字に使うと輪郭が粗くなり非推奨（8x8(ANK)だけ差し替えたい
場合の選択肢としては有効。現状`cgrom_gen.py`はサイズ単位でフォントを切り替える
オプションが無いため、使う場合は`--gothic`に指定して16x16も同じフォントで生成される
点に注意）。

東雲フォントは、自家製フォント工房の配布アーカイブ（`jfdotfont-20150527.zip`）から取得して、
`--gothic` / `--mincho` などに指定する。

### ix68k.cfgでの指定

```
cgrom = rom/CGROM_GEN.DAT
```

## 仕組み（概要）

CGROM内には英数字(ANK)4種・漢字(全角)2種、計6種類のフォントサイズがある。本ツールは
半角8x8/8x16と全角16x16を指定のゴシック体、半角12x12/12x24と全角24x24を指定の明朝体で
生成する（実機のROMがゴシック系・明朝系の2書体構成であることに合わせた区分け）。

| サイズ | 対象 | 文字数 | オフセット | 1文字あたり |
|--------|------|--------|-----------|------------|
| 16x16 | 漢字(非漢字752+第一水準3008+第二水準3478) | 7238 | `$000000`〜 | 32B |
| 8x8 | 半角(ANK) | 256 | `$03A000` | 8B |
| 8x16 | 半角(ANK) | 256 | `$03A800` | 16B |
| 12x12 | 半角(ANK) | 256 | `$03B800` | 24B |
| 12x24 | 半角(ANK) | 256 | `$03D000` | 48B |
| 24x24 | 漢字(同上) | 7238 | `$040000`〜 | 72B |

半角(ANK)は0x20-0x7Eの英数記号(JIS X0201ラテン)と0xA1-0xDFの半角カタカナのみ生成、
それ以外の未割当コードは空白のまま残す。漢字はJIS X0208の各コード点をフォントの
`euc_jp`デコード可否で判定し、未定義コード点（各行の空きマス）は自動的に空白になる。

## 制限・既知の課題

- グリフの配置は「フォントメトリクスに基づく中央揃え」という単純な方式（`anchor="mm"`）。
  実機ROMのような手作業チューニング済みの字面ではないため、見た目の細かい詰まり具合等は
  実機と一致しない
- 描画は`Image`を`"1"`(2値)モードで作りFreeTypeの2値ヒンティングに任せている。
  `"L"`(グレースケール)描画→閾値二値化と比べてストロークの欠け・太さのブレが少ない
  （実機での見た目比較は`local/investigation/cgrom_font_comparison/`参照）
- 指定フォントの設計サイズと生成対象サイズが大きくずれると輪郭が粗くなる（例: 8x8専用
  設計の美咲フォントを16x16漢字に使うとぼやける）。フォントは対象サイズに近い、または
  ジャストサイズで設計されたビットマップ系フォントを選ぶこと（上記「推奨フォント」参照）
- 指定フォントがJIS X0208の一部字体を持たない場合、そのグリフは欠けた(tofu)まま出力される
  （フォント選定側の責任。全域カバーする無償フォントの利用を推奨）
- 我々のエミュレータ（`src/bus/sram.c`）は現状CGROMの先頭512KBしか読み込んでいないため、
  24x24フォントの第二水準漢字後半は生成しても実際には使われない
