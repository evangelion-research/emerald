"""The read-only language features, computed from the outline.

Each function here is pure -- source text and a position in, LSP payload out --
which keeps them testable without a client and keeps `server.py` down to
plumbing. Positions are 0-based lines and UTF-32 character columns; the server
converts to the client's encoding on the way out.

What these can and cannot do is set by the fact that there is no type
information yet (DESIGN.md 4b). Hover shows a declaration, not an inferred
type; completion after `.` works on module bindings, where the answer is
lexical, and stays silent on values, where it would need the checker. That
silence is deliberate: a wrong completion list is worse than none.
"""

from __future__ import annotations

from dataclasses import dataclass

from lsprotocol import types

from . import modules
from .language import BUILTINS, CONSTANTS, KEYWORDS, TYPE_ATOMS, describe
from .lexer import Token, significant, token_at
from .outline import Definition, ImportInfo, Outline
from .positions import contains, path_to_uri, range_of, token_range


@dataclass(slots=True)
class Context:
    """Everything a feature needs about one document."""

    path: str
    source: str
    outline: Outline
    include_paths: list[str]
    compiler: str | None

    def line(self, number: int) -> str:
        lines = self.source.splitlines()
        return lines[number] if 0 <= number < len(lines) else ""

    def offset_at(self, position: types.Position) -> int:
        offset = 0
        for i, line in enumerate(self.source.splitlines(keepends=True)):
            if i == position.line:
                return offset + min(position.character, len(line))
            offset += len(line)
        return len(self.source)

    def module_bindings(self) -> dict[str, ImportInfo]:
        """Local name -> the `import` statement that bound it."""
        out = {}
        for info in self.outline.imports:
            local = info.local_module_name
            if local:
                out[local] = info
        return out

    def resolve_module(self, module_path: str) -> modules.Resolved | None:
        return modules.resolve(
            module_path, self.path, self.include_paths, self.compiler
        )


# -- document symbols ----------------------------------------------------


def document_symbols(outline: Outline) -> list[types.DocumentSymbol]:
    """The outline view. Repeated assignments to one name collapse into a
    single entry -- `i = i + 1` three times is one variable, not three."""

    def convert(defs: list[Definition]) -> list[types.DocumentSymbol]:
        out: list[types.DocumentSymbol] = []
        seen: set[tuple[str, int]] = set()
        for d in defs:
            key = (d.name, int(d.kind))
            if key in seen:
                continue
            seen.add(key)
            out.append(
                types.DocumentSymbol(
                    name=d.name,
                    kind=d.kind,
                    detail=d.detail,
                    range=d.range,
                    selection_range=d.selection_range,
                    children=convert(d.children) or None,
                )
            )
        return out

    return convert(outline.symbols)


def folding_ranges(outline: Outline) -> list[types.FoldingRange]:
    return [
        types.FoldingRange(
            start_line=r.start.line,
            end_line=max(r.end.line - 1, r.start.line),
            kind=types.FoldingRangeKind.Region,
        )
        for r in outline.folds
    ]


# -- hover ---------------------------------------------------------------


def hover(ctx: Context, position: types.Position) -> types.Hover | None:
    token = token_at(ctx.outline.tokens, position.line, position.character)
    if token is None or token.kind not in ("ident", "keyword"):
        return None

    markdown = _describe_token(ctx, token, position)
    if markdown is None:
        return None
    return types.Hover(
        contents=types.MarkupContent(kind=types.MarkupKind.Markdown, value=markdown),
        range=token_range(token),
    )


def _describe_token(ctx: Context, token: Token, position: types.Position) -> str | None:
    name = token.value

    for info in ctx.outline.imports:
        if contains(info.module_range, position):
            return _module_markdown(ctx, info.module_path)

    binding = ctx.module_bindings().get(name)
    if binding is not None and token.kind == "ident":
        return _module_markdown(ctx, binding.module_path)

    local = ctx.outline.resolve(name, ctx.offset_at(position))
    if local is not None:
        kind = local.kind.name.lower()
        note = "" if local.exported else "\n\nprivate to this module"
        return f"```emerald\n{local.detail}\n```\n\n{kind}{note}"

    qualified = _qualified_base(ctx, token)
    if qualified is not None:
        exported = _module_exports(ctx, qualified)
        match = next((d for d in exported if d.name == name), None)
        if match is not None:
            return f"```emerald\n{match.detail}\n```\n\nfrom module `{qualified}`"

    return describe(name)


