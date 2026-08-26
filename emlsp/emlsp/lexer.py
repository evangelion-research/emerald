"""A pure-Python lexer for Emerald, mirroring ``../emerald/src/lexer.c``.

The compiler is the authority on Emerald's syntax; this exists because two
LSP features are *purely lexical* and must keep working when the import graph
is broken or the buffer does not parse: semantic tokens and the outline
(`docs/grammar.md`, DESIGN.md 6.2). Everything that needs types goes to
`emeraldc` instead -- see `emerald_lsp.compiler`.

Positions here are 0-based lines and 0-based *character* (UTF-32) columns,
which is what pygls wants internally; the UTF-16 conversion for the wire
happens once, at the boundary, in `emerald_lsp.positions`.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Iterator

# lexer.c:14 `keywords[]` -- kept in the same order for diffing against it.
KEYWORDS = frozenset(
    """
    def if elif else while for in return and or not True False None
    break continue pass type const match pure partial import from as dim
    error try catch
    """.split()
)

# Longest match first; from lexer.c's punctuation switch and TokKind.
OPERATORS = (
    "//=", "**=",
    "==", "!=", "<=", ">=", "->", "=>", "|>", ">>", "<<", "//", "**",
    "+=", "-=", "*=", "/=",
    "{", "}", "(", ")", "[", "]", ",", ".", ":", ";", "=",
    "|", "&", "^", "+", "-", "*", "/", "%", "<", ">", "?",
)


@dataclass(slots=True, frozen=True)
class Token:
    """One token, with a half-open [start, end) span in the document."""

    kind: str  # ident|keyword|int|float|str|fstr|comment|op|error
    value: str
    line: int  # 0-based
    col: int  # 0-based, characters
    end_line: int
    end_col: int
    offset: int  # 0-based character offset into the source


def tokenize(src: str) -> list[Token]:
    """Tokenize `src`, never raising and never stopping early.

    Unlike the C lexer, an unterminated string or an unknown character yields
    an `error` token and scanning continues: an editor buffer is broken most of
    the time you are typing, so ending the stream there would blank out
    highlighting for the rest of the file.
    """
    return list(_scan(src))


def _scan(src: str) -> Iterator[Token]:
    i, line, col = 0, 0, 0
    n = len(src)

    def tok(kind: str, start: int, sl: int, sc: int) -> Token:
        return Token(kind, src[start:i], sl, sc, line, col, start)

    while i < n:
        c = src[i]

        if c == "\n":
            i, line, col = i + 1, line + 1, 0
            continue
        if c in " \t\r":
            i, col = i + 1, col + 1
            continue

        start, sl, sc = i, line, col

        if c == "#":  # lexer.c:41 -- comment runs to end of line
            while i < n and src[i] != "\n":
                i, col = i + 1, col + 1
            yield tok("comment", start, sl, sc)
            continue

        # `"..."`, `'...'`, and the f-string prefix (lexer.c:58, TK_FSTR).
        # Before the identifier scan, so `f"x"` is not read as the name `f`.
        quote = c if c in "\"'" else (src[i + 1] if c == "f" and i + 1 < n and src[i + 1] in "\"'" else "")
        if quote:
            prefix = 1 if c == quote else 2
            i, col = i + prefix, col + prefix
            i, line, col, closed = _string_body(src, i, line, col, quote)
            if not closed:
                i, line, col = _stop_at_newline(src, start, sl, sc)
            yield tok(("fstr" if prefix == 2 else "str") if closed else "error", start, sl, sc)
            continue

        if c.isalpha() or c == "_":
            while i < n and (src[i].isalnum() or src[i] == "_"):
                i, col = i + 1, col + 1
            word = src[start:i]
            yield tok("keyword" if word in KEYWORDS else "ident", start, sl, sc)
            continue

        if c.isdigit():
            i, col, kind = _number(src, i, col)
            yield tok(kind, start, sl, sc)
            continue

        for op in OPERATORS:
            if src.startswith(op, i):
                i, col = i + len(op), col + len(op)
                yield tok("op", start, sl, sc)
                break
        else:  # lexer.c:128 -- unknown character; we recover instead of stopping
            i, col = i + 1, col + 1
            yield tok("error", start, sl, sc)


def _stop_at_newline(src: str, start: int, line: int, col: int) -> tuple[int, int, int]:
    """Clip an unterminated string to its own line.

    A legal Emerald string may contain a raw newline, so scanning only stops at
    the closing quote -- but when there is no closing quote, running to EOF
    would blank out the rest of the file the moment a quote is typed. So an
    *unterminated* string is reported as a one-line error token and scanning
    resumes on the next line. Closed strings, multi-line ones included, are
    unaffected: this branch is only reached when the quote never matched.
    """
    newline = src.find("\n", start)
    end = len(src) if newline == -1 else newline
    return end, line, col + (end - start)


def _string_body(
    src: str, i: int, line: int, col: int, quote: str
) -> tuple[int, int, int, bool]:
    """Consume a string body after its opening quote. Returns the new cursor
    and whether the closing quote was found."""
    n = len(src)
    while i < n and src[i] != quote:
        if src[i] == "\\" and i + 1 < n:
            i, col = i + 2, col + 2
            continue
        if src[i] == "\n":
            i, line, col = i + 1, line + 1, 0
        else:
            i, col = i + 1, col + 1
    if i < n and src[i] == quote:
        return i + 1, line, col + 1, True
    return i, line, col, False


def _number(src: str, i: int, col: int) -> tuple[int, int, str]:
    """Scan an int or float literal (lexer.c:90)."""
    n = len(src)
    kind = "int"
    while i < n and src[i].isdigit():
        i, col = i + 1, col + 1
    # `1.5`, and also `1.` when not followed by an identifier (`1.foo` is a
    # field access on an int literal, not a float).
    if i < n and src[i] == "." and not (
        i + 1 < n and (src[i + 1].isalpha() or src[i + 1] == "_")
    ):
        kind = "float"
        i, col = i + 1, col + 1
        while i < n and src[i].isdigit():
            i, col = i + 1, col + 1
    if i < n and src[i] in "eE":
        save_i, save_col = i, col
        i, col = i + 1, col + 1
        if i < n and src[i] in "+-":
            i, col = i + 1, col + 1
        if i < n and src[i].isdigit():
            kind = "float"
            while i < n and src[i].isdigit():
                i, col = i + 1, col + 1
        else:
            i, col = save_i, save_col
    return i, col, kind


def significant(tokens: list[Token]) -> list[Token]:
    """Tokens with comments dropped -- the stream the C parser would see."""
    return [t for t in tokens if t.kind != "comment"]


def token_at(tokens: list[Token], line: int, col: int) -> Token | None:
    """The token containing a cursor position, or the one ending exactly at it.

    Ending-at counts so that completion and hover work with the cursor parked
    at the right edge of a word, which is where it always is while typing --
    but a token that genuinely contains the position wins, so that the cursor
    on the `u` of `m.upper` is on `upper` and not on the dot before it.
    """
    touching: Token | None = None
    for t in tokens:
        if (t.line, t.col) > (line, col):
            break
        if (t.line, t.col) <= (line, col) < (t.end_line, t.end_col):
            return t
        if (t.end_line, t.end_col) == (line, col):
            touching = t
    return touching
