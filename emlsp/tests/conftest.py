"""Shared fixtures.

The fake compiler lives here because both the compiler unit tests and the
end-to-end session tests need a binary that behaves like `emeraldc` without
one being installed.
"""

from __future__ import annotations

from pathlib import Path

import pytest

# A stand-in for emeraldc: reports one error naming the file it was given, so
# tests can tell the overlay copy from the real file.
FAKE = """\
#!/usr/bin/env python3
import json, sys
args = sys.argv[1:]
if "--version" in args:
    print("emeraldc 0.0-fake"); raise SystemExit(0)
target = [a for a in args if a.endswith(".rald")][-1]
roots = [args[i + 1] for i, a in enumerate(args) if a == "-I"]
text = open(target).read()
if "clean" in text:
    print("[]"); raise SystemExit(0)
print(json.dumps([{ "kind": "type", "severity": "error", "code": "E_TYPE_ARG",
    "file": target, "line": 1, "column": 1, "message": "roots=" + ",".join(roots),
    "source_line": text.splitlines()[0] if text else ""}]))
"""


def write_fake(directory: Path) -> str:
    path = directory / "fake-emeraldc"
    path.write_text(FAKE)
    path.chmod(0o755)
    return str(path)


@pytest.fixture
def fake_compiler(tmp_path: Path) -> str:
    return write_fake(tmp_path)
