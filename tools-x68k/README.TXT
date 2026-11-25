			   tools-x68k/


  Human68k上で動かすドライバ・コマンド・検証用プログラムです。リリース
ZIPの tools-x68k/ と同じ名前で、ツールごとにディレクトリを分けています。
ホストOS上で動かすものはtools-host/ にあります。


● バージョン

	AIQ.X は Ver.0.9、そのほかのプログラムは Ver.1.0 です。


● 配布物に入るもの

	・hostdrv/ : HOSTDRV.SYS（ホストのフォルダをドライブとして使うド
	  ライバ）のソースとビルド済みバイナリ。
	・hostfile/ : HOSTFILE.X。tools-host/ix68k-hostfile/ と対でファ
	  イルを1本ずつ転送する。
	・joyget_test/ : JOYGET_TEST.X。ジョイパッドの割り当てを確かめる。
	・ged/ : GED.X（GhostED）。MCPから編集するためのed風ラインエディ
	  タ。
	・aiq/ : AIQ.X。自然言語の指示からコマンド列を作る。ホスト側は
	  tools-host/aiq-bridge/。
	・fntadr/ : FNTADR.X。ANKフォントの実アドレスをエミュレータへ報
	  告する。

  ビルド済みのバイナリ（*.x、HOSTDRV.SYS）はGitで管理しており、m68k-xelf
の無い環境でもZIPを作れる。


● 配布物に入らないもの

	・asmtest/ : 実CPU（エミュレータ上のX68000）で走らせる使い捨てア
	  センブリ試験。build.sh でビルドしてディスクへ書き込む。
