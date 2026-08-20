# Emerald v1 — remaining holes

This is a deliberately adversarial release audit, not a feature wishlist.The main suite is green, but a green suite only proves the cases it contains.
This document is part of the maintained v1 release documentation; it is not a
second language specification. The
items below are things a first user, package maintainer, or fuzz tester can still
run into after the language features in [`RELEASE_V1.md`](RELEASE_V1.md) are
considered complete.

The recommendations are ordered by release risk. A finding marked **confirmed**
is visible directly in the current implementation. A finding marked **stress**
needs a regression test or sanitizer run before it can be called closed.

## Short version

Before tagging, I would do these five things:

1. Make the installed and archived compiler self-contained enough to compile a
   program outside the source checkout.
2. Add the CI that the release documents claim exists, and make it exercise the
   actual release tasks rather than only a local build.
3. Make every compiler stage return reliable status codes, especially
   `--emit-tokens` and `--emit-ast`, and define what `--json` covers.
4. Remove shell quoting and temporary-directory hazards from the compiler and
   REPL boundary.
5. Run sanitizer, fuzz, and large-value tests against the runtime; several
   numeric and dynamic-value paths currently rely on C behavior that is less
   defined than Emerald's documented behavior.

Everything else can reasonably be v1.1 if these boundaries are explicit.

---

## P0 — release blockers or serious trust-boundary problems

### 1. The release artifact cannot compile a program on a clean machine

**Confirmed.** `Taskfile.yml`'s `install` task copies only `bin/emeraldc` and
`stdlib/*.rald`. The `dist` task copies the compiler, stdlib, README, and
changelog, but not `src/runtime.c` or `include/`. Yet `src/main.c` invokes the
system C compiler with a runtime source path (`$EMERALD_SRC` or the baked
`EMERALD_SRC_DIR`) for every generated program. The baked path is the build
machine's checkout, not a path that exists in an installed prefix or release
archive.

This means `task dist` is described as relocatable but is not a complete compiler
distribution. `task install` has the same problem unless the user happens to
have the original source checkout or manually sets `EMERALD_SRC`.

**Implementation:** package `runtime.c` and the required headers, then resolve
the runtime source relative to the executable using the same strategy already
used for the stdlib. Alternatively, compile/link a reusable runtime library,
but do not make the normal user depend on the developer checkout.

**Acceptance test:** build the archive, extract it into a fresh temporary
folder, invoke `bin/emeraldc` from a different working directory, compile a
program importing a stdlib module, and run it. Repeat through an installed
`PREFIX/bin/emeraldc` found via `PATH`, not an absolute `argv[0]` path. The test
must pass with `EMERALD_SRC` and `EMERALD_STDLIB` unset.

There is a related path bug: `find_exe_stdlib()` calls `realpath(argv0, ...)`
directly. If the compiler is invoked as `emeraldc` through `PATH`, `argv[0]` may
not contain a slash and `realpath` does not perform a `PATH` search. Resolve the
executable with `PATH` lookup or use `/proc/self/exe`/the platform equivalent
before deriving relative resources.

### 2. CI is absent

**Confirmed.** There is no `.github/workflows/` workflow in the repository. The
local `task test` is useful, but it is not a substitute for a clean checkout on
two platforms. If v1 claims CI support, add the workflow before tagging; otherwise
keep CI explicitly outside the release guarantee.

**Implementation:** add one small matrix workflow that:

- checks out the repository;
- installs go-task and a C compiler;
- runs `task --force test`;
- runs `task examples`;
- runs the format check on the files covered by the repository's formatting
  policy; and
- performs the clean archive/install smoke test from item 1.

Either land that workflow or keep CI explicitly listed as a release follow-up.
A release document should not report an infrastructure guarantee that the tree
cannot reproduce.

### 3. Early compiler stages can report success after failure

**Confirmed.** In `src/main.c`, the `MODE_TOKENS` and `MODE_AST` branches return
zero immediately. Token emission stops on a lexer error but does not turn the
error token into a nonzero process status. AST emission parses with a diagnostic
list but does not render the diagnostics or return the error count.

This is particularly damaging to editor scripts and build pipelines: a malformed
file can appear valid when only the lexer or parser stage is requested.

**Implementation:** give each stage the same contract as `--check`: render
human diagnostics to stderr (or JSON to stdout), and return nonzero if the
stage produced an error. Add malformed-input goldens for:

