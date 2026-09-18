# Security Policy

## Reporting a vulnerability

Do **not** open a public issue for a security problem. Email
**security@evangelion-research.org** (PGP key on request) and include:

- the smallest reproduction (a `.rald` file and the exact `emeraldc`/`pme`
  command line),
- the affected commit or release,
- the impact you believe it has.

You will get an acknowledgment within 72 hours. Please give us 90 days to fix
and publish a coordinated advisory before any public disclosure.

## Scope

In scope: the `emeraldc` driver and compiler pipeline (crashes, memory
unsafety, code execution through *any* command-line input — paths, `-I`
arguments, source files), the runtime as compiled into generated programs,
`pme` (registry fetching, archive extraction, store handling), and `emlsp`
(processing untrusted `.rald` text from an editor).

Out of scope: the generated C's behavior *for programs the user chose to
compile*, and denial-of-service through adversarially huge inputs without a
memory-safety consequence.

## What we promise

- Command-line arguments are never interpreted by a shell anywhere in the
  driver (this class of bug is fixed and regression-tested; see the `execvp`
  argv path in `src/main.c`).
- Sanitizer and fuzz runs are part of the release checklist
  (`.github/workflows/`).
