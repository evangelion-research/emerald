"""Truncation fuzz (DESIGN.md 8).

Cut each sample at every character offset and assert the lexical layer
terminates and still produces *some* answer. An editor buffer is a truncated
program between every pair of keystrokes, so this is the cheapest test that
catches the majority of recovery bugs -- and it is the test the compiler's own
parser cannot pass yet (DESIGN.md 1a), which is exactly why the syntax-only
features do not go through it.
"""

import pytest

from emlsp import features, semantic
from emlsp.lexer import tokenize
from emlsp.outline import build

SAMPLES = [
    """\
import strings
from result import Result, Err as Failure

type Word = str
error NotFound { key: str }
dim Batch, Seq

def split(s: str, sep: str) -> list[str] pure {
    out: list[str] = []
    match s {
        "" -> { return out }
        _ -> { append(out, s) }
    }
    return out
}
""",
    'x = f"interpolated {name} text"\ny = try parse(x) |> strings.upper >> print\n',
    "def f[T: dim](t: Tensor[f32, [T]]) -> Fin[T] partial { return refl }\n",
    "const emoji = \"🙂 é ünïcode\"\nprint(emoji)\n",
]

# Each sample is one test case; the per-offset cuts stay an inner loop so the
# report does not drown in thousands of ids.
sample = pytest.mark.parametrize("sample", SAMPLES, ids=lambda s: s[:20])


@sample
def test_every_prefix_lexes_and_outlines(sample):
    for cut in range(len(sample) + 1):
        prefix = sample[:cut]
        tokens = tokenize(prefix)
        outline = build(prefix)
        semantic.encode(tokens, prefix.splitlines())
        features.document_symbols(outline)
        features.folding_ranges(outline)
        assert all(t.end_col >= 0 for t in tokens), f"cut={cut}"


@sample
def test_token_spans_stay_inside_the_document(sample):
    lines = sample.splitlines()
    for token in tokenize(sample):
        assert token.line < len(lines) + 1
        assert sample[token.offset : token.offset + len(token.value)] == token.value


@sample
def test_every_prefix_answers_hover_and_completion_at_its_end(sample):
    for cut in range(0, len(sample) + 1, 7):  # every offset is overkill here
        prefix = sample[:cut]
        lines = prefix.splitlines() or [""]
        ctx = features.Context(
            path="/tmp/fuzz.rald",
            source=prefix,
            outline=build(prefix),
            include_paths=[],
            compiler=None,
        )
        position = features.types.Position(
            line=len(lines) - 1, character=len(lines[-1])
        )
        features.hover(ctx, position)
        features.completions(ctx, position)
        features.definition(ctx, position)
