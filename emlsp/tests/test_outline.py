import pytest
from lsprotocol import types

from emlsp.outline import build

SOURCE = """\
import strings
from result import Result, Err as Failure

type Word = str
error NotFound { key: str }
dim Batch, Seq

const LIMIT = 10

def split(s: str, sep: str) -> list[str] pure {
    out: list[str] = []
    i = 0
    def _inner(x: int) -> int {
        local = x + 1
        return local
    }
    return out
}

def _private(x: int) -> int { return x }
"""


@pytest.fixture(scope="module")
def outline():
    return build(SOURCE)


@pytest.fixture(scope="module")
def by_name(outline):
    return {d.name: d for d in outline.symbols}


def test_top_level_symbols(outline):
    assert [d.name for d in outline.symbols] == [
        "strings", "Result", "Failure", "Word", "NotFound", "Batch", "Seq",
        "LIMIT", "split", "_private",
    ]


@pytest.mark.parametrize(
    "name, kind",
    [
        ("Word", types.SymbolKind.Interface),
        ("NotFound", types.SymbolKind.Struct),
        ("Batch", types.SymbolKind.TypeParameter),
        ("LIMIT", types.SymbolKind.Constant),
        ("split", types.SymbolKind.Function),
        ("strings", types.SymbolKind.Module),
    ],
)
def test_kinds(by_name, name, kind):
    assert by_name[name].kind == kind


def test_detail_is_the_signature(by_name):
    assert by_name["split"].detail == "def split(s: str, sep: str) -> list[str] pure"


def test_underscore_is_private(outline, by_name):
    # module.c:235 is_private -- and an import binding is never re-exported
    assert by_name["split"].exported
    assert not by_name["_private"].exported
    assert not by_name["strings"].exported
    assert [d.name for d in outline.exports()] == [
        "Word", "NotFound", "Batch", "Seq", "LIMIT", "split"
    ]


def test_nested_defs_are_children(by_name):
    children = [c.name for c in by_name["split"].children]
    assert "_inner" in children
    assert "out" in children
    assert "local" not in children  # it belongs to _inner


def test_locals_do_not_leak_out_of_their_function(outline):
    offset = SOURCE.index("def _private")
    assert outline.resolve("out", offset) is None
    assert outline.resolve("LIMIT", offset) is not None


def test_parameters_are_in_scope_in_the_body(outline):
    offset = SOURCE.index("i = 0")
    assert outline.resolve("sep", offset) is not None
    assert outline.resolve("sep", offset).detail == "sep: str"


def test_a_binding_is_not_visible_before_its_declaration(outline):
    assert outline.resolve("LIMIT", SOURCE.index("type Word")) is None


def test_functions_are_visible_before_their_declaration(outline):
    # top-level names link as a set, so forward references are legal
    assert outline.resolve("split", 0) is not None


def test_imports(outline):
    plain, from_import = outline.imports
    assert (plain.kind, plain.module_path) == ("import", "strings")
    assert plain.local_module_name == "strings"
    assert from_import.module_path == "result"
    assert [(n.name, n.alias, n.local) for n in from_import.names] == [
        ("Result", None, "Result"), ("Err", "Failure", "Failure")
    ]


def test_aliased_import_binds_the_alias():
    outline = build("import text.strings as s\n")
    assert outline.imports[0].local_module_name == "s"
    assert [d.name for d in outline.symbols] == ["s"]


def test_dotted_import_binds_the_last_component():
    assert build("import a.b.c\n").imports[0].local_module_name == "c"


def test_folding_covers_multiline_blocks(outline):
    assert any(r.end.line > r.start.line for r in outline.folds)


def test_broken_input_still_yields_symbols():
    outline = build("def good() { }\ndef ")
    assert [d.name for d in outline.symbols] == ["good"]


def test_a_brace_on_the_binding_line_is_not_part_of_it():
    outline = build("def f() {\n  if c { x = 1 }\n}\n")
    binding = outline.definitions[-1]
    assert binding.detail == "x = 1"
