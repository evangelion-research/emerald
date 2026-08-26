from emlsp.lexer import tokenize
from emlsp.semantic import LEGEND, TOKEN_MODIFIERS, TOKEN_TYPES, classify, encode


def types_of(src, modules=frozenset()):
    toks = tokenize(src)
    return [
        (src.splitlines()[line][col : col + length], TOKEN_TYPES[t], _mods(m))
        for line, col, length, t, m in classify(toks, src.splitlines(), modules)
    ]


def _mods(bits):
    return tuple(name for i, name in enumerate(TOKEN_MODIFIERS) if bits & (1 << i))


def test_declaration_positions():
    assert types_of("def f(x) { }")[:3] == [
        ("def", "keyword", ()),
        ("f", "function", ("declaration", "definition")),
        ("(", "operator", ()),
    ]


def test_builtins_are_marked_default_library():
    assert ("len", "function", ("defaultLibrary",)) in types_of("len(xs)")


def test_type_atoms():
    assert ("int", "type", ()) in types_of("x: int = 1")


def test_module_bindings_are_namespaces():
    got = types_of("strings.split(s)", frozenset({"strings"}))
    assert got[0] == ("strings", "namespace", ())
    assert got[2] == ("split", "function", ())


def test_field_access_on_a_value_is_a_property():
    got = types_of("point.x", frozenset())
    assert got[2] == ("x", "property", ())


def test_const_is_readonly():
    assert ("LIMIT", "variable", ("declaration", "readonly")) in types_of("const LIMIT = 1")


def test_error_tokens_are_skipped():
    assert [t for t in types_of("$") if t[1] != "operator"] == []


def test_multiline_strings_are_split_per_line():
    spans = classify(tokenize('x = "a\nb"'), 'x = "a\nb"'.splitlines())
    string_spans = [s for s in spans if TOKEN_TYPES[s[3]] == "string"]
    assert len(string_spans) == 2
    assert [s[0] for s in string_spans] == [0, 1]


def test_encoding_is_relative():
    data = encode(tokenize("a\nb"), ["a", "b"])
    assert len(data) % 5 == 0
    assert data[0] == 0  # first token, no line delta
    assert data[5] == 1  # second token, one line later


def test_legend_indices_are_stable():
    assert LEGEND.token_types == TOKEN_TYPES
    assert LEGEND.token_modifiers == TOKEN_MODIFIERS
