#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""AsterASM source formatter.

Dependency-free formatter for .aa source files. It is intentionally lexical: it
preserves strings/comments and only normalizes layout and token spacing.
"""
from __future__ import annotations

import argparse
import difflib
import pathlib
import sys
from dataclasses import dataclass
from typing import Iterable

MULTI = ("##", "->", "=>", "==", "!=", "<=", ">=", "<<", ">>", "&&", "||", "..")
OPERATORS = {"=", "+", "-", "*", "/", "%", "&", "|", "^", "!", "~", "<", ">", "==", "!=", "<=", ">=", "<<", ">>", "&&", "||", "->", "=>"}
BINARY = {"=", "+", "-", "*", "/", "%", "&", "|", "^", "<", ">", "==", "!=", "<=", ">=", "<<", ">>", "&&", "||", "->", "=>"}
UNARY_PREFIX = {"!", "~", "-", "+", "&", "*"}
NO_SPACE_BEFORE = {"(", "[", ".", ",", ";", ")", "]"}
NO_SPACE_AFTER = {"(", "[", ".", "@", "$"}
TYPE_CONSTRUCTORS = {"ptr", "cap", "bits", "vec", "mask", "svec", "pred", "trunc", "zext", "sext", "bitcast", "ptrcast"}
TYPE_WORDS = {
    "void", "bool", "i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "i128", "u128",
    "isize", "usize", "f16", "bf16", "f32", "f64", "const", "volatile", "type",
}
DECL_START = {"export", "type", "const", "global", "extern", "fn", "dispatch", "section", "macro", "target", "feature"}

@dataclass(frozen=True)
class Token:
    kind: str
    text: str
    had_space: bool = False
    had_newline: bool = False


def _scan_quoted(src: str, i: int, quote: str) -> int:
    i += 1
    while i < len(src):
        if src[i] == "\\":
            i += 2
            continue
        if src[i] == quote:
            return i + 1
        i += 1
    return i


def tokenize(src: str) -> list[Token]:
    out: list[Token] = []
    i = 0
    saw_space = False
    saw_newline = False
    n = len(src)
    while i < n:
        c = src[i]
        if c.isspace():
            saw_space = True
            if c in "\r\n":
                saw_newline = True
            i += 1
            continue
        start_space, start_newline = saw_space, saw_newline
        saw_space = saw_newline = False

        if src.startswith("//", i):
            j = src.find("\n", i)
            if j < 0:
                j = n
            out.append(Token("line_comment", src[i:j].rstrip(), start_space, start_newline))
            i = j
            continue
        if src.startswith("/*", i):
            j = i + 2
            depth = 1
            while j < n and depth:
                if src.startswith("/*", j):
                    depth += 1
                    j += 2
                elif src.startswith("*/", j):
                    depth -= 1
                    j += 2
                else:
                    j += 1
            out.append(Token("block_comment", src[i:j], start_space, start_newline))
            i = j
            continue

        # String/byte-string/C-string prefixes. Identifiers immediately before a quote
        # are not valid without punctuation, so treating b"..."/c"..." as one token
        # is safe for the 1.0 lexical contract.
        if c in ('"', "'"):
            j = _scan_quoted(src, i, c)
            out.append(Token("quoted", src[i:j], start_space, start_newline))
            i = j
            continue
        if c in "bc" and i + 1 < n and src[i + 1] == '"':
            j = _scan_quoted(src, i + 1, '"')
            out.append(Token("quoted", src[i:j], start_space, start_newline))
            i = j
            continue

        if c == "_" or c.isidentifier():
            j = i + 1
            while j < n and ("_" + src[j]).isidentifier():
                j += 1
            out.append(Token("word", src[i:j], start_space, start_newline))
            i = j
            continue
        if c.isdigit():
            j = i + 1
            if c == "0" and j < n and src[j] in "xXbBoO":
                j += 1
                while j < n and (src[j].isalnum() or src[j] == "_"):
                    j += 1
            else:
                while j < n and (src[j].isdigit() or src[j] == "_"):
                    j += 1
                if j < n and src[j] == "." and not src.startswith("..", j):
                    j += 1
                    while j < n and (src[j].isdigit() or src[j] == "_"):
                        j += 1
                if j < n and src[j] in "eE":
                    j += 1
                    if j < n and src[j] in "+-": j += 1
                    while j < n and (src[j].isdigit() or src[j] == "_"):
                        j += 1
                # Retain malformed suffixes as one token for diagnostic stability.
                while j < n and (src[j].isalnum() or src[j] == "_"):
                    j += 1
            out.append(Token("number", src[i:j], start_space, start_newline))
            i = j
            continue

        matched = None
        for op in MULTI:
            if src.startswith(op, i):
                matched = op
                break
        if matched:
            out.append(Token("punct", matched, start_space, start_newline))
            i += len(matched)
        else:
            out.append(Token("punct", c, start_space, start_newline))
            i += 1
    return out


def _looks_generic(tokens: list[Token], i: int) -> bool:
    if tokens[i].text != "<" or i == 0:
        return False
    prev = tokens[i - 1]
    if prev.text in TYPE_CONSTRUCTORS or prev.text in TYPE_WORDS:
        return True
    if prev.kind != "word" and prev.text not in (")", "]"):
        return False
    depth = 0
    saw_type_signal = False
    limit = min(len(tokens), i + 48)
    for j in range(i, limit):
        t = tokens[j].text
        if t == "<":
            depth += 1
        elif t == ">":
            depth -= 1
            if depth == 0:
                follow = tokens[j + 1].text if j + 1 < len(tokens) else ""
                if follow in {"(", ")", "[", "]", "{", ";", ",", "=", "@", "->", "."}:
                    return saw_type_signal or (j == i + 2 and tokens[i + 1].kind == "word")
                return False
        elif depth == 1:
            if t == ":" or t in TYPE_WORDS or t in TYPE_CONSTRUCTORS or t == ",":
                saw_type_signal = True
            if t in {";", "&&", "||", "==", "!="}:
                return False
    return False


class Writer:
    def __init__(self, indent_width: int):
        self.indent_width = indent_width
        self.indent = 0
        self.lines: list[str] = []
        self.line = ""

    def _prefix(self) -> str:
        return " " * (self.indent * self.indent_width)

    def ensure_text(self, text: str, space_before: bool = False) -> None:
        if not self.line:
            self.line = self._prefix()
        if space_before and self.line and not self.line.endswith((" ", "\t")) and self.line.strip():
            self.line += " "
        self.line += text

    def space(self) -> None:
        if self.line and self.line.strip() and not self.line.endswith(" "):
            self.line += " "

    def trim_space(self) -> None:
        self.line = self.line.rstrip(" ")

    def newline(self, blank: bool = False) -> None:
        text = self.line.rstrip()
        if text.strip():
            self.lines.append(text)
        elif self.lines and (not self.lines[-1]):
            pass
        self.line = ""
        if blank and self.lines and self.lines[-1] != "":
            self.lines.append("")

    def blankline(self) -> None:
        self.newline()
        if self.lines and self.lines[-1] != "":
            self.lines.append("")

    def finish(self) -> str:
        self.newline()
        while self.lines and self.lines[-1] == "":
            self.lines.pop()
        return "\n".join(self.lines) + "\n"


def format_source(src: str, indent_width: int = 4) -> str:
    tokens = tokenize(src)
    w = Writer(indent_width)
    paren_depth = 0
    bracket_depth = 0
    angle_depth = 0
    prev: Token | None = None
    previous_top_closed = False

    def next_noncomment(idx: int) -> Token | None:
        for k in range(idx + 1, len(tokens)):
            if tokens[k].kind not in {"line_comment", "block_comment"}:
                return tokens[k]
        return None

    def previous_is_generic_close(idx: int) -> bool:
        """Whether tokens[idx-1] is the `>` closing a generic/type argument list."""
        if idx <= 0 or tokens[idx - 1].text != ">":
            return False
        depth = 0
        lower = max(-1, idx - 64)
        for j in range(idx - 1, lower, -1):
            text_j = tokens[j].text
            if text_j == ">":
                depth += 1
            elif text_j == "<":
                depth -= 1
                if depth == 0:
                    return _looks_generic(tokens, j)
            elif depth == 0 and text_j in {";", "{", "}", "="}:
                break
        return False

    for i, tok in enumerate(tokens):
        text = tok.text
        nxt = next_noncomment(i)

        if tok.kind == "line_comment":
            if w.line.strip():
                w.space()
            w.ensure_text(text)
            w.newline()
            prev = tok
            continue
        if tok.kind == "block_comment":
            if w.line.strip():
                w.newline()
            parts = text.splitlines() or [text]
            for part in parts:
                w.ensure_text(part.rstrip())
                w.newline()
            if tok.had_newline:
                # Preserve separation around documentation-style block comments.
                if len(parts) > 1:
                    w.blankline()
            prev = tok
            continue

        # Add a visual separator between top-level declarations/functions.
        if previous_top_closed and w.indent == 0 and text not in {";", ",", "else", "@"}:
            if text in DECL_START or text in {"import", "module"}:
                if text not in {"import", "module"}:
                    w.blankline()
            previous_top_closed = False

        if text == "{":
            # One space before a block/aggregate unless punctuation already owns spacing.
            if w.line.strip() and not w.line.endswith((" ", "(", "[", ".")):
                w.space()
            w.ensure_text("{")
            w.newline()
            w.indent += 1
            prev = tok
            continue
        if text == "}":
            w.newline()
            w.indent = max(0, w.indent - 1)
            w.ensure_text("}")
            if nxt and nxt.text == "else":
                w.space()
            elif nxt and nxt.text in {";", ",", ")", "]", "@"}:
                pass
            else:
                w.newline()
                if w.indent == 0:
                    previous_top_closed = True
            prev = tok
            continue
        if text == ";":
            w.trim_space()
            w.ensure_text(";")
            # Semicolons always terminate AsterASM declarations/statements/native ops;
            # there is no C-style for(;;) syntax in 1.0.
            w.newline()
            if w.indent == 0:
                # module/imports stay compact; other top-level declarations can be separated later.
                previous_top_closed = prev is not None and prev.text not in {"module", "import"}
            prev = tok
            continue
        if text == ",":
            w.trim_space()
            w.ensure_text(",")
            w.space()
            prev = tok
            continue
        if text == ":":
            w.trim_space()
            w.ensure_text(":")
            w.space()
            prev = tok
            continue
        if text == ".":
            if prev and prev.kind == "number":
                w.space()
            else:
                w.trim_space()
            w.ensure_text(".")
            prev = tok
            continue
        if text == "@" or text == "$":
            if prev and prev.text not in {"(", "[", ".", "@", "$"} and (prev.kind in {"word", "number", "quoted"} or prev.text in {")", "]", "}"}):
                w.space()
            w.ensure_text(text)
            prev = tok
            continue
        if text == "(":
            # Calls/declarations bind tightly, while control keywords and a binary
            # operator followed by a grouped expression require a separating space.
            spaced_paren = bool(
                prev
                and (
                    prev.text in {"if", "while", "for", "when", "requires", "return"}
                    or (prev.text in BINARY and not (prev.text == ">" and previous_is_generic_close(i)))
                )
            )
            if spaced_paren:
                w.space()
            else:
                w.trim_space()
            w.ensure_text("(")
            paren_depth += 1
            prev = tok
            continue
        if text == ")":
            w.trim_space()
            w.ensure_text(")")
            paren_depth = max(0, paren_depth - 1)
            prev = tok
            continue
        if text == "[":
            # Memory operations may carry a typed suffix (`store.ptr<u8> [..]`).
            # Look through that suffix instead of treating the closing `>` as the
            # end of the operation name.  Ordinary indexing remains tight (`a[i]`).
            chain_words: list[str] = []
            k = i - 1
            scanned = 0
            while k >= 0 and scanned < 20:
                t = tokens[k]
                if t.text in {";", "{", "}", "=", ",", "("}:
                    break
                if t.kind == "word":
                    chain_words.append(t.text)
                elif t.text not in {".", "<", ">", "*", "&"} and t.kind != "number":
                    break
                k -= 1
                scanned += 1
            spaced_bracket = any(x in {"load", "store", "atomic"} for x in chain_words)
            if spaced_bracket:
                w.space()
            else:
                w.trim_space()
            w.ensure_text("[")
            bracket_depth += 1
            prev = tok
            continue
        if text == "]":
            w.trim_space()
            w.ensure_text("]")
            bracket_depth = max(0, bracket_depth - 1)
            prev = tok
            continue

        if text == "<" and _looks_generic(tokens, i):
            w.trim_space()
            w.ensure_text("<")
            angle_depth += 1
            prev = tok
            continue
        if text == ">" and angle_depth > 0:
            w.trim_space()
            w.ensure_text(">")
            angle_depth -= 1
            prev = tok
            continue

        if text in OPERATORS:
            # `!` and `~` have no binary meaning in AsterASM: they are
            # always prefixes (except `!=`, which is a separate token).
            # Control-flow heads such as `if !ready` and `while !done`
            # therefore must not be formatted as `if ! ready`.
            unary = text in {"!", "~"} or (text in UNARY_PREFIX and (
                prev is None
                or prev.text in {"(", "[", "{", ",", ":", "=", "return", "br", "if", "while", "requires"}
                or prev.text in OPERATORS
            ))
            if unary:
                if prev and prev.text + text in MULTI:
                    w.space()
                if text == "&" and prev and prev.text == "[":
                    pass
                elif prev and prev.kind in {"word", "number", "quoted"}:
                    w.space()
                w.ensure_text(text)
            else:
                w.space()
                w.ensure_text(text)
                w.space()
            prev = tok
            continue

        # Word/number/string and any punctuation not specially handled.
        need_space = False
        if w.line.strip():
            if prev is not None:
                if prev.text in NO_SPACE_AFTER or text in NO_SPACE_BEFORE:
                    need_space = False
                elif prev.text in {"<"} and angle_depth > 0:
                    need_space = False
                elif text == ">" and angle_depth > 0:
                    need_space = False
                elif prev.text in {"!", "~"}:
                    need_space = False
                elif prev.kind in {"word", "number", "quoted"} and tok.kind in {"word", "number", "quoted"}:
                    need_space = True
                elif prev.text in {"return", "let", "const", "global", "type", "module", "import", "export", "fn", "extern", "section", "native", "inputs", "outputs", "bind", "to", "reg", "unsafe", "exact", "bytes", "comptime", "target", "feature", "when", "else", "as", "in"}:
                    need_space = True
        w.ensure_text(text, need_space)
        prev = tok

    result = w.finish()
    # Normalize accidental excessive blank lines introduced by top-level separation.
    while "\n\n\n" in result:
        result = result.replace("\n\n\n", "\n\n")
    return result


def collect_paths(items: Iterable[str]) -> list[pathlib.Path]:
    found: set[pathlib.Path] = set()
    for raw in items:
        p = pathlib.Path(raw)
        if p.is_dir():
            for f in p.rglob("*.aa"):
                if f.is_file():
                    found.add(f.resolve())
        elif p.is_file():
            found.add(p.resolve())
        else:
            raise FileNotFoundError(raw)
    return sorted(found)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Format AsterASM .aa source files")
    ap.add_argument("paths", nargs="*", help="files or directories (directories recurse over *.aa)")
    ap.add_argument("--check", action="store_true", help="do not write; exit 1 if any file would change")
    ap.add_argument("--diff", action="store_true", help="show unified diff for changed files")
    ap.add_argument("--stdout", action="store_true", help="format exactly one file to stdout")
    ap.add_argument("--indent", type=int, default=4, help="indent width (default: 4)")
    args = ap.parse_args(argv)
    if args.indent < 1 or args.indent > 16:
        ap.error("--indent must be between 1 and 16")
    if not args.paths:
        ap.error("at least one path is required")
    try:
        files = collect_paths(args.paths)
    except FileNotFoundError as e:
        print(f"asterfmt: path not found: {e}", file=sys.stderr)
        return 2
    if args.stdout and len(files) != 1:
        ap.error("--stdout requires exactly one file")

    changed = 0
    for path in files:
        try:
            old = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            print(f"asterfmt: not UTF-8: {path}", file=sys.stderr)
            return 2
        new = format_source(old, args.indent)
        if args.stdout:
            sys.stdout.write(new)
            continue
        if new != old:
            changed += 1
            if args.diff:
                sys.stdout.writelines(difflib.unified_diff(
                    old.splitlines(True), new.splitlines(True),
                    fromfile=str(path), tofile=str(path) + " (formatted)"
                ))
            if not args.check:
                path.write_text(new, encoding="utf-8", newline="\n")
    if args.check and changed:
        print(f"asterfmt: {changed} file(s) need formatting", file=sys.stderr)
        return 1
    if not args.stdout:
        action = "checked" if args.check else "formatted"
        print(f"asterfmt: {action} {len(files)} file(s); changed={changed}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
