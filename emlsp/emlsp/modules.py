"""Following an `import` to a file, and reading what a module exports.

This mirrors the resolution order in `docs/modules.md` -- importing file's
directory, the nearest `src/`, each `-I` root, then the stdlib -- for the two
features that need it before `--lsp-index` exists: goto-definition on an import
and import completion (DESIGN.md 2d).

The compiler stays the authority. If the two ever disagree, the diagnostic
`emeraldc` produces is the truth and this is the bug.
"""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path

from .outline import Outline, build

SUFFIX = ".rald"


@dataclass(slots=True, frozen=True)
class Resolved:
    path: str
    root: str
    ambiguous: bool  # both spellings under one root -- E_IMPORT_AMBIGUOUS


def stdlib_root(compiler: str | None) -> Path | None:
    """`$EMERALD_STDLIB`, else the `stdlib/` shipped beside the binary."""
    env = os.environ.get("EMERALD_STDLIB")
    if env:
        return Path(env)
    if compiler:
        exe = Path(compiler).resolve().parent
        for candidate in (exe / "stdlib", exe.parent / "stdlib"):
            if candidate.is_dir():
                return candidate
    return None


def src_root(start: str) -> Path | None:
    """The nearest `src/` walking up from a file (`module.c` find_src_root)."""
    here = Path(start)
    if here.is_file():
        here = here.parent
    for directory in [here, *here.parents]:
        if directory.name == "src":
            return directory
        candidate = directory / "src"
        if candidate.is_dir():
            return candidate
    return None


def roots_for(
    importer: str, include_paths: list[str], compiler: str | None
) -> list[Path]:
    """The ordered search roots for imports in `importer`."""
    roots: list[Path] = [Path(importer).parent]
    src = src_root(importer)
    if src is not None:
        roots.append(src)
    roots.extend(Path(p) for p in include_paths)
    std = stdlib_root(compiler)
    if std is not None:
        roots.append(std)
    by_key: dict[str, Path] = {}
    for root in roots:
        by_key.setdefault(str(root.resolve()) if root.exists() else str(root), root)
    return list(by_key.values())


def resolve(
    module_path: str, importer: str, include_paths: list[str], compiler: str | None
) -> Resolved | None:
    """`text.strings` -> `text/strings.rald` or `text.strings.rald`."""
    nested = Path(*module_path.split(".")).with_suffix(SUFFIX)
    flat = Path(module_path + SUFFIX)
    # for a single-component path the two spellings are the same file, so only
    # a dotted path can be E_IMPORT_AMBIGUOUS
    spellings = [nested] if nested == flat else [nested, flat]
    for root in roots_for(importer, include_paths, compiler):
        hits = [root / s for s in spellings]
        found = [h for h in hits if h.is_file()]
        if found:
            return Resolved(str(found[0]), str(root), ambiguous=len(found) > 1)
    return None


def module_candidates(
    importer: str, include_paths: list[str], compiler: str | None
) -> list[str]:
    """Dotted module paths importable from `importer`, for completion.

    Only one directory level is descended: enough for `text.strings`, and it
    keeps a large workspace from turning completion into a filesystem walk.
    """
    return sorted(
        {
            name
            for root in roots_for(importer, include_paths, compiler)
            for name in _list_modules(root)
        }
    )


def _list_modules(root: Path, depth: int = 2) -> list[str]:
    out: list[str] = []
    try:
        entries = sorted(root.iterdir())
    except OSError:
        return out
    for entry in entries:
        if entry.name.startswith((".", "_")):
            continue
        if entry.is_file() and entry.suffix == SUFFIX:
            out.append(entry.stem)
        elif entry.is_dir() and depth > 1:
            out.extend(f"{entry.name}.{child}" for child in _list_modules(entry, depth - 1))
    return out


def read_outline(path: str) -> Outline | None:
    """The outline of a module on disk. Unsaved buffers are the server's job
    to substitute before calling this."""
    try:
        return build(Path(path).read_text(encoding="utf-8"))
    except OSError:
        return None
