"""Compiler and local diagnostics -> LSP diagnostics.

Compiler mapping is mechanical (DESIGN.md 5, last bullet). The interesting
parts are the two conversions it has to do on the way, and one gap it has to
paper over:

* 1-based byte columns become 0-based character columns (`positions`).
* `end_line`/`end_col` do not exist yet (DESIGN.md 1b/4d), so a range is
  synthesized by widening the start position over the token that begins there.
  Every range in this module gets narrower and more accurate the day the AST
  carries end positions -- and nothing else has to change.
"""

from __future__ import annotations

from lsprotocol import types

from .lexer import significant, tokenize
from .outline import Definition, Outline
from .positions import byte_col_to_char_col, line_text, path_to_uri, range_of

SOURCE = "emeraldc"

_SEVERITY = {
    "error": types.DiagnosticSeverity.Error,
    "warning": types.DiagnosticSeverity.Warning,
    "note": types.DiagnosticSeverity.Information,
    "hint": types.DiagnosticSeverity.Hint,
}

_UNUSED_SOURCE = "emerald-lsp"
_UNUSED_CODE = "E_UNUSED"


def unused_diagnostics(outline: Outline) -> list[types.Diagnostic]:
    """Find declarations that have no references in this file.

    This is intentionally the small, reliable subset of Go's unused checks:
    imports and local variables/constants/functions are errors. Parameters are
    exempt, as are top-level declarations because another module may use an
    exported name. The compiler remains responsible for type-aware and
    cross-module usage.
    """
    top_level = {id(definition) for definition in outline.symbols}
    parents = _definition_parents(outline)
    candidates = [
        definition
        for definition in outline.definitions
        if _is_unused_candidate(definition, top_level)
    ]
    if not candidates:
        return []

    groups: dict[tuple[int, str], list[Definition]] = {}
    for definition in candidates:
        parent = parents.get(id(definition))
        key = (id(parent) if parent is not None else 0, definition.name)
        groups.setdefault(key, []).append(definition)

    declarations = _declaration_tokens(outline)
    used: set[tuple[int, str]] = set()
    for token in significant(outline.tokens):
        if token.kind != "ident" or id(token) in declarations:
            continue
        definition = outline.resolve(token.value, token.offset)
        if definition is None:
            continue
        parent = parents.get(id(definition))
        key = (id(parent) if parent is not None else 0, definition.name)
        if key in groups:
            used.add(key)

    result: list[types.Diagnostic] = []
    for key, definitions in groups.items():
        if key in used:
            continue
        definition = min(
            definitions,
            key=lambda item: (
                item.selection_range.start.line,
                item.selection_range.start.character,
            ),
        )
        if definition.is_import:
            message = f'imported and not used: "{definition.name}"'
        else:
            message = f"declared and not used: {definition.name}"
        result.append(
            types.Diagnostic(
                range=_binding_range(outline, definition),
                message=message,
                severity=types.DiagnosticSeverity.Error,
                code=_UNUSED_CODE,
                source=_UNUSED_SOURCE,
                data={"kind": "unused"},
            )
        )

    result.sort(key=lambda item: (item.range.start.line, item.range.start.character))
    return result


def _binding_range(outline: Outline, definition: Definition) -> types.Range:
    """Highlight the local spelling of an import, including an alias."""
    if definition.is_import:
        for token in reversed(significant(outline.tokens)):
            if (
                token.kind == "ident"
                and token.value == definition.name
                and definition.range.start.line <= token.line <= definition.range.end.line
                and (
                    token.line != definition.range.start.line
                    or token.col >= definition.range.start.character
                )
                and (
                    token.line != definition.range.end.line
                    or token.end_col <= definition.range.end.character
                )
            ):
                return range_of(token.line, token.col, token.end_line, token.end_col)
    return definition.selection_range


def _is_unused_candidate(definition: Definition, top_level: set[int]) -> bool:
    if definition.is_import:
        return True
    if id(definition) in top_level or definition.is_parameter:
        return False
    return definition.kind in (
        types.SymbolKind.Variable,
        types.SymbolKind.Constant,
        types.SymbolKind.Function,
    )


