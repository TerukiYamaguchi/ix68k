"""docs/x68k-usage の操作資料から、質問に関連する断片を検索する。

資料を見出しごとの断片に分け、BM25 で質問との関連が高い順に選ぶ。
日本語は 2 文字ずつ(bigram)、英数字は語単位で照合する。標準ライブラリのみ。
"""

import math
import re
from collections import Counter
from dataclasses import dataclass
from pathlib import Path

DOC_SUBDIRS = ("guide", "official", "freeware", "development")
HEADING_PATTERN = re.compile(r"^(#{1,3})\s+(.*)$")
FRONTMATTER_DELIMITER = "---"
ASCII_TOKEN_PATTERN = re.compile(r"[A-Za-z0-9_$]+(?:\.[A-Za-z0-9_$]+)*")
CJK_RUN_PATTERN = re.compile(r"[^\x00-\x7f\s、。，．・「」『』（）()：:;,.!?！？\-—=|>*`#/\\]+")
MIN_CHUNK_CHARS = 30
MAX_CHUNK_CHARS = 1500
BM25_K1 = 1.5
BM25_B = 0.75
ASCII_QUERY_WEIGHT = 2.0
EXE_MATCH_BONUS = 4.0
MAX_CHUNKS_PER_FILE = 3
UNKNOWN_STATUS = "不明"
STATUS_DISPLAY_CHARS = 30


@dataclass(frozen=True)
class Chunk:
    source: str  # docs ルートからの相対パス
    status: str  # frontmatter の「検証状態」
    title: str
    text: str

    def header(self) -> str:
        status = self.status[:STATUS_DISPLAY_CHARS]
        return f"[{self.source} / 検証状態: {status}] {self.title}"

    def search_text(self) -> str:
        """照合対象。検証状態の文面はノイズになるので含めない。"""
        return f"{self.source} {self.title}\n{self.text}"


def parse_frontmatter(text: str) -> tuple[dict[str, str], str]:
    lines = text.splitlines()
    if not lines or lines[0].strip() != FRONTMATTER_DELIMITER:
        return {}, text
    meta: dict[str, str] = {}
    for index, line in enumerate(lines[1:], start=1):
        if line.strip() == FRONTMATTER_DELIMITER:
            return meta, "\n".join(lines[index + 1 :])
        key, separator, value = line.partition(":")
        if separator:
            meta[key.strip()] = value.strip()
    return {}, text


def split_long(text: str) -> list[str]:
    """MAX_CHUNK_CHARS を超える断片を行単位で分割する。"""
    parts, current = [], ""
    for line in text.splitlines(keepends=True):
        if current and len(current) + len(line) > MAX_CHUNK_CHARS:
            parts.append(current)
            current = ""
        current += line
    if current:
        parts.append(current)
    return parts


def chunk_document(source: str, text: str) -> tuple[list[Chunk], str]:
    """1 ファイルを断片に分ける。戻り値は (断片, exe 名の語幹)。"""
    meta, body = parse_frontmatter(text)
    status = meta.get("検証状態", UNKNOWN_STATUS)
    exe_stem = meta.get("exe", "").split(".")[0].lower()
    chunks: list[Chunk] = []
    title, buffer = Path(source).stem, []

    def flush() -> None:
        joined = "\n".join(buffer).strip()
        if len(joined) >= MIN_CHUNK_CHARS:
            for part in split_long(joined):
                chunks.append(Chunk(source, status, title, part.strip()))

    for line in body.splitlines():
        match = HEADING_PATTERN.match(line)
        if match:
            flush()
            title, buffer = match.group(2).strip(), [line]
        else:
            buffer.append(line)
    flush()
    return chunks, exe_stem


def tokenize(text: str) -> tuple[list[str], list[str]]:
    """(英数字の語, 日本語 bigram) を返す。語は小文字化する。"""
    ascii_tokens = [t.lower() for t in ASCII_TOKEN_PATTERN.findall(text) if len(t) >= 2]
    cjk_tokens: list[str] = []
    for run in CJK_RUN_PATTERN.findall(re.sub(ASCII_TOKEN_PATTERN, " ", text)):
        if len(run) == 1:
            cjk_tokens.append(run)
        cjk_tokens.extend(run[i : i + 2] for i in range(len(run) - 1))
    return ascii_tokens, cjk_tokens


class DocsIndex:
    def __init__(self, docs_root: Path):
        self.chunks: list[Chunk] = []
        self.exe_sources: dict[str, str] = {}
        for subdir in DOC_SUBDIRS:
            for path in sorted((docs_root / subdir).rglob("*.md")):
                source = path.relative_to(docs_root).as_posix()
                chunks, exe_stem = chunk_document(source, path.read_text(encoding="utf-8"))
                self.chunks.extend(chunks)
                if exe_stem and chunks:
                    self.exe_sources[exe_stem] = source
        self.term_counts = [Counter(sum(tokenize(c.search_text()), [])) for c in self.chunks]
        self.lengths = [sum(counts.values()) for counts in self.term_counts]
        self.average_length = (sum(self.lengths) / len(self.lengths)) if self.lengths else 0.0
        document_frequency: Counter = Counter()
        for counts in self.term_counts:
            document_frequency.update(counts.keys())
        total = len(self.chunks)
        self.idf = {
            term: math.log(1 + (total - df + 0.5) / (df + 0.5))
            for term, df in document_frequency.items()
        }

    def score(self, query: str) -> list[float]:
        ascii_tokens, cjk_tokens = tokenize(query)
        weights: Counter = Counter()
        for token in ascii_tokens:
            weights[token] += ASCII_QUERY_WEIGHT
        for token in cjk_tokens:
            weights[token] += 1.0
        matched_sources = {self.exe_sources[t] for t in ascii_tokens if t in self.exe_sources}
        scores = []
        for chunk, counts, length in zip(self.chunks, self.term_counts, self.lengths):
            total = EXE_MATCH_BONUS if chunk.source in matched_sources else 0.0
            norm = BM25_K1 * (1 - BM25_B + BM25_B * length / self.average_length)
            for term, weight in weights.items():
                frequency = counts.get(term, 0)
                if frequency:
                    total += weight * self.idf[term] * frequency * (BM25_K1 + 1) / (frequency + norm)
            scores.append(total)
        return scores

    def search(self, query: str, budget_chars: int) -> list[Chunk]:
        """関連度の高い断片を、文字数の予算に収まる範囲で返す。"""
        scores = self.score(query)
        ranked = sorted(range(len(self.chunks)), key=lambda i: scores[i], reverse=True)
        picked: list[Chunk] = []
        per_file: Counter = Counter()
        used = 0
        for index in ranked:
            if scores[index] <= 0:
                break
            chunk = self.chunks[index]
            if per_file[chunk.source] >= MAX_CHUNKS_PER_FILE:
                continue
            size = len(chunk.header()) + len(chunk.text)
            if used + size > budget_chars:
                continue
            picked.append(chunk)
            per_file[chunk.source] += 1
            used += size
        return picked


def format_reference(chunks: list[Chunk]) -> str:
    return "\n\n".join(f"{chunk.header()}\n{chunk.text}" for chunk in chunks)
