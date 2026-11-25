---
exe: APICG.r（本体はビューア。本HDSでは開発用ライブラリとして導入）
種別: freeware
作者: GORRY
版: v2.12A
出典: http://retropc.net/x68000/software/graphics/pic/apicg/ 添付 APICGLIB.doc
判別: -（配布アーカイブ apg_212a.lzh 収録版のみ確認）
検証状態: ✅ [[GCC.X]]から実際にPICファイルをロード・表示して動作確認済み
最終更新: 2026-08-21
---
# APICG.r（多機能画像ローダ、Cライブラリ APICGLIB.a）

> X68000用の多機能画像ローダ。PIC/APIC/MAG/MAKI/TIFF/BMP等のフォーマットの
> ロード・セーブに対応します。本HDSではビューア本体(APICG.r)ではなく、付属の
> Cライブラリ`APICGLIB.a`＋ヘッダ`apicglib.h`を、自作プログラムからPICファイルを
> 直接ロードする用途で導入しています。

---

## リファレンス（一次資料要約）

### 共通引数（`PICLOAD()`系ローダー）

書式:

```c
#include <apicglib.h>

int PICLOAD(
    UWORD *LoadBuf,      /* 展開先。通常$C00000(G-RAM)を直接指す */
    char *FileName,      /* ロードするファイル名（ディレクトリ名/拡張子の補完なし） */
    long LoadX, long LoadY,  /* 展開開始位置。負の場合はファイル内の情報を使う */
    UBYTE *FileBuf, long FileBufSize,  /* ファイル読み込みバッファ（128バイト以上）。
                                          ロード後は画像ファイル情報(APICGINFO)も
                                          ここに返る */
    ULONG Flag            /* 制御フラグ（下表） */
);
```

| フラグ | bit | 説明 | 検証 |
|--------|-----|------|----|
| `APF_NOINITCRT` | 0 | 1で画面モードを初期化しない（未指定なら画像ファイルの埋め込み情報から自動設定される） | 📖 |
| `APF_NOCLRBUF`  | 1 | 1で展開先バッファをクリアしない | 📖 |
| `APF_NOPRFC`    | 2 | 1でファイル名とコメントの表示バナーを出さない | ✅ |
| `APF_NOADJUST`  | 3 | 1で拡張情報による画像補正を行わない | 📖 |
| `APF_NOPRC`     | 4 | 1でコメントのみ表示しない | 📖 |
| `APF_LDIMAGE`   | 15 | 1でG-RAMでなく矩形バッファへロードする | 📖 |

`PICLOAD()`は「現在もっとも普及しているX68000の32768色PIC」専用の高速ローダー
（明るさ変更不可）。ほかに`APICLOAD()`（APIC・PICを高速ロード、明るさ変更不可、
`APICWorkBuf`引数が追加で必要）、`APICGLOAD()`（対応形式すべてをロード、
明るさ変更可）等があります。

戻り値は成功時0以上（Bright系拡張情報の有無等）、失敗時は負のエラーコード
（`_ERRAPG_*`、`apicglib.h`参照）。

---

## ix68kでの利用

### 入手先

http://retropc.net/x68000/software/graphics/pic/apicg/ から`apg_212a.lzh`
としてダウンロード（v2.12A、351.6KB）。**`arc/retropc.net/x68000/software/
graphics/pic/apicg/apg_212a.lzh`にローカル保存済み**（2026-08-21取得）。

配布は入れ子LZH形式（`apg_212a.lzh`の中に`APGX212A.Lzh`＝実行ファイル一式、
`APGL212A.Lzh`＝Cライブラリ一式、`APGB212A.Lzh`＝X-BASIC用、`APGD212A.Lzh`＝
文書、`include.Lzh`・`lib.lzh`・`tools.Lzh`等）。本HDSでは`APGL212A.Lzh`内の
`LIB/APICGLIB.a`と`INCLUDE/apicglib.h`のみを導入している（ビューア本体
`APICG.r`本体・X-BASIC用は未導入、開発目的外のため）。

### インストール（SYSTEM_HD.HDSへの導入手順）

