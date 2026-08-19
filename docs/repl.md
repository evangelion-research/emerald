# The REPL

```
$ emeraldc --repl          # or just: emeraldc
emerald repl — :help for commands, :quit to leave
emerald> 1 + 2
3
emerald> xs = [1, 2]
emerald> append(xs, 3)
emerald> xs
[1, 2, 3]
emerald> def sq(n: int) -> int {
.....     return n * n
..... }
emerald> sq(7)
49
```

## How it works, and why it works that way

Emerald compiles ahead of time. There is no interpreter, no partial program to
evaluate an expression against — the only thing that can run Emerald is a
native binary produced by `cc`. So the REPL does not evaluate anything itself:

**the session is its source text.** Each entry is appended to the text, the
whole program is compiled and run again, and only the new entry's output is
shown.

That is the entire design, and it buys the three properties that matter:

- **The type checker is the same type checker.** Nothing about a session is
  special-cased, so a program that works in the REPL works in a file, with the
  same diagnostics quoting the same lines.
- **State needs no runtime support.** `xs` still holds three elements on the
  next entry because the line that appended is still in the program.
- **Nothing is half-defined.** An entry is committed only if it both compiles
  and runs to a zero status, so a typo leaves the session exactly as it was.

### Showing only the new output

Re-running the whole program re-prints everything it ever printed. Diffing
output lengths would work until an earlier line printed the time or a random
number and the tail came out garbled. So the generated program prints one
improbable marker line between the committed session and the new entry, and the
REPL shows whatever follows the *last* marker. Nondeterministic earlier output
stays invisible however much it changes.

### Echoing a value

A single-line entry that is an expression — no assignment, no keyword, no open
brace — is wrapped in `print(...)` so its value is shown. A result of `None`
prints nothing, so a call kept for its effect (`append(xs, 3)`) stays quiet,
exactly as in Python. The line is committed **as typed**, never as its wrapper,
so its effects survive into the next entry without its value being re-echoed.

### The cost

**Effects repeat.** A session that writes a file writes it once per entry; one
that appends to a log appends once per entry. There is no way around this short
of an interpreter, and it is the one thing to keep in mind when a session
reaches out of the process. `:reset` starts over.

Compilation is also per entry — a full `cc` invocation — so entries land in a
few hundred milliseconds rather than instantly.

## Commands

| Command | Effect |
| --- | --- |
| `:help` | the command list |
| `:list` | the session's source so far |
| `:undo` | drop the last committed entry |
| `:reset` | start an empty session |
| `:cancel` | abandon a half-typed block |
| `:load FILE` | read a `.rald` file into the session |
| `:save FILE` | write the session's source to a file |
| `:quit` | leave (Ctrl-D does too) |

No Emerald line begins with a colon, so a command is always a command — even
in the middle of an unfinished block.

`:load` and `:save` are the bridge to ordinary files: prototype at the prompt,
`:save prototype.rald`, then compile it like anything else. Because the session
*is* source text, the saved file is exactly what ran.

## Multi-line entries

Braces are counted (ignoring strings and comments), so a `def`, an `if`, or any
other block keeps reading until it closes; the prompt changes to `.....`. An
unbalanced entry can always be abandoned with `:cancel`.

## Reading input inside a session

The session's program inherits the terminal, so `read_line()` and `input()`
read from the same place the REPL does. That is usually what is wanted at a
prompt and never what is wanted from a pipe: feeding a script to `--repl` on
stdin means the program and the REPL compete for it. Test console programs by
compiling them (see `tests/stdlib/console_test.rald`).

## Implementation

`src/repl.c`, driven from `src/main.c`; `tests/repl/*.in` are golden sessions
run by `tests/run_tests.sh repl`. The REPL re-invokes the compiler binary
itself (`argv[0]`) for each entry, passing through any `-I` roots, so it stays
a front end over the ordinary pipeline rather than a second copy of it.
