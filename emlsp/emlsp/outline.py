"""A token-level outline of one Emerald file.

This is deliberately *not* a parser. The compiler owns parsing and typing; what
this gives the server is the per-file, syntax-only layer the design calls for
(DESIGN.md 6.2/6.3): document symbols, folding ranges, import structure, and
a scope-aware list of names to complete and jump to. Being token-level, it
degrades gracefully -- a half-typed file still yields every symbol above the
cursor, which is the state an editor buffer is usually in.

Its limits are worth stating plainly: no types, and no cross-file resolution
beyond following an `import` to its file. Both arrive with `--lsp-index`
(DESIGN.md 3), and the handlers that use this module are the ones that get
rewritten then.
"""

from __future__ import annotations

from dataclasses import dataclass, field

from lsprotocol import types

from .lexer import Token, significant, tokenize
from .positions import range_of, span, token_range


@dataclass(slots=True)
class Definition:
    """A named thing defined in this file, with the scope it is visible in."""

    name: str
    kind: types.SymbolKind
    detail: str
    range: types.Range  # the whole construct, for the outline
    selection_range: types.Range  # just the name, for goto-definition
    scope_start: int  # character offsets bounding visibility
    scope_end: int
    exported: bool  # module.c:235 -- a leading underscore means private
    children: list["Definition"] = field(default_factory=list)
    is_parameter: bool = False
    is_import: bool = False


@dataclass(slots=True)
class ImportedName:
    name: str
    alias: str | None
    range: types.Range

    @property
    def local(self) -> str:
        return self.alias or self.name


@dataclass(slots=True)
class ImportInfo:
    """One `import a.b [as c]` or `from a.b import x [as y], ...` statement."""

    kind: str  # "import" | "from"
    module_path: str  # dotted, as written
    module_range: types.Range
    alias: str | None
    names: list[ImportedName]
    range: types.Range

    @property
    def local_module_name(self) -> str | None:
        """What this statement binds for a plain `import` (modules.md: the
        path's last component, unless renamed with `as`)."""
        if self.kind != "import":
            return None
        return self.alias or self.module_path.rsplit(".", 1)[-1]


@dataclass(slots=True)
class Outline:
    tokens: list[Token]
    symbols: list[Definition]  # top-level, nested via .children
    definitions: list[Definition]  # flat, every scope
    imports: list[ImportInfo]
    folds: list[types.Range]

    def visible_at(self, offset: int) -> list[Definition]:
        """Definitions in scope at a character offset, innermost first."""
        hits = [d for d in self.definitions if d.scope_start <= offset <= d.scope_end]
        hits.sort(key=lambda d: d.scope_start, reverse=True)
        return hits

    def resolve(self, name: str, offset: int) -> Definition | None:
        for d in self.visible_at(offset):
            if d.name == name:
                return d
        return None

    def exports(self) -> list[Definition]:
        """What another module may import from this one (modules.md,
        "Exports and privacy"): top-level, non-underscore names."""
        return [d for d in self.symbols if d.exported]


def build(source: str) -> Outline:
    return _Builder(source).run()


@dataclass(slots=True)
class _Frame:
    """An enclosing `def` body: where nested definitions are filed, and the
    character range they are visible in."""

    owner: Definition
    depth: int  # brace depth the body closes at
    scope_start: int
    scope_end: int


_TOPLEVEL_KIND = {
    "def": types.SymbolKind.Function,
    "type": types.SymbolKind.Interface,
    "error": types.SymbolKind.Struct,
}

_CLOSERS = {"{": "}", "(": ")", "[": "]"}