def _definition_parents(outline: Outline) -> dict[int, Definition | None]:
    parents: dict[int, Definition | None] = {}

    def visit(definitions: list[Definition], parent: Definition | None) -> None:
        for definition in definitions:
            parents[id(definition)] = parent
            visit(definition.children, definition)

    visit(outline.symbols, None)
    # Parameters are kept flat by the token outline, but they are not
    # candidates. Recording them makes resolution grouping complete and keeps
    # this helper correct if they become children in a future outline pass.
    for definition in outline.definitions:
        parents.setdefault(id(definition), None)
    return parents


def _declaration_tokens(outline: Outline) -> set[int]:
    """Token identities that spell declarations, not references."""
    declarations: set[int] = set()
    tokens = significant(outline.tokens)
    import_ranges = [
        definition.range
        for definition in outline.definitions
        if definition.is_import
    ]
    starts = {
        (definition.selection_range.start.line, definition.selection_range.start.character)
        for definition in outline.definitions
        if not definition.is_import
    }
    for token in tokens:
        position = (token.line, token.col)
        if any(
            (rng.start.line, rng.start.character)
            <= position
            <= (rng.end.line, rng.end.character)
            for rng in import_ranges
        ) or position in starts:
            declarations.add(id(token))
    return declarations


def to_lsp(diag: dict, source_lines: dict[str, list[str]]) -> types.Diagnostic | None:
    """Convert one diagnostic. `source_lines` maps a file path to its lines,
    so ranges can be widened using the text the editor actually has."""
    file = diag.get("file")
    line = diag.get("line")
    if not isinstance(file, str) or not isinstance(line, int):
        return None

    lineno = max(line - 1, 0)
    lines = source_lines.get(file)
    if lines is None:
        # multi-module runs report files that are not open; the compiler
        # already quoted the offending line for us
        quoted = diag.get("source_line")
        lines = [quoted] if isinstance(quoted, str) else []
        text = line_text(lines, 0)
    else:
        text = line_text(lines, lineno)
    column = diag.get("column") if isinstance(diag.get("column"), int) else 1
    start = byte_col_to_char_col(text, column)
    end = _token_end(text, start)

    message = diag.get("message") or "error"
    expected, actual = diag.get("expected"), diag.get("actual")
    if isinstance(expected, str) and isinstance(actual, str):
        message = f"{message}\n  expected: {expected}\n  actual:   {actual}"
    for note in diag.get("notes") or []:
        if isinstance(note, dict):
            message += f"\n  {note.get('label', 'note')}: {note.get('value', '')}"

    return types.Diagnostic(
        range=range_of(lineno, start, lineno, end),
        message=message,
        severity=_SEVERITY.get(diag.get("severity", "error"), types.DiagnosticSeverity.Error),
        code=diag.get("code"),
        source=SOURCE,
        data={"kind": diag.get("kind")} if diag.get("kind") else None,
    )


def _token_end(text: str, start: int) -> int:
    """Widen a point to the token starting there -- the caret in the human
    rendering points at a construct's first token (`docs/diagnostics.md`)."""
    if start >= len(text):
        return max(start + 1, len(text))
    tokens = tokenize(text[start:])
    # only a token beginning right at the caret and ending on the same line
    # describes the construct; anything else falls back to a single character
    if tokens and tokens[0].offset == 0 and tokens[0].end_line == 0:
        return start + tokens[0].end_col
    return start + 1


def group_by_uri(
    diagnostics: list[dict], source_lines: dict[str, list[str]]
) -> dict[str, list[types.Diagnostic]]:
    """Bucket diagnostics per file: a multi-module check reports errors in
    files other than the one being edited, and those belong on those files."""
    out: dict[str, list[types.Diagnostic]] = {}
    for raw in diagnostics:
        converted = to_lsp(raw, source_lines)
        if converted is None:
            continue
        file = raw["file"]
        uri = path_to_uri(file) if file != "<stdlib>" else None
        if uri is None:
            continue
        out.setdefault(uri, []).append(converted)
    return out
