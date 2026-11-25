# 技術記事の作成

「技術記事を作って」と言われたらこの手順を使う。

## 手順

1. `local/articles/md/<name>.md` に Markdown で記事を書く（先頭に `<!-- 作成日: YYYY-MM-DD -->` を入れる）。
   本文の文章リズムは `/cognitive-rhythm-writing` の規範に従う。
2. `md2pdf.py` で PDF に変換する:
   ```bash
   cd local/articles/md
   uv run --with reportlab python3 md2pdf.py <name>.md
   # → local/articles/<name>.pdf が生成される
   ```
3. `make_<name>.py` は書かない（md2pdf.py が汎用コンバーター）

## Markdown 規約

```markdown
<!-- 作成日: YYYY-MM-DD -->
# タイトル
── サブタイトル ──

リード文（全幅エリア）

# セクション        ← ここから2段組
## サブセクション
```コードブロック```
| Col | Col |
|-----|-----|
| ... | ... |
> **補足** ノートボックス
- 箇条書き
```
