# Emerald IDE

A native desktop IDE for the Emerald language, written in Go with [Fyne](https://fyne.io/).
It provides an editable source buffer, accepted-prefix proof checking, a goal panel,
an obligation ledger, diagnostics, and a type-checking REPL without project-owned C or Rust code.

## Features

- Open, save, save-as, and create `.rald` files from the File menu.
- Advance, retract, check to the cursor, check all, and interrupt proof sessions.
- Treat top-level Emerald statements as the proof-session unit.
- Display source-derived symbols and `never` obligations.
- Run `emeraldc --check --json` asynchronously with a timeout and structured diagnostics.
- Fall back to lightweight parse checks when `emeraldc` is unavailable.
- Choose emerald, tokyo, mono, or paper appearance modes.
- Preserve native Fyne text editing, selection, clipboard, undo, and redo behavior.

## Build and run

Requires Go 1.23 or newer. Fyne uses the platform graphics stack; on macOS,
install Xcode Command Line Tools if they are not already available.

```sh
go mod download
go test ./...
go run .
go build -o emerald-ide .
```

The application has a single Go build graph. `go.mod` tracks Fyne; `go.sum` pins
its transitive dependencies.

## Compiler resolution

The proof channel searches for `emeraldc` in this order:

1. `$EMERALDC`
2. `$PATH`
3. `../emerald/bin/emeraldc`

The status bar shows the source of the active compiler. Every check writes the
current buffer to a temporary overlay next to the open file when possible, runs
`emeraldc --check --json`, captures stdout/stderr, and removes the overlay.
Missing compilers leave the editor usable and use source-level parse diagnostics.

## Keybindings

| Action | Binding |
|---|---|
| advance | Cmd/Ctrl+Down |
| retract | Cmd/Ctrl+Up |
| check to cursor | Cmd/Ctrl+Right |
| check all | Cmd/Ctrl+Enter |
| interrupt | Cmd/Ctrl+. |
| open / save / save-as / new | Cmd/Ctrl+O / S / Shift+S / N |

The toolbar and Session menu expose the proof controls for mouse users.

## Layout

```text
main.go       Fyne window, editor, menus, file operations, status bar
analysis.go   Emerald statement outline, symbols, obligations, token spans
session.go    proof-session state machine, compiler discovery, subprocess checks
panels.go     goal, symbols, ledger, diagnostics, and REPL views
theme.go      built-in Fyne appearance selection
*_test.go     parser, position, diagnostics, and session regression tests
```

The source analysis intentionally avoids a vendored parser generator. It is a
small editor-oriented lexer/outline pass; semantic checking remains the job of
`emeraldc`.