```sh
# 1. ダウンロード（arc/にあればそれを使う）
curl --insecure -L --create-dirs \
  -o "arc/retropc.net/x68000/software/graphics/pic/apicg/apg_212a.lzh" \
  "http://retropc.net/x68000/software/graphics/pic/apicg/apg_212a.lzh"

# 2. 展開（入れ子LZH。unar で一括展開可、要 brew install unar）
mkdir apg_212a_extracted && cd apg_212a_extracted
unar -f "../arc/retropc.net/x68000/software/graphics/pic/apicg/apg_212a.lzh"
cd apg_212a
unar -f -o APGL212A APGL212A.Lzh   # → APGL212A/APGL212A/LIB/APICGLIB.a
                                   #   APGL212A/APGL212A/INCLUDE/apicglib.h
cd ../..

# 3. HDSへ配置（エミュレータ停止中、またはフロッピー経由でホットコピー。
#    エミュレータ稼働中にHDSを直接hu68diskで書き換えないこと）
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS \
  apg_212a_extracted/apg_212a/APGL212A/APGL212A/LIB/APICGLIB.a /LIB/APICGLIB.a
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS \
  apg_212a_extracted/apg_212a/APGL212A/APGL212A/INCLUDE/apicglib.h /INCLUDE/apicglib.h
```

元のファイル名は`LIB/APICGLIB.a`（ベース名は大文字、拡張子は小文字）・
`INCLUDE/apicglib.h`（全て小文字）で、互いに大文字小文字の付け方が異なる
（配布アーカイブそのままの表記、ix68kで`hu68disk list`により確認済み）。両方とも
**配布時の表記のまま**、リネームせずに配置しています。

[[GCC.X]]の`-l<name>`は`GCC_LIB=.a`設定のもとで`lib<name>.a`を`\LIB`から
探す規約（[[LIBC]]の`libiocs.a`等と同じ）だが、`APICGLIB.a`はこの規約に
合わせて`lib`プレフィックスを付けてリネームする**必要は無い**。GCCの
リンク対象には`-l<name>`だけでなく`.a`ファイルをフルパスで直接指定できる
ため、ビルド時に`C:\LIB\APICGLIB.a`とそのまま指定すればよい（ix68kで
`-lapicglib`用のリネーム版・フルパス直接指定版の両方をビルド・実行して
同一サイズのバイナリになることを確認済み。リネームは不要な手間だった）。

`APICGLIB.a`のリンクには`-ldos`（DOSコールラッパー、`_dos_print`等）も
合わせて必要になることがある（[[LIBC]]の`libdos.a`。`_dos_print`未定義で
リンクエラーになった実例あり）。

### 使用例（`../pal65536/PAL65536.C`）

```c
#include <apicglib.h>

static unsigned char file_buf[2048];
int r = PICLOAD((UWORD *)0xc00000, filename, -1, -1,
                file_buf, sizeof(file_buf), APF_NOPRFC);
```

ビルド:

```
GCC -O PAL65536.C -liocs -ldos C:\LIB\APICGLIB.a -o PAL65536.X
```

`LoadX`/`LoadY`に`-1`を渡すとファイル埋め込みの位置情報を使います。
`APF_NOINITCRT`を立てていないため、画面モード（CRTMOD相当）はPICファイルの
色数・解像度に応じて自動設定されます。

### 検証メモ

- 2026-08-21: `../pal65536/PAL65536.C`から`PICLOAD()`で
  `C:\WORK\MECHA.PIC`（56451バイト、既存アセット）をix68kで
  ロード・表示し、スクリーンショットで正常表示を確認。
- 出典アーカイブ: `apg_212a.lzh`（351.6KB、`APGX212A.Lzh`/`APGL212A.Lzh`/
  `APGB212A.Lzh`/`APGD212A.Lzh`/`include.Lzh`/`lib.lzh`/`tools.Lzh`/
  `Makefile`/`APGVER.MAK`/`APICGENV.BAT`/`FORMATS.DOC`/使用許諾規定.doc/
  `verup.doc`/`readme.doc`を同梱）。
