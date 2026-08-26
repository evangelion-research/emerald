import os
import textwrap
from pathlib import Path

import pytest

from emlsp import compiler


# -- settings ------------------------------------------------------------
def test_defaults():
    settings = compiler.Settings.from_object(None)
    assert settings.diagnostics_enabled
    assert settings.compiler_path is None


def test_reads_the_emerald_key():
    settings = compiler.Settings.from_object(
        {"emerald": {"compilerPath": "/x/emeraldc", "includePaths": ["/lib"],
                     "proof": True, "debounceMs": 50,
                     "diagnostics": {"enabled": False}}}
    )
    assert settings.compiler_path == "/x/emeraldc"
    assert settings.include_paths == ["/lib"]
    assert settings.proof
    assert settings.debounce_ms == 50
    assert not settings.diagnostics_enabled


def test_accepts_settings_without_the_wrapper():
    assert compiler.Settings.from_object({"includePaths": ["/lib"]}).include_paths == ["/lib"]


# -- lockfile ------------------------------------------------------------
@pytest.fixture
def emerald_home(tmp_path, monkeypatch):
    monkeypatch.setenv("EMERALD_HOME", str(tmp_path / "home"))
    return tmp_path


@pytest.fixture
def write_lock(emerald_home):
    def write(body: str) -> Path:
        lock = emerald_home / compiler.LOCKFILE
        lock.write_text(textwrap.dedent(body))
        return lock

    return write


def test_dependencies_come_before_dependents(write_lock):
    lock = write_lock(
        """
        [[package]]
        name = "web"
        version = "1.0.0"
        dependencies = ["http"]

        [[package]]
        name = "http"
        version = "0.2.0"
        """
    )
    roots = compiler.lock_include_paths(lock)
    assert [Path(r).parent.name for r in roots] == ["http-0.2.0", "web-1.0.0"]
    assert all(r.endswith("src") for r in roots)


def test_path_dependencies_stay_where_they_are(write_lock, emerald_home):
    lock = write_lock(
        """
        [[package]]
        name = "local"
        version = "0.1.0"
        path = "vendor/local"
        """
    )
    assert compiler.lock_include_paths(lock) == [str(emerald_home / "vendor/local/src")]


def test_a_cycle_does_not_hang(write_lock):
    lock = write_lock(
        """
        [[package]]
        name = "a"
        version = "1.0.0"
        dependencies = ["b"]

        [[package]]
        name = "b"
        version = "1.0.0"
        dependencies = ["a"]
        """
    )
    assert len(compiler.lock_include_paths(lock)) == 2


def test_an_unreadable_lockfile_yields_no_roots(write_lock):
    assert compiler.lock_include_paths(write_lock("this is not toml [[[")) == []


def test_find_lockfile_walks_up(write_lock, emerald_home):
    nested = emerald_home / "src" / "deep"
    nested.mkdir(parents=True)
    lock = write_lock("[[package]]\nname='a'\nversion='1.0.0'\n")
    assert compiler.find_lockfile(str(nested / "f.rald")) == lock


def test_include_paths_deduplicate_and_expand(emerald_home):
    settings = compiler.Settings(include_paths=["~/x", "~/x"])
    roots = compiler.include_paths_for(str(emerald_home / "f.rald"), settings)
    assert roots == [os.path.expanduser("~/x")]


# -- check ---------------------------------------------------------------
@pytest.fixture
def project(tmp_path, fake_compiler):
    """A directory holding the fake compiler and one source file."""
    source = tmp_path / "main.rald"
    source.write_text("print(1)\n")
    return tmp_path, source, compiler.Settings(compiler_path=fake_compiler)


def test_a_clean_file_has_no_diagnostics(project):
    _, source, settings = project
    source.write_text("clean\n")
    result = compiler.check(str(source), None, settings)
    assert result.ok
    assert result.diagnostics == []


def test_diagnostics_are_returned_verbatim(project):
    _, source, settings = project
    result = compiler.check(str(source), None, settings)
    assert result.diagnostics[0]["code"] == "E_TYPE_ARG"


def test_an_unsaved_buffer_is_checked_and_reported_against_the_real_file(project):
    _, source, settings = project
    result = compiler.check(str(source), "dirty buffer\n", settings)
    assert result.diagnostics[0]["file"] == str(source)
    assert result.diagnostics[0]["source_line"] == "dirty buffer"


def test_the_overlay_file_is_cleaned_up(project):
    directory, source, settings = project
    compiler.check(str(source), "dirty\n", settings)
    assert [p.name for p in directory.glob(".*emlsp*")] == [], "temp overlay leaked"


def test_the_overlay_sits_beside_the_real_file_so_imports_resolve(project):
    _, source, _ = project
    temp = compiler._overlay_path(str(source))
    assert temp.parent == source.parent
    assert temp.name.startswith(".")


def test_include_paths_are_passed_as_I_flags(project, fake_compiler):
    _, source, _ = project
    settings = compiler.Settings(compiler_path=fake_compiler, include_paths=["/lib"])
    result = compiler.check(str(source), None, settings)
    assert result.diagnostics[0]["message"] == "roots=/lib"


def test_a_missing_compiler_is_reported_not_raised(tmp_path):
    settings = compiler.Settings(compiler_path=str(tmp_path / "nope"))
    with pytest.raises(compiler.CompilerNotFound):
        compiler.find_compiler(settings)


def test_garbage_on_stdout_is_not_fatal(project):
    directory, source, _ = project
    broken = directory / "broken"
    broken.write_text("#!/bin/sh\necho not json\n")
    broken.chmod(0o755)
    result = compiler.check(
        str(source), None, compiler.Settings(compiler_path=str(broken))
    )
    assert not result.ok
    assert "unparseable" in result.detail


def test_probe_reports_the_version(fake_compiler):
    assert compiler.probe(fake_compiler) == "emeraldc 0.0-fake"
