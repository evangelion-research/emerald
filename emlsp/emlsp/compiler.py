"""Driving `emeraldc` -- the analysis half of the architecture (DESIGN.md 3).

The rule from the design is that C analyzes and Python speaks: nothing here
reimplements a checker. This module is the subprocess seam, and today it uses
the one query mode the compiler already has -- `--check --json`
(`docs/diagnostics.md`) -- to produce diagnostics for a program.

Two things the design asks for do not exist in `emeraldc` yet, and their
absence shapes this file:

* **No `--stdin` / `--overlay`.** Unsaved buffers cannot be handed to the
  loader, so a dirty buffer is written to a temp file *in its own directory*
  before checking, which keeps `import` resolution (the importing file's
  directory is search root #1, `docs/modules.md`) behaving as it does on disk.
  When the overlay hook of DESIGN.md 2a lands, `Overlay` becomes a JSON map on
  the command line and the temp file disappears.
* **No parser error recovery.** The parser exits on the first syntax error
  (DESIGN.md 1a), so a broken buffer yields exactly one diagnostic and no type
  errors. That is the compiler's behaviour, faithfully reported, not a bug
  here.

Resolution roots follow the one-way rule: pme resolves, the LSP consumes. We
read `emerald.lock` and turn it into `-I` flags; we never solve versions.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

DEFAULT_BINARY = "emeraldc"
LOCKFILE = "emerald.lock"


class CompilerNotFound(Exception):
    """`emeraldc` is not on PATH and none was configured."""


@dataclass(slots=True)
class Settings:
    """Server configuration, from `initializationOptions` or
    `workspace/didChangeConfiguration` under the `emerald` key."""

    compiler_path: str | None = None
    include_paths: list[str] = field(default_factory=list)
    diagnostics_enabled: bool = True
    proof: bool = False  # pass --proof: promotes warnings to errors
    debounce_ms: int = 250
    timeout_s: float = 10.0

    @classmethod
    def from_object(cls, obj: object) -> "Settings":
        data = obj if isinstance(obj, dict) else {}
        emerald = data.get("emerald", data)
        if not isinstance(emerald, dict):
            emerald = {}
        diags = emerald.get("diagnostics")
        diags = diags if isinstance(diags, dict) else {}
        return cls(
            compiler_path=emerald.get("compilerPath") or None,
            include_paths=list(emerald.get("includePaths") or []),
            diagnostics_enabled=bool(diags.get("enabled", True)),
            proof=bool(emerald.get("proof", False)),
            debounce_ms=int(emerald.get("debounceMs", 250)),
            timeout_s=float(emerald.get("timeoutSeconds", 10.0)),
        )


@dataclass(slots=True)
class CheckResult:
    diagnostics: list[dict]
    """Raw diagnostic objects, exactly as `docs/diagnostics.md` documents them."""
    ok: bool
    """False when the compiler could not be run or spoke something else."""
    detail: str = ""
    """Human-readable explanation when `ok` is False."""


def find_compiler(settings: Settings) -> str:
    """Locate `emeraldc`: explicit setting, then $EMERALDC, then PATH."""
    for candidate in (settings.compiler_path, os.environ.get("EMERALDC")):
        if candidate:
            resolved = shutil.which(candidate) or (
                candidate if os.path.isfile(candidate) else None
            )
            if resolved:
                return resolved
            raise CompilerNotFound(f"configured compiler not found: {candidate}")
    found = shutil.which(DEFAULT_BINARY)
    if not found:
        raise CompilerNotFound(
            "emeraldc is not on PATH; set emerald.compilerPath or $EMERALDC"
        )
    return found


def store_root() -> Path:
    """Where pme puts fetched packages."""
    home = os.environ.get("EMERALD_HOME")
    return Path(home) / "store" if home else Path.home() / ".emerald" / "store"


def find_lockfile(start: str) -> Path | None:
    """Nearest `emerald.lock` walking up from a file's directory."""
    here = Path(start)
    if here.is_file():
        here = here.parent
    for directory in [here, *here.parents]:
        candidate = directory / LOCKFILE
        if candidate.is_file():
            return candidate
    return None


def lock_include_paths(lock: Path) -> list[str]:
    """pme's frozen `-I` rule: each locked package contributes its `src/`
    under the store, dependencies before dependents, ties broken by name.

    This is a *copy* of a rule pme owns (DESIGN.md, "Package management"). It
    is intentionally tolerant: an unreadable or unfamiliar lockfile yields no
    roots rather than an error, because the alternative is a workspace that
    reports every import as unresolvable.
    """
    try:
        data = tomllib.loads(lock.read_text(encoding="utf-8"))
    except (OSError, tomllib.TOMLDecodeError):
        return []

    packages = data.get("package")
    if not isinstance(packages, list):
        return []

    by_name: dict[str, dict] = {}
    for entry in packages:
        if isinstance(entry, dict) and isinstance(entry.get("name"), str):
            by_name[entry["name"]] = entry

    ordered: list[str] = []
    seen: set[str] = set()
    store = store_root()

    def visit(name: str, stack: frozenset[str]) -> None:
        if name in seen or name in stack:  # a cycle is pme's problem to report
            return
        entry = by_name.get(name)
        if entry is None:
            return
        deps = entry.get("dependencies") or entry.get("deps") or []
        for dep in sorted(d for d in deps if isinstance(d, str)):
            visit(dep.split()[0], stack | {name})
        seen.add(name)
        src = _package_src(entry, store, lock.parent)
        if src is not None:
            ordered.append(str(src))

    for name in sorted(by_name):
        visit(name, frozenset())
    return ordered


