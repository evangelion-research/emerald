"""End-to-end: the real server process, driven over stdio.

These are the "session tests" of DESIGN.md 8. They are the only tests that
exercise framing, the lifecycle, the handler signatures, the debounce, and the
UTF-16 conversion together -- everything that can be perfectly right in
isolation and still leave a client seeing nothing.
"""

import pytest

from tests.session import Session

MAIN = """\
import strings

def greet(name: str) -> str {
    return strings.upper(name)
}
"""

STRINGS = "def upper(s: str) -> str pure { return s }\n"


@pytest.fixture
def workspace(tmp_path):
    (tmp_path / "strings.rald").write_text(STRINGS)
    (tmp_path / "main.rald").write_text(MAIN)
    return tmp_path


@pytest.fixture
def main(workspace):
    return workspace / "main.rald"


@pytest.fixture
def session(workspace, fake_compiler, monkeypatch):
    """Open a session against `workspace`, optionally with the fake compiler."""

    def start(with_compiler: bool = False) -> Session:
        if with_compiler:
            monkeypatch.setenv("EMERALDC", fake_compiler)
        else:
            monkeypatch.delenv("EMERALDC", raising=False)
        return Session(workspace)

    return start


def test_syntax_features_work_without_a_compiler(session, main):
    with session() as client:
        uri = client.open(main)

        symbols = client.request("textDocument/documentSymbol", {"textDocument": {"uri": uri}})
        assert [s["name"] for s in symbols] == ["strings", "greet"]

        tokens = client.request("textDocument/semanticTokens/full", {"textDocument": {"uri": uri}})
        assert len(tokens["data"]) % 5 == 0
        assert len(tokens["data"]) > 0

        hover = client.request(
            "textDocument/hover",
            {"textDocument": {"uri": uri}, "position": {"line": 0, "character": 8}},
        )
        assert "strings.rald" in hover["contents"]["value"]

        definition = client.request(
            "textDocument/definition",
            {"textDocument": {"uri": uri}, "position": {"line": 3, "character": 21}},
        )
        assert definition[0]["uri"].endswith("strings.rald")


def test_completion_and_workspace_symbols(session, main):
    with session() as client:
        uri = client.open(main)
        completion = client.request(
            "textDocument/completion",
            {"textDocument": {"uri": uri}, "position": {"line": 3, "character": 4}},
        )
        assert "greet" in [i["label"] for i in completion["items"]]

        found = client.request("workspace/symbol", {"query": "upper"})
        assert [s["name"] for s in found] == ["upper"]


def test_diagnostics_are_published_for_an_open_document(session, main):
    with session(with_compiler=True) as client:
        client.open(main)
        params = client.wait_for("textDocument/publishDiagnostics")
        assert len(params["diagnostics"]) == 1
        assert params["diagnostics"][0]["code"] == "E_TYPE_ARG"
        assert params["diagnostics"][0]["source"] == "emeraldc"


def test_unused_diagnostics_work_without_a_compiler(session, workspace):
    path = workspace / "unused.rald"
    path.write_text("import strings\n\ndef main() {\n    value = 1\n}\n")
    with session() as client:
        client.open(path)
        params = client.wait_for("textDocument/publishDiagnostics")
        assert [d["code"] for d in params["diagnostics"]] == ["E_UNUSED", "E_UNUSED"]
        assert [d["message"] for d in params["diagnostics"]] == [
            'imported and not used: "strings"',
            "declared and not used: value",
        ]
        assert all(d["severity"] == 1 for d in params["diagnostics"])


def test_editing_re_checks_the_unsaved_buffer(session, main, workspace):
    with session(with_compiler=True) as client:
        uri = client.open(main)
        client.wait_for("textDocument/publishDiagnostics")
        client.notify(
            "textDocument/didChange",
            {
                "textDocument": {"uri": uri, "version": 2},
                "contentChanges": [{"text": "clean\n"}],
            },
        )
        params = client.wait_for("textDocument/publishDiagnostics")
        assert params["diagnostics"] == []
        # the temp overlay must not survive the request
        assert list(workspace.glob(".*emlsp*")) == []


def test_positions_on_the_wire_are_utf16(session, workspace):
    source = 'x = "🙂"\nprint(x)\n'
    path = workspace / "emoji.rald"
    path.write_text(source)
    with session() as client:
        uri = client.open(path, source)
        tokens = client.request(
            "textDocument/semanticTokens/full", {"textDocument": {"uri": uri}}
        )
        # line 0: `x`(1) `=`(1) then the string, which is 4 UTF-16 units
        # ("🙂" is a surrogate pair) even though it is 3 characters
        lengths = tokens["data"][2::5]
        assert lengths[:3] == [1, 1, 4]


def test_initialization_options_configure_the_compiler(
    workspace, main, fake_compiler, monkeypatch
):
    monkeypatch.delenv("EMERALDC", raising=False)
    client = Session(workspace)
    try:
        client.request(
            "initialize",
            {
                "processId": None,
                "rootUri": workspace.as_uri(),
                "capabilities": {},
                "initializationOptions": {
                    "emerald": {"compilerPath": fake_compiler, "debounceMs": 10}
                },
            },
        )
        client.notify("initialized", {})
        client.open(main)
        params = client.wait_for("textDocument/publishDiagnostics")
        assert len(params["diagnostics"]) == 1
    finally:
        client.__exit__()


def test_closing_a_document_clears_its_diagnostics(session, main):
    with session(with_compiler=True) as client:
        uri = client.open(main)
        client.wait_for("textDocument/publishDiagnostics")
        client.notify("textDocument/didClose", {"textDocument": {"uri": uri}})
        params = client.wait_for("textDocument/publishDiagnostics")
        assert params["diagnostics"] == []