- unterminated strings and invalid escapes under `--emit-tokens`;
- truncated function/record syntax under `--emit-ast`; and
- the corresponding `--json` behavior, if JSON is advertised for those modes.

Also decide whether the token stream itself should be emitted before the final
error. Either behavior is fine; the exit status is not optional.

### 4. The compiler and REPL build commands pass user-controlled text to a shell

**Confirmed.** `src/main.c` constructs a shell command for `system()` and puts
paths in single quotes without escaping embedded single quotes. `src/repl.c`
does the same for the compiler path, scratch paths, and `-I` roots. A source file
or output path containing a quote can break the command; in an environment where
those paths are attacker-controlled, this is command injection.

`CC` is intentionally a command override, and Emerald's `run()` is intentionally
shell-oriented, so those do not need to disappear. The compiler's own file,
include, output, and temporary paths should not accidentally become shell
programs.

**Implementation:** prefer `fork`/`exec` with an argument vector for the C
compiler and child compiler invocations. If a shell is retained for portability,
centralize a correct shell-quoting routine and test paths containing spaces,
quotes, `$`, backticks, semicolons, and newlines. Treat `-I` the same way.

The REPL also creates `/tmp/emerald-repl-<pid>` with `mkdir -p`, which is
predictable and is not removed on normal exit. Use `mkdtemp`, register cleanup,
and avoid following pre-existing paths. Add a test that interrupts or exits a
REPL and verifies that scratch files do not remain.

---

## P1 — correctness holes users can hit

### 5. Documented integer wrapping is not implemented in defined C

**Confirmed by inspection; needs sanitizer/compiler testing.** README promises
64-bit two's-complement wrapping, but `src/runtime.c` performs signed C
arithmetic directly in `em_add`, `em_sub`, `em_mul`, `em_pow`, `em_neg`, floor
division, and shifts. Signed overflow is undefined behavior in C, not wrapping.
The left shift of a signed value and right shift of a negative value have
additional portability problems. `INT64_MIN / -1`, `INT64_MIN % -1`, and
`-INT64_MIN` are especially dangerous edge cases.

**Implementation:** perform integer operations through `uint64_t` where wrapping
is the language rule, then convert back with an explicit documented convention.
Handle exceptional cases such as division by zero and `INT64_MIN / -1` before
performing the C operation. Define shift behavior for negative values and
counts outside `0..63`.

Add boundary tests for every operator and run them under UBSan with optimization
levels different from `-O2`. A language cannot promise wraparound while leaving
its implementation to compiler-dependent undefined behavior.

### 6. Large integers are compared through `double`

**Confirmed.** `value_eq()` and `value_cmp()` first classify all numbers as
numeric and then use `as_double()`. Distinct integers above the exact IEEE-754
range can therefore compare equal, and ordering can collapse adjacent values.
For example, `9007199254740992` and `9007199254740993` cannot both be represented
exactly as doubles.

**Implementation:** use integer comparison when both operands are integers;
only promote to double when at least one operand is a float. Decide and document
how mixed int/float equality behaves for values outside the exact float range.
Add equality, ordering, modulo, and dictionary/set membership tests around
`2^53` and `INT64_MAX`.

### 7. NUL bytes are not representable consistently in strings

**Confirmed.** `chr()` advertises the range `0..255`, but short strings are
stored inline and `str_len()` uses `strlen()` for inline values. `chr(0)` stores a
NUL byte and then reports length zero. Other code also uses C string functions
such as `strstr()` for substring membership, so embedded NULs do not behave like
ordinary bytes even though the language says strings are byte-oriented.

**Implementation options:** add an explicit length to the inline string payload
(and replace C-string assumptions with length-aware operations), or explicitly
remove NUL from the string contract and make `chr(0)` a runtime error. The
first option is more coherent with `read_file()` and the stated byte semantics.
Add tests for `chr(0)`, embedded NUL concatenation, slicing, equality,
`in`, printing, and file round trips.

### 8. Dynamic values can reach tensor builtins without safe runtime checks

**Confirmed by code shape; needs a runtime regression test.** The checker uses
`any` as a dynamic escape hatch. Several tensor entry points in `runtime.c`
then immediately access `tv.as.o` or tensor fields without first checking
`is_tensor()`. A dynamically typed call such as a tensor builtin receiving an
`int`, `None`, or an arbitrary record can dereference an invalid object instead
of producing an Emerald runtime error.

