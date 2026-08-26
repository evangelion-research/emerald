"""Position and path conversions -- the two traps from DESIGN.md 5.

Two coordinate systems meet here and nowhere else in this package:

* `emeraldc` speaks **1-based lines** and **1-based byte columns**
  (`Token.col`, `Diag.column`), and **canonical filesystem paths**.
* LSP speaks **0-based lines** and, unless the client negotiated otherwise,
  **UTF-16 code units**, over `file://` URIs.

Internally the server works in 0-based lines and 0-based UTF-32 character
columns -- pygls' own convention -- so the UTF-16 step is applied once, by
`emerald_lsp.server`, through the document's own position codec. Everything
here is the byte-to-char and path/URI half.
"""

from __future__ import annotations

import os
import pathlib
import urllib.parse

from lsprotocol import types


def uri_to_path(uri: str) -> str | None:
    """`file://` URI -> canonical filesystem path, or None for other schemes."""
    parsed = urllib.parse.urlparse(uri)
    if parsed.scheme != "file":
        return None
    path = urllib.parse.unquote(parsed.path)
    if os.name == "nt" and path.startswith("/") and len(path) > 2 and path[2] == ":":
        path = path[1:]
    return canonical(path)


def path_to_uri(path: str) -> str:
    """Filesystem path -> `file://` URI, canonicalized to match the compiler."""
    return pathlib.Path(canonical(path)).as_uri()


def canonical(path: str) -> str:
    """The compiler's notion of module identity (`module.c:164`).

    Cache keys must agree with it or a symlinked workspace produces two
    analyses of one file.
    """
    try:
        return os.path.realpath(path)
    except OSError:  # pragma: no cover -- realpath is effectively total
        return os.path.abspath(path)


def byte_col_to_char_col(line_text: str, byte_col: int) -> int:
    """1-based byte column (compiler) -> 0-based character column.

    A single non-ASCII character earlier in the line shifts every column after
    it, so this is not the identity even though it is on ASCII input.
    """
    byte_offset = max(byte_col - 1, 0)
    encoded = line_text.encode("utf-8")
    if byte_offset >= len(encoded):
        return len(line_text)
    return len(encoded[:byte_offset].decode("utf-8", errors="ignore"))


def line_text(lines: list[str], line: int) -> str:
    return lines[line] if 0 <= line < len(lines) else ""


def range_of(
    start_line: int, start_char: int, end_line: int, end_char: int
) -> types.Range:
    return types.Range(
        start=types.Position(line=start_line, character=start_char),
        end=types.Position(line=end_line, character=end_char),
    )


def span(first, last) -> types.Range:
    """From one `emerald_lsp.lexer.Token`'s start to another's end."""
    return range_of(first.line, first.col, last.end_line, last.end_col)


def token_range(token) -> types.Range:
    """The range covered by a single token."""
    return span(token, token)


def contains(rng: types.Range, pos: types.Position) -> bool:
    start = (rng.start.line, rng.start.character)
    end = (rng.end.line, rng.end.character)
    return start <= (pos.line, pos.character) <= end
