"""The pygls server: protocol plumbing, and nothing else.

`pygls` owns framing, the lifecycle, and the document store; this module owns
the handlers, the debounce, and the analysis cache (DESIGN.md 5). Everything a
handler answers with comes from `features` (syntax-only, always available) or
from `compiler` (the checker, via a subprocess).

Two rules keep the file honest:

* Every position crossing the wire goes through the document's position codec.
  Internally the server works in UTF-32 characters; the client's encoding is
  applied here and nowhere else.
* No analysis runs on the event loop. `emeraldc` is invoked in a thread, after
  a debounce, and a newer document version cancels the run in flight.
"""

from __future__ import annotations

import asyncio
import logging
from pathlib import Path

from lsprotocol import types
from pygls.lsp.server import LanguageServer

from . import compiler, diagnostics, features, semantic
from .outline import build
from .positions import path_to_uri, uri_to_path

log = logging.getLogger(__name__)

SERVER_NAME = "emerald-lsp"
SERVER_VERSION = "0.1.0"
# how many .rald files a workspace-symbol query is willing to read
WORKSPACE_SCAN_LIMIT = 2000


class EmeraldLanguageServer(LanguageServer):
    def __init__(self) -> None:
        super().__init__(
            SERVER_NAME,
            SERVER_VERSION,
            text_document_sync_kind=types.TextDocumentSyncKind.Incremental,
        )
        self.settings = compiler.Settings()
        self.compiler_path: str | None = None
        self.compiler_error: str | None = None
        self._warned = False
        self._pending: dict[str, asyncio.Task] = {}
        self._published: set[str] = set()

    # -- configuration ---------------------------------------------------
    def configure(self, options: object) -> None:
        self.settings = compiler.Settings.from_object(options)
        self.locate_compiler()

    def locate_compiler(self) -> None:
        try:
            self.compiler_path = compiler.find_compiler(self.settings)
            self.compiler_error = None
        except compiler.CompilerNotFound as exc:
            self.compiler_path, self.compiler_error = None, str(exc)

    def warn_once(self, message: str) -> None:
        """Report a missing or broken compiler once per session -- a message
        per keystroke would be worse than the missing diagnostics."""
        if self._warned:
            return
        self._warned = True
        self.window_show_message(
            types.ShowMessageParams(
                type=types.MessageType.Warning,
                message=f"{SERVER_NAME}: {message}. "
                "Syntax features and unused-code diagnostics stay available; "
                "compiler diagnostics need emeraldc.",
            )
        )

    # -- per-document state ----------------------------------------------
    def context(self, uri: str) -> features.Context | None:
        path = uri_to_path(uri)
        if path is None:
            return None
        doc = self.workspace.get_text_document(uri)
        source = doc.source
        return features.Context(
            path=path,
            source=source,
            outline=build(source),
            include_paths=compiler.include_paths_for(path, self.settings),
            compiler=self.compiler_path,
        )

    # -- diagnostics -----------------------------------------------------
    def schedule_check(self, uri: str) -> None:
        if not self.settings.diagnostics_enabled:
            return
        previous = self._pending.pop(uri, None)
        if previous is not None:
            previous.cancel()  # a newer version supersedes the run in flight
        self._pending[uri] = asyncio.create_task(self._check_later(uri))

    async def _check_later(self, uri: str) -> None:
        try:
            await asyncio.sleep(self.settings.debounce_ms / 1000)
            await self.check_now(uri)
        except asyncio.CancelledError:  # superseded; nothing to clean up
            raise
        except Exception:  # pragma: no cover -- a handler must never die
            log.exception("diagnostics failed for %s", uri)
        finally:
            self._pending.pop(uri, None)

    async def check_now(self, uri: str) -> None:
        path = uri_to_path(uri)
        if path is None:
            return

        doc = self.workspace.get_text_document(uri)
        source = doc.source
        local = diagnostics.unused_diagnostics(build(source))

        if self.compiler_path is None:
            self.locate_compiler()
        if self.compiler_path is None:
            self.warn_once(self.compiler_error or "emeraldc not found")
            self._publish({uri: local})
            return

        on_disk = _read(path)
        result = await asyncio.to_thread(
            compiler.check,
            path,
            None if source == on_disk else source,
            self.settings,
            compiler=self.compiler_path,
        )
        if not result.ok:
            self.warn_once(result.detail)
            self._publish({uri: local})
            return

        grouped = diagnostics.group_by_uri(result.diagnostics, {path: source.splitlines()})
        grouped.setdefault(uri, []).extend(local)
        self._publish(grouped)

    def _publish(self, grouped: dict[str, list[types.Diagnostic]]) -> None:
        """Publish compiler and local diagnostics, clearing stale files."""
        for stale in self._published - grouped.keys():
            self.text_document_publish_diagnostics(
                types.PublishDiagnosticsParams(uri=stale, diagnostics=[])
            )
        for target, items in grouped.items():
            self.text_document_publish_diagnostics(
                types.PublishDiagnosticsParams(
                    uri=target, diagnostics=[self.to_client(target, d) for d in items]
                )
            )
        self._published = set(grouped)

    def forget(self, uri: str) -> None:
        """Drop a closed document: cancel its pending run and clear the
        diagnostics the client is still showing for it."""
        task = self._pending.pop(uri, None)
        if task is not None:
            task.cancel()
        self.text_document_publish_diagnostics(
            types.PublishDiagnosticsParams(uri=uri, diagnostics=[])
        )
        self._published.discard(uri)

    # -- encoding boundary ------------------------------------------------
    def client_range(self, uri: str, rng: types.Range) -> types.Range:
        """UTF-32 character columns -> whatever the client negotiated."""
        lines = self._lines(uri)
        codec = self.workspace.position_codec
        return codec.range_to_client_units(lines, rng)

    def server_position(self, uri: str, position: types.Position) -> types.Position:
        lines = self._lines(uri)
        codec = self.workspace.position_codec
        return codec.position_from_client_units(lines, position)

    def _lines(self, uri: str) -> list[str]:
        doc = self.workspace.text_documents.get(uri)
        if doc is not None:
            return doc.lines
        path = uri_to_path(uri)
        return _read(path).splitlines(keepends=True) if path else []

    def to_client(self, uri: str, diag: types.Diagnostic) -> types.Diagnostic:
        diag.range = self.client_range(uri, diag.range)
        return diag

    def location_to_client(self, location: types.Location) -> types.Location:
        location.range = self.client_range(location.uri, location.range)
        return location