The same audit should cover every builtin whose static checker accepts `any`,
not just tensors. The rule should be simple: dynamic misuse is a located
`rt_fatal`, never a segfault.

**Acceptance test:** create a small program that passes each primitive value and
an ordinary record through a dynamically typed wrapper around `exp`, `item`,
`shape`, `matmul`, `reshape`, `send`, and collection operations. Assert nonzero
exit plus a useful runtime diagnostic, and run the matrix under ASan.

### 9. Numeric conversion and allocation sizes lack complete bounds checks

There are several related edges:

- `int(float)` casts an out-of-range double to `int64_t`;
- `strtoll`/`strtod` conversion errors and range errors are not consistently
  checked through `errno`;
- `range()` computes `b - a` before converting to `size_t`;
- string/list repetition multiplies lengths by a user-provided count;
- tensor element counts multiply dimensions in `t_numel_of()`;
- tensor byte allocation multiplies element count by dtype size.

These can become negative-to-huge allocations, overflow, or undefined casts.

**Implementation:** add checked arithmetic helpers for element counts and byte
sizes, impose a deliberate maximum allocation policy, and produce a runtime
error before allocating. Test empty, maximum, negative, and overflowing ranges,
repetition, conversions, and tensor shapes.

### 10. File and process I/O does not consistently report operating-system errors

`em_read_file`, `em_write_file`, and `em_append_file` do not check every
`fseek`, `ftell`, `fread`, `fwrite`, or `fclose` result. `em_run()` returns the
raw `system()` result, which on POSIX is a wait-status encoding rather than the
child's numeric exit code. A program asking for the exit status can therefore
observe `256` for a process that exited with `1`.

**Implementation:** centralize checked file helpers, preserve `errno` in the
message or `Result` API, and decode process status into a documented result
(normal exit code, signal, or a separate failure value). Add tests for missing,
empty, unreadable, short-read, failed-write, and nonzero-command cases.

### 11. Recursive values can hang or overflow the printer and equality operator

Lists and records are mutable and can be made cyclic (`xs = []; append(xs,
xs)`). `write_value()` and `value_eq()` recursively traverse containers without
a visited set or depth limit. The GC handles cycles, but printing or comparing
one can recurse forever or exhaust the C stack.

**Implementation:** choose Python-like cycle markers, identity-based cycle
handling, or a bounded runtime error. Apply the same policy to `str`, `pprint`,
`==`, and `!=`. Add a tiny end-to-end cycle test; it should terminate with a
stable result or a located runtime error.

### 12. Default and keyword arguments need one fully specified calling model

The current implementation handles direct top-level calls and positional
closure calls, but the tests only exercise direct keyword calls plus a simple
closure call. The code generator maps keywords in its direct-call path, while
indirect calls go through `em_call()` with positional arguments. The behavior
of keyword calls through a function value, duplicate keywords, positional
arguments after keywords, and defaults that reference mutable or captured state
needs to be explicit.

There is also a semantic question: generated default expressions are emitted
at a call or closure-read site rather than clearly being evaluated once at
function definition time. That may be acceptable, but it must be intentional.

**Implementation:** choose Python-like definition-time defaults or Emerald's
alternative, document it, then test direct calls, aliases, closures, nested
functions, duplicate/missing keywords, positional-after-keyword, and defaults
with side effects. If keyword calls on function values are not supported, reject
them with a stable checker diagnostic rather than allowing an inconsistent path.

### 13. Channel and task lifecycle needs stress coverage

The scheduler is deliberately built from detached pthreads plus a cooperative
token. That is a clever v1 tradeoff, but the ordinary tests do not establish
long-run behavior for task handles, repeated joins, close races, blocked senders,
blocked receivers, sleeping tasks, or allocation/GC while tasks are parked.

Before release, add stress cases for:

- joining a completed task twice;
- dropping a task handle while the task is still running;
- closing buffered and unbuffered channels with waiters on both sides;
- many short-lived tasks and repeated scheduler handoffs;
- GC while a value is in `xfer`, a channel buffer, a task result, or a closure;
- a real deadlock versus a program that becomes runnable after `sleep`.

Run these under ThreadSanitizer if the platform supports it. The goal is not
parallel speed; it is proving that the detached-thread implementation does not
turn a managed handle into a use-after-free or a false deadlock.

---

## P1 — release process and test-harness gaps

### 14. `task bless` can hide a failed regeneration