def _package_src(entry: dict, store: Path, lock_dir: Path) -> Path | None:
    """A locked package's `src/` -- a path dependency stays where it is."""
    path = entry.get("path")
    if isinstance(path, str):
        base = Path(path)
        return (base if base.is_absolute() else lock_dir / base) / "src"
    name, version = entry.get("name"), entry.get("version")
    if isinstance(name, str) and isinstance(version, str):
        return store / f"{name}-{version}" / "src"
    return None


def include_paths_for(path: str, settings: Settings) -> list[str]:
    """The ordered `-I` roots for analyzing `path`.

    The importing file's directory, the nearest `src/`, and the stdlib are the
    compiler's own business (`module.c` `find_src_root`); the server supplies
    only package roots and whatever the user configured.
    """
    roots = list(settings.include_paths)
    lock = find_lockfile(path)
    if lock is not None:
        roots.extend(lock_include_paths(lock))
    return list(
        dict.fromkeys(os.path.abspath(os.path.expanduser(r)) for r in roots)
    )


def _overlay_path(path: str) -> Path:
    """Where a dirty buffer is staged: beside the real file, so imports still
    resolve.

    A sibling temp file is not free -- it is briefly visible to anything
    watching the directory -- but it is the only way to analyze unsaved text
    until the loader takes an overlay map (DESIGN.md 2a). The dot prefix keeps
    it out of `*.rald` globs.
    """
    real = Path(path)
    return real.with_name(f".{real.stem}.emlsp-{os.getpid()}.rald")


def check(
    path: str,
    source: str | None,
    settings: Settings,
    *,
    compiler: str | None = None,
) -> CheckResult:
    """Run `emeraldc --check --json` over `path`.

    `source` is the editor's text; pass None when the buffer matches disk, and
    the file is checked in place with no temp file at all.
    """
    binary = compiler or find_compiler(settings)
    roots = include_paths_for(path, settings)

    def run(target: Path) -> CheckResult:
        argv = [binary, "--check", "--json"]
        for root in roots:
            argv += ["-I", root]
        if settings.proof:
            argv.append("--proof")
        argv.append(str(target))
        try:
            proc = subprocess.run(
                argv,
                capture_output=True,
                text=True,
                timeout=settings.timeout_s,
                cwd=str(Path(path).parent),
            )
        except subprocess.TimeoutExpired:
            return CheckResult([], False, f"emeraldc timed out after {settings.timeout_s}s")
        except OSError as exc:
            return CheckResult([], False, f"could not run {binary}: {exc}")
        return _parse_output(proc.stdout, proc.stderr, proc.returncode)

    if source is None:
        return run(Path(path))
    temp = _overlay_path(path)
    try:
        temp.write_text(source, encoding="utf-8")
        result = run(temp)
    finally:
        temp.unlink(missing_ok=True)
    _remap(result.diagnostics, temp.name, path)
    return result


def _parse_output(stdout: str, stderr: str, code: int) -> CheckResult:
    text = stdout.strip()
    if not text:
        # a clean file emits `[]`; genuinely empty stdout means it crashed or
        # never got as far as the checker (stderr is the crash channel, 1d)
        if code == 0:
            return CheckResult([], True)
        return CheckResult([], False, stderr.strip() or f"emeraldc exited {code}")
    try:
        data = json.loads(text)
    except json.JSONDecodeError:
        return CheckResult([], False, f"unparseable output from emeraldc: {text[:200]}")
    if not isinstance(data, list):
        return CheckResult([], False, "expected a JSON array of diagnostics")
    return CheckResult([d for d in data if isinstance(d, dict)], True)


def _remap(diagnostics: list[dict], temp_name: str, real_path: str) -> None:
    """Point diagnostics at the buffer's real file, not the temp copy."""
    for diag in diagnostics:
        file = diag.get("file")
        if isinstance(file, str) and os.path.basename(file) == temp_name:
            diag["file"] = real_path


def probe(binary: str) -> str | None:
    """`emeraldc --version`, for the startup log. None if it does not answer."""
    try:
        proc = subprocess.run(
            [binary, "--version"], capture_output=True, text=True, timeout=5
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    return (proc.stdout or proc.stderr).strip() or None
