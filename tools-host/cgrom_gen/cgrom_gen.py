#!/usr/bin/env python3
"""互換CGROM生成ツール（クリーンルーム実装）。

X68000実機CGROMのアドレス配置・ビットパッキング仕様は『インサイドX68000』
（アスキー出版局）4章「CGROM」（p.218-226、図39〜図46）に基づく。ユーザー指定の
TrueType/OpenTypeフォント（ゴシック・明朝）から各サイズのビットマップを生成し、
実機互換のCGROM.DATバイナリ（768KB）を出力する。

JIS X0208 -> Unicode変換にはPython標準ライブラリのeuc_jpコーデックを使う
（JISコード上位・下位バイトそれぞれに0x80を立てるとEUC-JPの2バイト表現になる、
という一般に知られた技術的事実を利用しているだけで、他者のソースコードは
参照・移植していない）。

半角(ANK)フォント: 8x8/8x16はゴシック、12x12/12x24は明朝から生成する
（レターフェースはフォントセルより小さく収める。図39の記載に基づく）。
全角(漢字)フォント: 16x16はゴシック、24x24は明朝から生成する。

`--gothic`/`--mincho`を省略した場合、OS標準のフォントを既知のパスから自動検出する
（XEiJ本体`PrinterPort.java`のFNT_GOTHIC_FAMILIES/FNT_MINCHO_FAMILIESと同じ発想の
優先順位リストだが、ファイルパスは独自に調査したもの。Java版はフォント名を渡すだけで
OSが解決してくれるが、Pillowにその機能が無いためファイルパスを直接探す）。
"""
from __future__ import annotations

import argparse
import platform
import shutil
import subprocess
import sys
from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

VERSION = "1.0"
TOTAL_SIZE = 0x0C0000