The `bless` recipes use `|| true` around check, proof, e2e, and stdlib commands.
That is convenient for generating negative goldens, but it also means a broken
compiler or failed link can silently leave an empty or stale expected file. The
normal runner catches the result later, but a reviewer may miss what happened
in a large bless diff.

**Implementation:** split blessing into commands that explicitly expect failure
and commands that must succeed. Write outputs to temporary files, verify the
expected exit status, then replace goldens atomically. Add `task bless-check` to
regenerate in a clean temporary tree and fail on any unexpected compile/run
failure.

### 15. The default release test does not exercise the release

`task test` runs the 185-case compiler/runtime suite, but not `task examples`,
`task install`, `task dist`, a fresh-prefix compile, sanitizer builds, or a
cross-platform build. Those are exactly the paths where the packaging and
resource bugs above live.

Add a `task release-check` that runs the suite, examples, archive/install smoke
test, and a clean checkout-style invocation. Keep sanitizer and fuzz tasks
separate if they are too slow for every pull request, but run them before the
release tag.

### 16. The generated C boundary is less strict than the compiler boundary

The compiler itself is built with `-Wall -Wextra`, but the generated program is
compiled by `src/main.c` with fixed `-std=c11 -O2 -pthread` flags and no warning
flags. There is no user-facing way to request `-O0 -g`, extra warnings, or a
sanitizer build. This makes debugging runtime and codegen failures needlessly
hard, and compiler warnings in generated C can remain invisible.

**Implementation:** add a small, documented set of C passthrough controls (for
example `--cc-arg` or `EMERALD_CFLAGS`) with safe argument handling. At minimum,
provide a debug/sanitizer task that invokes the compiler with a known runtime
configuration. Do not silently concatenate arbitrary flags into a shell command.

### 17. Version and package metadata have too many sources of truth

The version is currently supplied by `Taskfile.yml` and has a fallback macro in
`src/main.c`; release text, archive names, and changelog dates are edited by
hand. This is manageable once, but it is easy to ship a binary that reports a
different version from its archive or changelog.

**Implementation:** choose one version file or generated header and have build,
`--version`, `dist`, and release checks read it. Add a smoke test that compares
all reported package metadata before tagging.

---

## P2 — useful v1.1 work, not tag blockers

These are worthwhile, but none should delay a stable first release once the P0
items and runtime hardening are addressed:

- **Optimization/debug controls:** `-O0 -g`, `-O3`, compiler selection, and
  sanitizer-friendly builds.
- **Incremental modules:** cache parsed/type-checked import graphs; current
  linking reparses and rechecks the entire graph on every build and every REPL
  entry.
- **Language tooling:** a formatter, syntax highlighting/TextMate grammar, and
  an LSP-compatible diagnostic mode.
- **Convenience syntax:** methods-by-convention and chained comparisons.
- **Runtime collections:** static `dict`/`set` types, bounded hashable keys, or
  an explicit dynamic-collection type instead of returning `any` everywhere.
- **Text model:** Unicode/UTF-8 support, if the project decides byte strings are
  no longer sufficient.
- **Concurrency:** channel `select`, cancellation, task failure values, and a
  less surprising API for closing/send-after-close behavior.
- **Distribution:** platform packages, checksums/reproducible archives, a
  license/docs payload, and an uninstall story.
- **Self-hosting:** rewrite the lexer/parser/codegen in Emerald only after the
  runtime and packaging boundaries are reliable.

The existing intentional omissions — methods, chained comparisons, Unicode,
`select`, and general hashable dictionary keys — should remain explicit in the
README rather than being half-implemented to resemble Python.

---

## Recommended release gate

The tree should be called v1-ready when all of the following are true:

```text
[ ] task --force test
[ ] task examples
[ ] task release-check (fresh archive/prefix, outside the checkout)
[ ] installed compiler works when invoked through PATH
[ ] stdlib and runtime resources are found without developer-machine paths
[ ] malformed --emit-tokens/--emit-ast inputs fail with stable diagnostics
[ ] ASan/UBSan runtime smoke suite passes
[ ] numeric boundary, embedded-NUL, dynamic-type, I/O, and cycle tests pass
[ ] channel/task stress suite passes on the supported platforms
[ ] CI is present and green on every claimed platform
[ ] version reported by the binary matches the release artifact
[ ] final docs no longer claim an unimplemented workflow
```

After that gate, creating the `v1.0.0` tag is a release operation rather than a
bet that an untested boundary happens to work.