server = EmeraldLanguageServer()


def _read(path: str | None) -> str:
    if not path:
        return ""
    try:
        return Path(path).read_text(encoding="utf-8")
    except OSError:
        return ""


# -- lifecycle -----------------------------------------------------------


@server.feature(types.INITIALIZE)
def initialize(params: types.InitializeParams) -> None:
    server.configure(getattr(params, "initialization_options", None))
    if server.compiler_path:
        version = compiler.probe(server.compiler_path)
        log.info("using %s (%s)", server.compiler_path, version or "version unknown")


@server.feature(types.WORKSPACE_DID_CHANGE_CONFIGURATION)
def did_change_configuration(params: types.DidChangeConfigurationParams) -> None:
    server.configure(params.settings)
    for uri in list(server.workspace.text_documents):
        server.schedule_check(uri)


@server.feature(types.WORKSPACE_DID_CHANGE_WATCHED_FILES)
def did_change_watched_files(params: types.DidChangeWatchedFilesParams) -> None:
    """A dependency, a manifest, or a lockfile changed on disk.

    `emerald.toml` / `emerald.lock` change the `-I` set (DESIGN.md, "pme
    resolves, the LSP consumes"); a sibling `.rald` changes what the open
    buffers see. Either way every open document is re-checked -- crude, but
    correct until the reverse-dependency map of DESIGN.md 2b exists.
    """
    del params
    for uri in list(server.workspace.text_documents):
        server.schedule_check(uri)


# -- document sync -------------------------------------------------------


@server.feature(types.TEXT_DOCUMENT_DID_OPEN)
@server.feature(types.TEXT_DOCUMENT_DID_CHANGE)
@server.feature(types.TEXT_DOCUMENT_DID_SAVE)
def did_touch_document(params: types.DidOpenTextDocumentParams) -> None:
    """Open, edit, and save all mean the same thing here: re-check the buffer.

    The debounce in `schedule_check` is what keeps a burst of edits from
    turning into a burst of subprocesses.
    """
    server.schedule_check(params.text_document.uri)


@server.feature(types.TEXT_DOCUMENT_DID_CLOSE)
def did_close(params: types.DidCloseTextDocumentParams) -> None:
    server.forget(params.text_document.uri)


# -- syntax-only features ------------------------------------------------


@server.feature(
    types.TEXT_DOCUMENT_SEMANTIC_TOKENS_FULL, semantic.LEGEND
)
def semantic_tokens(params: types.SemanticTokensParams) -> types.SemanticTokens:
    uri = params.text_document.uri
    ctx = server.context(uri)
    if ctx is None:
        return types.SemanticTokens(data=[])
    lines = ctx.source.splitlines()
    modules_bound = frozenset(ctx.module_bindings())
    data = semantic.encode(ctx.outline.tokens, lines, modules_bound)
    return types.SemanticTokens(data=_tokens_to_client(server, uri, data, lines))