# OS標準フォントの既知パス（優先順）。実在するものを先頭から採用する。
# macOS/Windowsはよくある標準インストール構成、Linuxはディストリのパッケージ既定パス。
FONT_CANDIDATES: dict[str, dict[str, list[str]]] = {
    "gothic": {
        "Darwin": [
            "/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc",
            "/System/Library/Fonts/ヒラギノ角ゴ ProN W3.ttc",
            "/Library/Fonts/ヒラギノ角ゴ ProN W3.ttc",
        ],
        "Windows": [
            r"C:\Windows\Fonts\msgothic.ttc",
            r"C:\Windows\Fonts\YuGothM.ttc",
            r"C:\Windows\Fonts\meiryo.ttc",
        ],
        "Linux": [
            "/usr/share/fonts/opentype/ipafont-gothic/ipag.ttf",
            "/usr/share/fonts/truetype/fonts-japanese-gothic.ttf",
            "/usr/share/fonts/truetype/vlgothic/VL-Gothic-Regular.ttf",
            "/usr/share/fonts/truetype/takao-gothic/TakaoGothic.ttf",
            "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        ],
    },
    "mincho": {
        "Darwin": [
            "/System/Library/Fonts/ヒラギノ明朝 ProN.ttc",
            "/Library/Fonts/ヒラギノ明朝 ProN W6.ttc",
        ],
        "Windows": [
            r"C:\Windows\Fonts\msmincho.ttc",
            r"C:\Windows\Fonts\YuMin.ttf",
        ],
        "Linux": [
            "/usr/share/fonts/opentype/ipafont-mincho/ipam.ttf",
            "/usr/share/fonts/truetype/takao-mincho/TakaoMincho.ttf",
            "/usr/share/fonts/opentype/noto/NotoSerifCJK-Regular.ttc",
        ],
    },
}

# fc-match(fontconfig)で問い合わせるときのパターン（既知パスが全滅した場合の最後の手段）
FC_MATCH_PATTERN = {"gothic": "sans-serif:lang=ja", "mincho": "serif:lang=ja"}


def resolve_font(style: str) -> str:
    """styleは'gothic'または'mincho'。既知パス→fc-matchの順で解決する。"""
    system = platform.system()
    for path in FONT_CANDIDATES.get(style, {}).get(system, []):
        if Path(path).is_file():
            return path

    if shutil.which("fc-match"):
        try:
            out = subprocess.run(
                ["fc-match", "--format=%{file}", FC_MATCH_PATTERN[style]],
                capture_output=True,
                text=True,
                timeout=5,
                check=True,
            ).stdout.strip()
            if out and Path(out).is_file():
                return out
        except (subprocess.SubprocessError, OSError):
            pass

    tried = FONT_CANDIDATES.get(style, {}).get(system, [])
    raise FileNotFoundError(
        f"{style}体フォントが見つかりません（{system}で既知パスを {len(tried)} 件試行、"
        f"fc-matchも失敗）。--{style} でフォントファイルを明示指定してください。"
    )

# JIS X0208 上位バイト範囲（下位は共通 0x21-0x7E, 94列）
JIS_HI_NONKANJI = (0x21, 0x28)  # 非漢字 8行 x 94 = 752文字
JIS_HI_LV1 = (0x30, 0x4F)  # 第一水準漢字 32行 x 94 = 3008文字
JIS_HI_LV2 = (0x50, 0x74)  # 第二水準漢字 37行 x 94 = 3478文字
JIS_LO_RANGE = (0x21, 0x7E)


@dataclass(frozen=True)
class FontSpec:
    name: str
    offset: int
    width: int
    height: int
    source: str  # "ank" | "nonkanji" | "lv1" | "lv2"
    style: str  # "gothic" | "mincho"

    @property
    def bytes_per_char(self) -> int:
        # 1ラインあたりのバイト数 x 高さ。1ラインは width ビットを
        # バイト境界まで0パディングして格納する（実機の仕様、図41〜46）。
        pad = (-self.width) % 8
        return (self.width + pad) // 8 * self.height


# 図40（CGROMアドレス配置）のオフセット。$F00000を0とした相対値。
SPECS: list[FontSpec] = [
    FontSpec("16x16-nonkanji", 0x000000, 16, 16, "nonkanji", "gothic"),
    FontSpec("16x16-lv1", 0x005E00, 16, 16, "lv1", "gothic"),
    FontSpec("16x16-lv2", 0x01D600, 16, 16, "lv2", "gothic"),
    FontSpec("8x8", 0x03A000, 8, 8, "ank", "gothic"),
    FontSpec("8x16", 0x03A800, 8, 16, "ank", "gothic"),
    FontSpec("12x12", 0x03B800, 12, 12, "ank", "mincho"),
    FontSpec("12x24", 0x03D000, 12, 24, "ank", "mincho"),
    FontSpec("24x24-nonkanji", 0x040000, 24, 24, "nonkanji", "mincho"),
    FontSpec("24x24-lv1", 0x04D380, 24, 24, "lv1", "mincho"),
    FontSpec("24x24-lv2", 0x082180, 24, 24, "lv2", "mincho"),
]


def jis_kanji_char(hi: int, lo: int) -> str | None:
    """JIS X0208 (上位,下位) バイトから文字を得る。未定義コードはNone。

    EUC-JPは上位・下位バイトそれぞれの最上位ビットを立てるだけでJIS X0208の
    2バイト表現になる（広く知られた技術的事実）。標準ライブラリのデコーダが
    未定義コードを例外で弾いてくれるので、除外テーブルを自前で持つ必要がない。
    """
    try:
        return bytes([hi | 0x80, lo | 0x80]).decode("euc_jp")
    except UnicodeDecodeError:
        return None


def ank_char(code: int) -> str | None:
    """半角コード(0x00-0xFF)から文字を得る。未割当はNone。

    0x20-0x7E: JIS X0201 ラテン部分（0x5Cは¥、0x7Eはオーバーラインだが、
    実装を単純にするためASCII互換文字で代用する）。
    0xA1-0xDF: 半角カタカナ（Shift_JISの1バイト表現がそのままJIS X0201に一致）。
    """
    if 0x20 <= code <= 0x7E:
        return chr(code)
    if 0xA1 <= code <= 0xDF:
        return bytes([code]).decode("shift_jis")
    return None


def iter_chars(spec: FontSpec):
    """(index, char_or_None) を順に返す。indexはCGROM内の文字順。"""
    if spec.source == "ank":
        for code in range(256):
            yield code, ank_char(code)
        return

    hi_range = {
        "nonkanji": JIS_HI_NONKANJI,
        "lv1": JIS_HI_LV1,
        "lv2": JIS_HI_LV2,
    }[spec.source]
    lo_lo, lo_hi = JIS_LO_RANGE
    index = 0
    for hi in range(hi_range[0], hi_range[1] + 1):
        for lo in range(lo_lo, lo_hi + 1):
            yield index, jis_kanji_char(hi, lo)
            index += 1


@lru_cache(maxsize=None)
def _load_font(path: str, pixel_height: int) -> ImageFont.FreeTypeFont:
    # Raqm（Pillowが実行時に読む任意のレイアウトエンジン）の有無で字形の位置がずれ、出力が環境で変わる。
    # 1文字ずつ描くだけでRaqmの機能は要らないので、BASICに固定して出力を一定にする。
    return ImageFont.truetype(path, pixel_height, layout_engine=ImageFont.Layout.BASIC)


def render_bits(font_path: str, char: str, width: int, height: int) -> list[list[int]]:
    """charをwidth x heightの1bppビットマップ(行のリスト、各行はビットのリスト)に焼く。

    モード"1"(2値)で直接描画する。"L"(グレースケール)でアンチエイリアス
    描画してから閾値で二値化すると、CGROMのような極小サイズではストロークが
    途切れたり太さがぶれたりして読みにくくなる。"1"モードだとFreeTypeが
    2値専用のヒンティングでラスタライズするため、ストロークが繋がりやすい。
    """
    font = _load_font(font_path, height)
    img = Image.new("1", (width, height), 0)
    draw = ImageDraw.Draw(img)
    # anchor="mm": グリフ個別のインク境界でなく、フォントメトリクスに基づく
    # 中央揃え。文字ごとにベースラインが揺れないようにするため。
    draw.text((width / 2, height / 2), char, font=font, fill=1, anchor="mm")
    rows = []
    for y in range(height):
        rows.append([1 if img.getpixel((x, y)) else 0 for x in range(width)])
    return rows


def pack_char(rows: list[list[int]], width: int) -> bytes:
    """1文字分のビットマップを実機CGROMのバイト列に詰める。

    1ラインwidthビットをMSB(左端)から詰め、バイト境界まで0パディング
    （12x12/12x24の下位4bitが常に'0'になるのはこの規則の帰結、図43/44準拠）。
    """
    pad = (-width) % 8
    out = bytearray()
    for row in rows:
        val = 0
        for bit in row:
            val = (val << 1) | bit
        val <<= pad
        out += val.to_bytes((width + pad) // 8, "big")
    return bytes(out)


def build_cgrom(gothic_path: str, mincho_path: str, verbose: bool = False) -> bytearray:
    buf = bytearray(TOTAL_SIZE)
    blank_cache: dict[tuple[str, int, int], bytes] = {}

    for spec in SPECS:
        font_path = gothic_path if spec.style == "gothic" else mincho_path
        blank_key = (font_path, spec.width, spec.height)
        if blank_key not in blank_cache:
            blank_cache[blank_key] = bytes(spec.bytes_per_char)
        blank = blank_cache[blank_key]

        n_defined = 0
        n_total = 0
        for index, char in iter_chars(spec):
            n_total += 1
            off = spec.offset + index * spec.bytes_per_char
            if char is None:
                buf[off : off + spec.bytes_per_char] = blank
                continue
            rows = render_bits(font_path, char, spec.width, spec.height)
            buf[off : off + spec.bytes_per_char] = pack_char(rows, spec.width)
            n_defined += 1

        if verbose:
            print(
                f"{spec.name}: {n_defined}/{n_total} chars rendered "
                f"(offset 0x{spec.offset:06X}, {spec.bytes_per_char}B/char)",
                file=sys.stderr,
            )

    return buf


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", action="version", version=f"cgrom_gen {VERSION}")
    parser.add_argument(
        "--gothic", help="ゴシック体フォントファイル(TTF/OTF/TTC)。省略時はOS標準フォントを自動検出"
    )
    parser.add_argument(
        "--mincho", help="明朝体フォントファイル(TTF/OTF/TTC)。省略時はOS標準フォントを自動検出"
    )
    parser.add_argument("--output", required=True, help="出力先CGROMファイルパス")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    gothic_path = args.gothic or resolve_font("gothic")
    mincho_path = args.mincho or resolve_font("mincho")
    if args.verbose or not (args.gothic and args.mincho):
        print(f"gothic: {gothic_path}", file=sys.stderr)
        print(f"mincho: {mincho_path}", file=sys.stderr)

    buf = build_cgrom(gothic_path, mincho_path, verbose=args.verbose)
    assert len(buf) == TOTAL_SIZE
    with open(args.output, "wb") as f:
        f.write(buf)
    print(f"wrote {args.output} ({len(buf)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
