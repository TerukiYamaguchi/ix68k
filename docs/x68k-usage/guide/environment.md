# 環境を確認する

ix68k はホスト上のエミュレータ、Human68k はその中で動くX68000用OSです。両者のパスと
ファイルは別物として扱います。

1. ix68k、IPLROM、CGROM、起動媒体を用意します。手順は
   [ix68k マニュアル](../manual.md) の「準備するもの」と [QUICKSTART](https://github.com/TerukiYamaguchi/ix68k/blob/main/QUICKSTART.md) を参照します。
   ソースからビルドする場合は [ビルド・実行手順](https://github.com/TerukiYamaguchi/ix68k/blob/main/docs/ix68k-internals/build.md) に従います。
2. `ix68k.cfg` の `rom`、`cgrom`、`fd0` または `scsi_disk0` を確認して ix68k を起動します。
   `ix68k.cfg` は、カレントディレクトリ、実行ファイルの隣、OSのユーザー領域の順に探します。
   macOSの `ix68k.app` をFinderから起動した場合は `~/Library/Application Support/ix68k/` を使います。
3. `A>` や `C>` のプロンプトが表示されれば、Human68kがコマンドを受け付ける状態です。

ROM、CGROM、Human68k媒体はリポジトリにもリリースZIPにも同梱されません。入手条件は
[QUICKSTART](https://github.com/TerukiYamaguchi/ix68k/blob/main/QUICKSTART.md) の「ROM・ディスクイメージを用意する」を参照します。