def _module_markdown(ctx: Context, module_path: str) -> str:
    """Hover for a module path: where it resolves to, or that it does not."""
    found = _module_file(ctx, module_path)
    where = found[0] if found else "*unresolved*"
    return f"**module `{module_path}`**\n\n{where}"


def _qualified_base(ctx: Context, token: Token) -> str | None:
    """For the `f` in `m.f`, the module path `m` names -- if `m` is one."""
    sig = significant(ctx.outline.tokens)
    for i, t in enumerate(sig):
        if t is token and i >= 2 and sig[i - 1].value == "." and sig[i - 1].kind == "op":
            info = ctx.module_bindings().get(sig[i - 2].value)
            return info.module_path if info else None
    return None


def _module_file(ctx: Context, module_path: str) -> tuple[str, list[Definition]] | None:
    """The file a module path resolves to and what it exports, or None if the
    path does not resolve under any search root."""
    resolved = ctx.resolve_module(module_path)
    if resolved is None:
        return None
    other = modules.read_outline(resolved.path)
    return resolved.path, (other.exports() if other else [])


def _module_exports(ctx: Context, module_path: str) -> list[Definition]:
    found = _module_file(ctx, module_path)
    return found[1] if found else []


# -- goto definition -----------------------------------------------------


def definition(ctx: Context, position: types.Position) -> list[types.Location]:
    token = token_at(ctx.outline.tokens, position.line, position.character)
    if token is None or token.kind != "ident":
        return []

    # on the module path of an import: jump to the module's file
    for info in ctx.outline.imports:
        if contains(info.module_range, position):
            return _module_location(ctx, info.module_path)
        for name in info.names:
            if contains(name.range, position):
                return _exported_location(
                    ctx, info.module_path, name.name
                ) or _module_location(ctx, info.module_path)

    binding = ctx.module_bindings().get(token.value)
    if binding is not None:
        return _module_location(ctx, binding.module_path)

    qualified = _qualified_base(ctx, token)
    if qualified is not None:
        found = _exported_location(ctx, qualified, token.value)
        if found:
            return found

    local = ctx.outline.resolve(token.value, ctx.offset_at(position))
    if local is not None:
        return [
            types.Location(uri=path_to_uri(ctx.path), range=local.selection_range)
        ]

    # a name lifted by `from m import x` resolves in the module it came from
    for info in ctx.outline.imports:
        for name in info.names:
            if name.local == token.value:
                return _exported_location(ctx, info.module_path, name.name)
    return []


def _module_location(ctx: Context, module_path: str) -> list[types.Location]:
    """The top of a module's file."""
    found = _module_file(ctx, module_path)
    if found is None:
        return []
    return [types.Location(uri=path_to_uri(found[0]), range=range_of(0, 0, 0, 0))]


def _exported_location(
    ctx: Context, module_path: str, name: str
) -> list[types.Location]:
    """Where a module defines the name it exports -- empty if either the
    module or the name does not resolve."""
    found = _module_file(ctx, module_path)
    if found is None:
        return []
    path, exports = found
    return [
        types.Location(uri=path_to_uri(path), range=d.selection_range)
        for d in exports
        if d.name == name
    ][:1]


# -- references and highlights (this file only) --------------------------


def occurrences(ctx: Context, position: types.Position) -> list[Token]:
    """Every token in this file spelling the same name.

    Single-file and name-based: cross-module references need the symbol table
    of DESIGN.md 4c, and shadowing needs scopes the checker owns.
    """
    token = token_at(ctx.outline.tokens, position.line, position.character)
    if token is None or token.kind != "ident":
        return []
    return [
        t
        for t in ctx.outline.tokens
        if t.kind == "ident" and t.value == token.value
    ]


def references(ctx: Context, position: types.Position) -> list[types.Location]:
    uri = path_to_uri(ctx.path)
    return [types.Location(uri=uri, range=token_range(t)) for t in occurrences(ctx, position)]


def highlights(ctx: Context, position: types.Position) -> list[types.DocumentHighlight]:
    return [
        types.DocumentHighlight(range=token_range(t), kind=types.DocumentHighlightKind.Text)
        for t in occurrences(ctx, position)
    ]


# -- completion ----------------------------------------------------------


