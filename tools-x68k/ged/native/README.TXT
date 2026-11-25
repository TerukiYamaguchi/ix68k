			   GhostED ネイティブビルド版


  ../ghosted.c（elf2x68kクロスコンパイラ用、UTF-8）を X68000 上で動く
freeware/GCC.X（真里子版GCC 1.42、docs/x68k-usage/freeware/GCC.X.md）で
そのまま実機/エミュレータ上でビルドできるかを検証した副産物（Issue #148）。

  GHOSTED.Cはghosted.cと論理的に同一（コード自体の変更なし）。異なるのは
以下の2点のみ:

	1. #include <x68k/dos.h> → #include <doslib.h>（ヘッダ名の違い。
	   _dos_*関数群はどちらも同じ名前で提供されている）。
	2. 文字エンコーディング: UTF-8 → CP932（Shift-JIS）。

  一般的な移植手順（他のホストビルドプログラムをネイティブビルドする際の
手順）はdocs/x68k-usage/development/native_build.mdを参照。


● ビルド手順（X68000上、MCP経由）

	GCC -O GHOSTED.C -lgcc -ldos -o GED.X

	・-lgcc: printf使用に必須（floatを使わない場合でも、真里子版GCC
	  の実装上必要。docs/x68k-usage/freeware/GCC.X.md参照）。
	・-ldos: _dos_*関数（libdos.a）のリンクに必須。忘れるとUndefined
	  symbol(s)で__dos_open等（Cシンボルにコンパイラが付与する先頭ア
	  ンダースコアが二重になった形）が列挙される。


● 検証済み

  2026-08-27、F:\WORK\ghosted_native\（HOSTDRV経由）でコンパイル・リンク
とも警告・エラーなく成功。新規ファイル作成・挿入モード・保存まで実機（エ
ミュレータ）で動作確認済み。C:\BIN\GED.Xとして導入済み（クロスビルド版を
DELし、こちらをGED.Xにリネームして置き換えた。以後GEDコマンドはこのネイ
ティブビルド版を指す）。