def _tokens_to_client(
    ls: EmeraldLanguageServer, uri: str, data: list[int], lines: list[str]
) -> list[int]:
    """Re-encode column deltas in the client's units.

    Deltas are relative, so converting them means walking back to absolute
    columns first; doing it here keeps the conversion at the boundary.
    """
    codec = ls.workspace.position_codec
    if codec.encoding == types.PositionEncodingKind.Utf32:
        return data

    out: list[int] = []
    line = col = 0
    prev_line = prev_col = 0
    for i in range(0, len(data), 5):
        delta_line, delta_col, length, ttype, mods = data[i : i + 5]
        line += delta_line
        col = delta_col if delta_line else col + delta_col
        text = lines[line] if line < len(lines) else ""
        start = codec.impl.num_units(text[:col])
        end = codec.impl.num_units(text[: col + length])
        out += [
            line - prev_line,
            start - prev_col if line == prev_line else start,
            end - start,
            ttype,
            mods,
        ]
        prev_line, prev_col = line, start
    return out


@server.feature(types.TEXT_DOCUMENT_DOCUMENT_SYMBOL)
def document_symbol(
    params: types.DocumentSymbolParams,
) -> list[types.DocumentSymbol]:
    uri = params.text_document.uri
    ctx = server.context(uri)
    if ctx is None:
        return []
    symbols = features.document_symbols(ctx.outline)
    for symbol in _walk(symbols):
        symbol.range = server.client_range(uri, symbol.range)
        symbol.selection_range = server.client_range(uri, symbol.selection_range)
    return symbols


def _walk(symbols: list[types.DocumentSymbol]):
    for symbol in symbols:
        yield symbol
        yield from _walk(symbol.children or [])


@server.feature(types.TEXT_DOCUMENT_FOLDING_RANGE)
def folding_range(params: types.FoldingRangeParams) -> list[types.FoldingRange]:
    ctx = server.context(params.text_document.uri)
    return features.folding_ranges(ctx.outline) if ctx else []


@server.feature(types.TEXT_DOCUMENT_HOVER)
def hover(params: types.HoverParams) -> types.Hover | None:
    uri = params.text_document.uri
    ctx = server.context(uri)
    if ctx is None:
        return None
    result = features.hover(ctx, server.server_position(uri, params.position))
    if result is not None and result.range is not None:
        result.range = server.client_range(uri, result.range)
    return result


@server.feature(types.TEXT_DOCUMENT_DEFINITION)
def definition(params: types.DefinitionParams) -> list[types.Location]:
    uri = params.text_document.uri
    ctx = server.context(uri)
    if ctx is None:
        return []
    found = features.definition(ctx, server.server_position(uri, params.position))
    return [server.location_to_client(loc) for loc in found]


@server.feature(types.TEXT_DOCUMENT_REFERENCES)
def references(params: types.ReferenceParams) -> list[types.Location]:
    uri = params.text_document.uri
    ctx = server.context(uri)
    if ctx is None:
        return []
    found = features.references(ctx, server.server_position(uri, params.position))
    return [server.location_to_client(loc) for loc in found]


@server.feature(types.TEXT_DOCUMENT_DOCUMENT_HIGHLIGHT)
def document_highlight(
    params: types.DocumentHighlightParams,
) -> list[types.DocumentHighlight]:
    uri = params.text_document.uri
    ctx = server.context(uri)
    if ctx is None:
        return []
    found = features.highlights(ctx, server.server_position(uri, params.position))
    for item in found:
        item.range = server.client_range(uri, item.range)
    return found


@server.feature(
    types.TEXT_DOCUMENT_COMPLETION,
    types.CompletionOptions(trigger_characters=[".", " "]),
)
def completion(params: types.CompletionParams) -> types.CompletionList:
    uri = params.text_document.uri
    ctx = server.context(uri)
    if ctx is None:
        return types.CompletionList(is_incomplete=False, items=[])
    return features.completions(ctx, server.server_position(uri, params.position))


@server.feature(types.WORKSPACE_SYMBOL)
def workspace_symbol(params: types.WorkspaceSymbolParams) -> list[types.WorkspaceSymbol]:
    """Top-level symbols across the workspace's `.rald` files.

    Files are read from disk except where a buffer is open, which is the same
    overlay discipline the checker needs (DESIGN.md 2a) applied to search.
    """
    query = params.query.lower()
    out: list[types.WorkspaceSymbol] = []
    for path in _workspace_files():
        uri = path_to_uri(path)
        doc = server.workspace.text_documents.get(uri)
        source = doc.source if doc is not None else _read(path)
        if not source:
            continue
        for symbol in build(source).symbols:
            if query and query not in symbol.name.lower():
                continue
            out.append(
                types.WorkspaceSymbol(
                    name=symbol.name,
                    kind=symbol.kind,
                    location=types.Location(
                        uri=uri, range=server.client_range(uri, symbol.selection_range)
                    ),
                    container_name=Path(path).stem,
                )
            )
    return out


def _workspace_files() -> list[str]:
    seen: list[str] = []
    for folder in server.workspace.folders.values():
        root = uri_to_path(folder.uri)
        if root is None:
            continue
        for path in Path(root).rglob("*.rald"):
            if any(part.startswith(".") for part in path.parts):
                continue
            seen.append(str(path))
            if len(seen) >= WORKSPACE_SCAN_LIMIT:
                return seen
    return seen