def completions(ctx: Context, position: types.Position) -> types.CompletionList:
    line = ctx.line(position.line)[: position.character]
    stripped = line.strip()
    words = stripped.split()

    # `import <here>` / `from <here>`: module paths under the search roots
    if words and words[0] in ("import", "from") and "import" not in words[1:]:
        if len(words) <= 2 and not stripped.endswith(","):
            return _module_completions(ctx)

    # `from m import <here>`: exactly m's exported names (module.c mod_exports)
    if words and words[0] == "from" and "import" in words:
        module_path = words[1] if len(words) > 1 else ""
        return _export_completions(ctx, module_path)

    # `m.<here>`: a module's exports. On a value we would need the checker's
    # type for the receiver, so we stay quiet rather than guess.
    dotted = _dotted_base(line)
    if dotted is not None:
        info = ctx.module_bindings().get(dotted)
        if info is None:
            return types.CompletionList(is_incomplete=True, items=[])
        return _export_completions(ctx, info.module_path)

    return types.CompletionList(is_incomplete=False, items=_scope_completions(ctx, position))


def _dotted_base(prefix: str) -> str | None:
    """The `m` in a `m.par|` prefix, if the cursor is in a field position."""
    head = prefix.rstrip()
    if head != prefix.rstrip(" "):  # a space after the dot ends the access
        return None
    i = len(prefix)
    while i > 0 and (prefix[i - 1].isalnum() or prefix[i - 1] == "_"):
        i -= 1
    if i == 0 or prefix[i - 1] != ".":
        return None
    j = i - 1
    while j > 0 and (prefix[j - 1].isalnum() or prefix[j - 1] == "_"):
        j -= 1
    base = prefix[j : i - 1]
    return base or None


def _module_completions(ctx: Context) -> types.CompletionList:
    names = modules.module_candidates(ctx.path, ctx.include_paths, ctx.compiler)
    return types.CompletionList(
        is_incomplete=False,
        items=[
            types.CompletionItem(
                label=name,
                kind=types.CompletionItemKind.Module,
                detail="module",
            )
            for name in names
        ],
    )


def _export_completions(ctx: Context, module_path: str) -> types.CompletionList:
    items = [
        types.CompletionItem(
            label=d.name,
            kind=_completion_kind(d.kind),
            detail=d.detail,
            documentation=f"from module `{module_path}`",
        )
        for d in _module_exports(ctx, module_path)
    ]
    return types.CompletionList(is_incomplete=False, items=items)


def _scope_completions(ctx: Context, position: types.Position) -> list[types.CompletionItem]:
    offset = ctx.offset_at(position)
    items: list[types.CompletionItem] = []
    seen: set[str] = set()

    for d in ctx.outline.visible_at(offset):
        if d.name in seen:
            continue
        seen.add(d.name)
        items.append(
            types.CompletionItem(
                label=d.name,
                kind=_completion_kind(d.kind),
                detail=d.detail,
                sort_text=f"0{d.name}",
            )
        )

    for name, doc in KEYWORDS.items():
        items.append(
            types.CompletionItem(
                label=name,
                kind=types.CompletionItemKind.Keyword,
                detail="keyword",
                documentation=doc,
                sort_text=f"1{name}",
            )
        )
    for name, doc in {**TYPE_ATOMS, **CONSTANTS}.items():
        items.append(
            types.CompletionItem(
                label=name,
                kind=types.CompletionItemKind.Class,
                detail="built-in type",
                documentation=doc,
                sort_text=f"2{name}",
            )
        )
    for name, builtin in BUILTINS.items():
        if name in seen:
            continue
        items.append(
            types.CompletionItem(
                label=name,
                kind=types.CompletionItemKind.Function,
                detail=builtin.signature,
                documentation=builtin.documentation(),
                sort_text=f"3{name}",
            )
        )
    return items


def _completion_kind(kind: types.SymbolKind) -> types.CompletionItemKind:
    return {
        types.SymbolKind.Function: types.CompletionItemKind.Function,
        types.SymbolKind.Interface: types.CompletionItemKind.Interface,
        types.SymbolKind.Struct: types.CompletionItemKind.Struct,
        types.SymbolKind.Module: types.CompletionItemKind.Module,
        types.SymbolKind.Constant: types.CompletionItemKind.Constant,
        types.SymbolKind.TypeParameter: types.CompletionItemKind.TypeParameter,
    }.get(kind, types.CompletionItemKind.Variable)
