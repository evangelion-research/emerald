import pytest
from lsprotocol import types

from emlsp import features
from emlsp.outline import build

MAIN = """\
import strings
from result import Result

const LIMIT = 3

def greet(name: str) -> str {
    prefix = "hi "
    return prefix + strings.upper(name)
}
"""

STRINGS = """\
def upper(s: str) -> str pure { return s }
def _hidden(s: str) -> str { return s }
type Word = str
"""

RESULT = "type Result[T, E] = { ok: bool }\n"


def position(line, character):
    return types.Position(line=line, character=character)


def line_of(text, needle):
    """The index of the line of `text` that equals `needle`."""
    return text.splitlines().index(needle)


@pytest.fixture
def workspace(tmp_path):
    (tmp_path / "strings.rald").write_text(STRINGS)
    (tmp_path / "result.rald").write_text(RESULT)
    main = tmp_path / "main.rald"
    main.write_text(MAIN)
    return tmp_path


@pytest.fixture
def make_context(workspace):
    def make(source):
        return features.Context(
            path=str(workspace / "main.rald"),
            source=source,
            outline=build(source),
            include_paths=[],
            compiler=None,
        )

    return make


@pytest.fixture
def ctx(make_context):
    return make_context(MAIN)


class TestHover:
    def test_hover_on_a_local_shows_its_declaration(self, ctx):
        line = line_of(MAIN, '    prefix = "hi "')
        hover = features.hover(ctx, position(line, 5))
        assert 'prefix = "hi "' in hover.contents.value

    def test_hover_on_a_module_binding_shows_the_file_it_resolved_to(self, ctx, workspace):
        hover = features.hover(ctx, position(0, 8))
        assert str(workspace / "strings.rald") in hover.contents.value

    def test_hover_on_a_qualified_name_reads_the_other_module(self, ctx):
        line = line_of(MAIN, "    return prefix + strings.upper(name)")
        column = MAIN.splitlines()[line].index("upper")
        hover = features.hover(ctx, position(line, column))
        assert "def upper(s: str) -> str pure" in hover.contents.value

    def test_hover_on_a_builtin(self, make_context):
        ctx = make_context("len(xs)\n")
        assert "builtin" in features.hover(ctx, position(0, 1)).contents.value

    def test_hover_on_a_keyword(self, make_context):
        ctx = make_context("const x = 1\n")
        assert "keyword" in features.hover(ctx, position(0, 2)).contents.value

    def test_hover_on_punctuation_is_nothing(self, ctx):
        assert features.hover(ctx, position(3, 12)) is None


class TestDefinition:
    def test_local_definition(self, ctx):
        line = line_of(MAIN, "    return prefix + strings.upper(name)")
        found = features.definition(ctx, position(line, 12))
        assert len(found) == 1
        assert found[0].uri.endswith("main.rald")

    def test_definition_across_a_module_boundary(self, ctx):
        line = line_of(MAIN, "    return prefix + strings.upper(name)")
        column = MAIN.splitlines()[line].index("upper")
        found = features.definition(ctx, position(line, column))
        assert found[0].uri.endswith("strings.rald")
        assert found[0].range.start.line == 0

    def test_definition_on_an_import_path_opens_the_module(self, ctx):
        found = features.definition(ctx, position(0, 8))
        assert found[0].uri.endswith("strings.rald")

    def test_definition_on_a_lifted_name(self, ctx):
        found = features.definition(ctx, position(1, 20))
        assert found[0].uri.endswith("result.rald")

    def test_an_unknown_name_has_no_definition(self, make_context):
        ctx = make_context("nowhere\n")
        assert features.definition(ctx, position(0, 2)) == []


class TestCompletion:
    @staticmethod
    def labels(ctx, pos):
        return [item.label for item in features.completions(ctx, pos).items]

    @pytest.mark.parametrize(
        "expected", ["name", "LIMIT", "greet", "const", "len", "seq"]
    )
    def test_scope_completion_offers_locals_keywords_and_builtins(self, ctx, expected):
        line = line_of(MAIN, '    prefix = "hi "')
        assert expected in self.labels(ctx, position(line, 4))

    def test_completion_after_a_module_binding_lists_its_exports(self, make_context):
        source = MAIN.replace("strings.upper(name)", "strings.")
        ctx = make_context(source)
        line = line_of(source, "    return prefix + strings.")
        labels = self.labels(ctx, position(line, len("    return prefix + strings.")))
        assert "upper" in labels
        assert "Word" in labels
        assert "_hidden" not in labels  # module.c is_private

    def test_completion_after_a_value_stays_silent(self, make_context):
        # a field list needs the checker's type for the receiver (DESIGN.md 4b)
        ctx = make_context("def f(p) { p. }\n")
        result = features.completions(ctx, position(0, 13))
        assert result.items == []
        assert result.is_incomplete

    def test_import_completion_lists_module_paths(self, make_context):
        ctx = make_context("import ")
        assert "strings" in self.labels(ctx, position(0, 7))

    def test_from_import_completion_lists_the_modules_exports(self, make_context):
        ctx = make_context("from strings import ")
        assert sorted(self.labels(ctx, position(0, 20))) == ["Word", "upper"]


class TestSymbolsAndReferences:
    def test_document_symbols_nest_and_deduplicate(self):
        source = "def f() {\n  i = 0\n  i = i + 1\n}\n"
        symbols = features.document_symbols(build(source))
        assert [s.name for s in symbols] == ["f"]
        assert [c.name for c in symbols[0].children] == ["i"]

    def test_references_are_name_matches_in_this_file(self, ctx):
        line = line_of(MAIN, '    prefix = "hi "')
        assert len(features.references(ctx, position(line, 5))) == 2

    def test_folding_ranges_end_before_the_closing_brace(self):
        source = "def f() {\n  x = 1\n}\n"
        fold = features.folding_ranges(build(source))[0]
        assert (fold.start_line, fold.end_line) == (0, 1)
