import pytest

from emlsp.positions import byte_col_to_char_col, path_to_uri, uri_to_path


@pytest.mark.parametrize("byte_col, expected", [(1, 0), (3, 2)])
def test_ascii_is_the_identity_modulo_the_base(byte_col, expected):
    assert byte_col_to_char_col("abc", byte_col) == expected


def test_non_ascii_shifts_every_later_column():
    line = 'print("héllo", x)'
    # "x" is at byte column 17 but character column 15
    byte_col = line.encode().index(b"x") + 1
    assert byte_col_to_char_col(line, byte_col) == line.index("x")


def test_emoji_outside_the_bmp():
    line = 'x = "🙂" + y'
    byte_col = line.encode().index(b"+") + 1
    assert byte_col_to_char_col(line, byte_col) == line.index("+")


@pytest.mark.parametrize("char_col", range(len("é🙂 abc")))
def test_every_character_boundary_maps_back(char_col):
    line = "é🙂 abc"
    byte_col = len(line[:char_col].encode("utf-8")) + 1
    assert byte_col_to_char_col(line, byte_col) == char_col


def test_past_the_end_clamps():
    assert byte_col_to_char_col("ab", 99) == 2


def test_uri_round_trip():
    uri = path_to_uri("/tmp/a b/f.rald")
    assert uri.startswith("file://")
    assert uri_to_path(uri).endswith("/a b/f.rald")


def test_non_file_scheme_is_not_a_path():
    assert uri_to_path("untitled:Untitled-1") is None