class _Builder:
    def __init__(self, source: str) -> None:
        self.source = source
        self.all_tokens = tokenize(source)
        self.toks = significant(self.all_tokens)
        self.defs: list[Definition] = []
        self.roots: list[Definition] = []
        self.imports: list[ImportInfo] = []
        self.folds: list[types.Range] = []
        self.stack: list[_Frame] = []  # the enclosing def chain

    # -- token helpers ----------------------------------------------------
    def at(self, i: int) -> Token | None:
        return self.toks[i] if 0 <= i < len(self.toks) else None

    def is_op(self, i: int, *ops: str) -> bool:
        t = self.at(i)
        return t is not None and t.kind == "op" and t.value in ops

    def is_kw(self, i: int, *words: str) -> bool:
        t = self.at(i)
        return t is not None and t.kind == "keyword" and t.value in words

    def matching(self, i: int) -> int:
        """Index of the token closing the bracket at `i`, or the last token."""
        open_tok = self.toks[i]
        close = _CLOSERS[open_tok.value]
        depth = 0
        for j in range(i, len(self.toks)):
            t = self.toks[j]
            if t.kind != "op":
                continue
            if t.value == open_tok.value:
                depth += 1
            elif t.value == close:
                depth -= 1
                if depth == 0:
                    return j
        return len(self.toks) - 1

    # -- construction -----------------------------------------------------
    def add(self, d: Definition) -> None:
        self.defs.append(d)
        if self.stack:
            self.stack[-1].owner.children.append(d)
        else:
            self.roots.append(d)

    def run(self) -> Outline:
        depth = 0
        i = 0
        while i < len(self.toks):
            t = self.toks[i]

            if t.kind == "op" and t.value == "{":
                depth += 1
                i += 1
                continue
            if t.kind == "op" and t.value == "}":
                depth -= 1
                # leaving a body closes the scope anything inside it belongs to
                while self.stack and depth < self.stack[-1].depth:
                    self.stack.pop()
                i += 1
                continue

            if t.kind == "keyword":
                if t.value == "def":
                    i = self.func(i, depth)
                    continue
                if t.value in ("type", "error"):
                    i = self.type_like(i, t.value, depth)
                    continue
                if t.value == "dim":
                    i = self.dims(i, depth)
                    continue
                if t.value == "const":
                    i = self.binding(i + 1, depth, const=True)
                    continue
                if t.value in ("import", "from"):
                    i = self.import_stmt(i)
                    continue
                i += 1
                continue

            if t.kind == "ident" and self.assigns(i):
                i = self.binding(i, depth, const=False)
                continue

            i += 1

        self.fold_blocks()
        return Outline(self.all_tokens, self.roots, self.defs, self.imports, self.folds)

    def assigns(self, i: int) -> bool:
        """`x = e` or `x: T = e` -- a binding, not an expression statement."""
        if self.is_op(i + 1, "="):
            return True
        if not self.is_op(i + 1, ":"):
            return False
        for j in range(i + 2, min(i + 40, len(self.toks))):
            if self.is_op(j, "="):
                return True
            if self.is_op(j, "{", "}", ";") or self.toks[j].line != self.toks[i].line:
                return False
        return False

    def scope_bounds(self) -> tuple[int, int]:
        """A name is visible to the end of the block that encloses it."""
        if self.stack:
            return self.stack[-1].scope_start, self.stack[-1].scope_end
        return 0, len(self.source)

    def func(self, i: int, depth: int) -> int:
        name_tok = self.at(i + 1)
        if name_tok is None or name_tok.kind != "ident":
            return i + 1

        # signature text: from `def` to the body's `{`, or end of line
        j = i + 2
        brace = None
        while j < len(self.toks):
            if self.is_op(j, "{"):
                brace = j
                break
            if self.toks[j].kind == "keyword" and self.toks[j].value == "def":
                break
            j += 1

        end_tok = self.toks[self.matching(brace)] if brace is not None else name_tok
        detail = _slice(self.source, self.toks[i], self.toks[(brace or j) - 1])
        outer_start, outer_end = self.scope_bounds()

        d = Definition(
            name=name_tok.value,
            kind=types.SymbolKind.Function,
            detail=detail,
            range=span(self.toks[i], end_tok),
            selection_range=token_range(name_tok),
            # a `def` is visible in the whole enclosing scope: Emerald links
            # top-level names as a set, so forward references are legal
            scope_start=outer_start,
            scope_end=outer_end,
            exported=depth == 0 and not name_tok.value.startswith("_"),
        )
        self.add(d)

        if brace is None:
            return i + 2

        body_start = self.toks[brace].offset
        body_end = end_tok.offset + len(end_tok.value)
        self.params(i + 2, brace, body_start, body_end)
        # `d` keeps the whole construct's scope; the frame carries the *body*
        # scope, which is what anything declared inside is visible in
        self.stack.append(_Frame(d, depth + 1, body_start, body_end))
        return i + 2

    def params(self, start: int, brace: int, scope_start: int, scope_end: int) -> None:
        """Bind the header's parameter names inside the body's scope."""
        i = start
        while i < brace and not self.is_op(i, "("):
            i += 1
        if i >= brace:
            return
        close = self.matching(i)
        depth = 0
        j = i
        while j < close:
            t = self.toks[j]
            if t.kind == "op" and t.value in "([{":
                depth += 1
            elif t.kind == "op" and t.value in ")]}":
                depth -= 1
            elif depth == 1 and t.kind == "ident" and self.is_op(j - 1, "(", ","):
                self.defs.append(
                    Definition(
                        name=t.value,
                        kind=types.SymbolKind.Variable,
                        detail=_param_detail(self.source, self.toks, j, close),
                        range=token_range(t),
                        selection_range=token_range(t),
                        scope_start=scope_start,
                        scope_end=scope_end,
                        exported=False,
                        is_parameter=True,
                    )
                )
            j += 1

    def type_like(self, i: int, keyword: str, depth: int) -> int:
        name_tok = self.at(i + 1)
        if name_tok is None or name_tok.kind != "ident":
            return i + 1
        end = name_tok
        if keyword == "type":
            j = i + 2
            while j < len(self.toks) and self.toks[j].line == name_tok.line:
                end = self.toks[j]
                j += 1
        elif self.is_op(i + 2, "{"):
            end = self.toks[self.matching(i + 2)]
        start, stop = self.scope_bounds()
        self.add(
            Definition(
                name=name_tok.value,
                kind=_TOPLEVEL_KIND[keyword],
                detail=_slice(self.source, self.toks[i], end),
                range=span(self.toks[i], end),
                selection_range=token_range(name_tok),
                scope_start=start,
                scope_end=stop,
                exported=depth == 0 and not name_tok.value.startswith("_"),
            )
        )
        return i + 2

    def dims(self, i: int, depth: int) -> int:
        """`dim Batch, Seq` -- nominally distinct dimension names."""
        start, stop = self.scope_bounds()
        j = i + 1
        while j < len(self.toks):
            t = self.toks[j]
            if t.kind != "ident":
                break
            self.add(
                Definition(
                    name=t.value,
                    kind=types.SymbolKind.TypeParameter,
                    detail=f"dim {t.value}",
                    range=token_range(t),
                    selection_range=token_range(t),
                    scope_start=start,
                    scope_end=stop,
                    exported=depth == 0 and not t.value.startswith("_"),
                )
            )
            if not self.is_op(j + 1, ","):
                break
            j += 2
        return j + 1

    def binding(self, i: int, depth: int, const: bool) -> int:
        name_tok = self.at(i)
        if name_tok is None or name_tok.kind != "ident":
            return i + 1
        end = name_tok
        j = i + 1
        while j < len(self.toks) and self.toks[j].line == name_tok.line:
            if self.is_op(j, "}", ";"):  # `if c { x = 1 }` -- the brace is not ours
                break
            end = self.toks[j]
            j += 1
        start, stop = self.scope_bounds()
        # a binding is visible from its own declaration onward, not before
        self.add(
            Definition(
                name=name_tok.value,
                kind=types.SymbolKind.Constant if const else types.SymbolKind.Variable,
                detail=_slice(self.source, self.toks[i - 1] if const else name_tok, end),
                range=span(name_tok, end),
                selection_range=token_range(name_tok),
                scope_start=name_tok.offset,
                scope_end=stop,
                exported=depth == 0 and not name_tok.value.startswith("_"),
            )
        )
        return j

    def import_stmt(self, i: int) -> int:
        kw = self.toks[i]
        j = i + 1
        parts: list[Token] = []
        while j < len(self.toks):
            t = self.toks[j]
            if t.kind == "ident":
                parts.append(t)
                j += 1
                if self.is_op(j, "."):
                    j += 1
                    continue
            break
        if not parts:
            return i + 1

        module_path = ".".join(p.value for p in parts)
        module_range = span(parts[0], parts[-1])
        alias: str | None = None
        names: list[ImportedName] = []
        end = parts[-1]

        if kw.value == "import":
            if self.is_kw(j, "as") and (a := self.at(j + 1)) and a.kind == "ident":
                alias, end, j = a.value, a, j + 2
        else:  # from a.b import x as y, z
            if self.is_kw(j, "import"):
                j += 1
                while j < len(self.toks):
                    t = self.at(j)
                    if t is None or t.kind != "ident":
                        break
                    nm, na, end, j = t.value, None, t, j + 1
                    if self.is_kw(j, "as") and (a := self.at(j + 1)) and a.kind == "ident":
                        na, end, j = a.value, a, j + 2
                    names.append(
                        ImportedName(
                            nm, na, token_range(t)
                        )
                    )
                    if not self.is_op(j, ","):
                        break
                    j += 1

        info = ImportInfo(
            kind=kw.value,
            module_path=module_path,
            module_range=module_range,
            alias=alias,
            names=names,
            range=span(kw, end),
        )
        self.imports.append(info)

        # the names an import binds are ordinary top-level definitions
        bound: list[tuple[str, types.Range]] = []
        if info.kind == "import":
            local = info.local_module_name
            if local:
                bound.append((local, module_range))
        else:
            bound.extend((n.local, n.range) for n in names)
        for name, rng in bound:
            self.add(
                Definition(
                    name=name,
                    kind=types.SymbolKind.Module
                    if info.kind == "import"
                    else types.SymbolKind.Variable,
                    detail=_slice(self.source, kw, end),
                    range=info.range,
                    selection_range=rng,
                    scope_start=0,
                    scope_end=len(self.source),
                    exported=False,  # an import binding is not re-exported
                    is_import=True,
                )
            )
        return j

    def fold_blocks(self) -> None:
        stack: list[Token] = []
        for t in self.toks:
            if t.kind != "op":
                continue
            if t.value == "{":
                stack.append(t)
            elif t.value == "}" and stack:
                open_tok = stack.pop()
                if t.line > open_tok.line:
                    self.folds.append(
                        range_of(open_tok.line, open_tok.col, t.line, t.end_col)
                    )


def _slice(source: str, first: Token, last: Token) -> str:
    text = source[first.offset : last.offset + len(last.value)]
    return " ".join(text.split())


def _param_detail(source: str, toks: list[Token], i: int, close: int) -> str:
    """`x: int` for an annotated parameter, else just the name."""
    end = i
    j = i + 1
    if j < close and toks[j].kind == "op" and toks[j].value == ":":
        depth = 0
        while j < close:
            t = toks[j]
            if t.kind == "op" and t.value in "([{":
                depth += 1
            elif t.kind == "op" and t.value in ")]}":
                depth -= 1
            elif t.kind == "op" and t.value == "," and depth == 0:
                break
            end = j
            j += 1
    return _slice(source, toks[i], toks[end])
