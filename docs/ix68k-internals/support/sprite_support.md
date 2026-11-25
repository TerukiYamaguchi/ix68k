# スプライトコントローラ 実装状況

関連: 現行仕様は `docs/ix68k-internals/design/sprite_design.md`、CRTC/ビデオコントローラは
`docs/ix68k-internals/support/crtc_support.md`、IOCS は
`docs/ix68k-internals/support/iocs_support.md` を参照。

> **必須**: スプライトコントローラのレジスタ・機能を実装・変更したら、この
> ファイルの該当表を必ず更新します。

---

X68000の画面機構はCRTC・ビデオコントローラ・スプライトコントローラの3チップに
分かれている（『Inside X68000』3-1節、図13/図14）うちの1ちます。
「スタブ」ではなく、合成描画に必要な範囲は本格実装済み。

実装済み（`docs/ix68k-internals/design/sprite_design.md`）。`src/bus/bus_internal.h`の
`sp_ram`（64KB、`$EB0000-$EBFFFF`）にバッキングストレージを持ち、`src/peripheral/sprite.c`の
`x68k_bus_get_sprite_scene()`でモード派生値を算出、`src/render/sprite_renderer.c`の`x68k_render_sp()`
で合成描画する（2026-07-29のドキュメント分割を機に`crtc.c`/`renderer.c`から
ソースファイルも分離）。

| 領域 | アドレス | 実装状況 |
|---|---|---|
| スプライトスクロールレジスタ | $EB0000-$EB03FF | ✅ 128組×4ワード（XPOS/YPOS/VR・HR・COLOR・SPAT#/PRW）。座標系は図11通り(16,16)オフセット |
| BGスクロール/コントロール | $EB0800-$EB0808 | ✅ BG0/BG1スクロール位置＋BGコントロール（DISP/CPU・BG0/1 ON・BG0/1 TXSEL、図18で実機確認済み） |
| 画面モードレジスタ「解像度設定」 | $EB0810 | ✅ ビット配置はCRTC R20下位バイトと同一（bit0=H-RES, bit1=R20 H-RESフィールド上位ビット, bit2=V-RES, bit3=R20 V-RESフィールド上位ビット, bit4=L/H）。bit1/bit3が立つ（768ドット系/1024ライン系）組合せのみSPレイヤ無効化、それ以外はH-RESのみでBGパタンサイズ・実画面サイズ・BG1利用可否を判定（512×256/256×512混在モードも正規サポート） |
| 画面モードレジスタ（H-TOTAL/H-DISP/V-DISP） | $EB080A-$EB080E | 🔄 読み書きは保持するが解釈しない（$EB0810のH-RES/V-RESのみ使用） |
| PCGエリア（8×8共用/16×16用＋16×16専用） | $EB8000-$EBBFFF | ✅ 8×8セル単位でデコード、16×16はセル4つ(TL/BL/TR/BR)を組み合わせ（図8） |
| BGデータエリア0/1 | $EBC000-$EBFFFF | ✅ 64×64エントリ、球面スクロールでBG0/BG1合成（図9） |

**実装済み機能**: PCGデコード（8×8/16×16、透過は下位4bit=0）、BG0/BG1合成（球面スクロール、
BG0は常にBG1より手前）、スプライト合成（常に16×16、反転HR/VR、低番号ほど手前）、優先順位統合
（PRWでSP/BG0/BG1の相対順序を決め、ビデオコントローラR1のSP優先順位で他画面との順序を決める
2段階構造）、同一水平線32個制限（33個目以降非表示）。

**未実装（既知の限界）**: 実機のタイミング精度（垂直帰線期間中のみレジスタ更新有効等）。
768×512等のモードは「表示しない」が正しい挙動として実装済み（上表の$EB0810行、判定は
$EB0810のみでCRTC側のレジスタは参照しない）。$EB080A-$EB080Eによるスプライト画面と
CRTC表示窓のアライメント補正（XEiJ方式のオフセット計算）は未実装です。
256×256「正方形」
モード自体はCRTC側の表示サイズ判定・PAR補正まで対応済み
（`docs/ix68k-internals/support/crtc_support.md`のシステムポート節参照）。

## 変更履歴

| 日付 | 内容 |
|------|------|
| 2026-09-10 | OTOKO.Xでスプライトが表示されないリグレッションを修正。原因は2つ: (1) 2026-08-19に追加したCRTC R20ゲートが、CRTC直叩きの正方形ドットモード（otoko.c等）ではR20と$EB0810が食い違うことを考慮しておらず、対象にしていたはずのOTOKO.X自身を誤って弾いてきました。(2) $EB0810のビット配置解釈自体が誤りで、正しくはCRTC R20下位バイトと同一配置（bit0=H-RES/bit2=V-RES/bit4=L/H）でした。一次資料5-3本文・図56・IPLROMモードテーブル実測（$FF65E0）・参照実装XEiJの4点で裏付け。この結果、CRTMOD mode0等の標準512×512モードのスプライトも2026-08-19以前から誤判定されていた可能性が高い。CRTC R20ゲートを撤去し、$EB0810を正しいビット配置（bit1/bit3が立つ組合せのみ非対応）で判定する方式へ変更。512×256/256×512混在モードも正規サポートに変更（旧実装は非対応としていたが根拠なし）。設計文書は`docs/ix68k-internals/design/sprite_design.md`。`tests/sprite_render_test.c`ケースH/Jを新方針に合わせ書き換え、ケースK（OTOKO.X回帰）・L（IPLROMモードテーブル網羅）を追加。`tests/video_r2_effects_test.c`の$EB0810生値依存箇所も新配置に合わせ修正 |
| 2026-08-19 | 表示画面が512×512／256×256以外のときSP/BG面を表示しないゲートを`x68k_bus_get_sprite_scene()`に追加（『Inside X68000』2-3-1 p.173／2-4-1 p.178）。従来は$EB0810とR2 SON・$EB0808 DISPだけで判定していたため、MMDSP終了後に768×512のHuman68kコンソールへ戻るとスプライト/BGの残骸が表示され続けていた（MMDSPは通常起動パスでR2/$EB0808を復元しない＝実機では768×512で表示されないため問題にならない挙動）。`tests/sprite_render_test.c`にケースJを追加 |
| 2026-07-29 | ペリフェラル単位のドキュメント分割に伴い`docs/ix68k-internals/support/crtc_support.md`から本ファイルへ分離。`src/peripheral/crtc.c`の`x68k_bus_get_sprite_scene()`を`src/peripheral/sprite.c`へ、`src/render/`配下のPCG/BG/スプライト合成一式を`src/render/sprite_renderer.c`へソース分割（CMakeLists.txt更新、全17テスト回帰なしを確認） |
| 2026-07-19 | スプライトコントローラを実装（6フェーズ）。`sp_ram`バッキングストレージ・PCGデコード・BG0/BG1合成・スプライト合成・優先順位統合(`x68k_bus_get_screen_order()`)・同一水平線32個制限。画面モード判定は当初CRTC表示サイズ依存だったが、ユーザー指摘によりスプライトコントローラ自身の`$EB0810`(H-RES/V-RES)直接読み方式に訂正 |
