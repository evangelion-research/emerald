"""Semantic tokens -- the cheapest real feature in the ladder (DESIGN.md 6.2).

Highlighting needs no type information, so it stays per-file and purely
lexical: it keeps working when the import graph is broken, when the file does
not parse, and when `emeraldc` is not even installed. That is the whole reason
this server carries its own lexer.

Classification uses only the token stream and one bit of file-level context --
which names are module bindings -- so `strings` in `strings.split(s)` is a
namespace rather than a variable.
"""

from __future__ import annotations

from lsprotocol import types

from .language import BUILTINS, CONSTANTS, TYPE_ATOMS
from .lexer import Token, significant

TOKEN_TYPES = [
    "namespace",
    "type",
    "parameter",
    "variable",
    "property",
    "function",
    "keyword",
    "comment",
    "string",
    "number",
    "operator",
]
TOKEN_MODIFIERS = ["declaration", "definition", "readonly", "defaultLibrary"]

LEGEND = types.SemanticTokensLegend(
    token_types=TOKEN_TYPES, token_modifiers=TOKEN_MODIFIERS
)

_TYPE_INDEX = {name: i for i, name in enumerate(TOKEN_TYPES)}
_MOD_BIT = {name: 1 << i for i, name in enumerate(TOKEN_MODIFIERS)}

_SIMPLE = {
    "str": "string",
    "fstr": "string",
    "int": "number",
    "float": "number",
    "op": "operator",
    "keyword": "keyword",
}


def encode(
    tokens: list[Token], lines: list[str], modules: frozenset[str] = frozenset()
) -> list[int]:
    """The flat delta-encoded array LSP wants, five ints per token."""
    data: list[int] = []
    prev_line = prev_col = 0
    for line, col, length, ttype, mods in classify(tokens, lines, modules):
        delta_line = line - prev_line
        delta_col = col - prev_col if delta_line == 0 else col
        data += [delta_line, delta_col, length, ttype, mods]
        prev_line, prev_col = line, col
    return data


def classify(
    tokens: list[Token], lines: list[str], modules: frozenset[str] = frozenset()
) -> list[tuple[int, int, int, int, int]]:
    """(line, col, length, type index, modifier bits) per highlighted token."""
    out: list[tuple[int, int, int, int, int]] = []
    sig = significant(tokens)
    i = -1  # index of `token` within `sig`, tracked as we go

    for token in tokens:
        if token.kind == "comment":
            out.extend(_spans(token, lines, _TYPE_INDEX["comment"], 0))
            continue
        i += 1
        if token.kind == "error":
            continue
        kind, mods = _kind_of(token, sig, i, modules)
        if kind is None:
            continue
        out.extend(
            _spans(token, lines, _TYPE_INDEX[kind], sum(_MOD_BIT[m] for m in mods))
        )
    return out


def _prev(sig: list[Token], i: int) -> Token | None:
    return sig[i - 1] if 0 < i <= len(sig) else None


def _next(sig: list[Token], i: int) -> Token | None:
    return sig[i + 1] if 0 <= i + 1 < len(sig) else None


def _kind_of(
    token: Token, sig: list[Token], i: int, modules: frozenset[str]
) -> tuple[str | None, tuple[str, ...]]:
    simple = _SIMPLE.get(token.kind)
    if simple is not None:
        return simple, ()
    if token.kind != "ident":
        return None, ()

    prev, nxt = _prev(sig, i), _next(sig, i)
    prev_kw = prev.value if prev is not None and prev.kind == "keyword" else None

    if prev_kw == "def":
        return "function", ("declaration", "definition")
    if prev_kw in ("type", "error", "dim"):
        return "type", ("declaration", "definition")
    if prev_kw in ("import", "from"):
        return "namespace", ()
    if prev is not None and prev.kind == "op" and prev.value == ".":
        # `m.f` on a module binding is still that module's function
        before = _prev(sig, i - 1)
        if before is not None and before.value in modules:
            return "function" if _is_call(nxt) else "property", ()
        return "property", ()
    if prev_kw == "const":
        return "variable", ("declaration", "readonly")
    if token.value in modules:
        return "namespace", ()
    if token.value in TYPE_ATOMS:
        return "type", ()
    if token.value in CONSTANTS:
        return "variable", ("readonly", "defaultLibrary")
    if token.value in BUILTINS:
        return "function", ("defaultLibrary",)
    if _is_call(nxt):
        return "function", ()
    return "variable", ()


def _is_call(nxt: Token | None) -> bool:
    return nxt is not None and nxt.kind == "op" and nxt.value == "("


def _spans(
    token: Token, lines: list[str], type_index: int, mods: int
) -> list[tuple[int, int, int, int, int]]:
    """Split a token at line boundaries: LSP has no multi-line semantic token,
    and Emerald's string literals may contain a raw newline."""
    if token.end_line == token.line:
        return [(token.line, token.col, token.end_col - token.col, type_index, mods)]
    spans = []
    for line in range(token.line, token.end_line + 1):
        text = lines[line] if line < len(lines) else ""
        start = token.col if line == token.line else 0
        end = token.end_col if line == token.end_line else len(text)
        if end > start:
            spans.append((line, start, end - start, type_index, mods))
    return spans
