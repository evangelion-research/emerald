"""Static knowledge about Emerald: keywords, type atoms, and the builtin table.

The builtin table is generated from the compiler's `include/builtins.def`,
which is the single list all three consumers agree on (checker, codegen, and
now this). `arity` is -1 for the variadic and optional-argument builtins that
codegen lowers specially, and `pure` is the fact a `pure` Emerald function
cares about -- a `pure` function may only call pure code, so surfacing it in
completion and hover prevents an `E_TYPE_PURE_CALL` before it is written.

The *typing* rules for builtins live in the checker and are not duplicated
here; when `--lsp-index` lands, hover for a builtin should come from it
instead of from this table's arity.
"""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True, slots=True)
class Builtin:
    name: str
    arity: int  # -1: variadic / specially lowered
    pure: bool
    returns_none: bool
    section: str

    @property
    def signature(self) -> str:
        args = "..." if self.arity < 0 else ", ".join(f"a{i}" for i in range(self.arity))
        tail = " -> None" if self.returns_none else ""
        return f"{self.name}({args}){tail}"

    def documentation(self) -> str:
        purity = "pure" if self.pure else "impure"
        note = (
            "callable from a `pure` function"
            if self.pure
            else "not callable from a `pure` function (E_TYPE_PURE_CALL)"
        )
        return f"builtin · {self.section} · {purity}\n\n{note}"


BUILTINS: dict[str, Builtin] = {
    'print': Builtin('print', -1, False, False, 'core'),
    'eprint': Builtin('eprint', -1, False, False, 'core'),
    'pprint': Builtin('pprint', 1, False, True, 'core'),
    'pprint_err': Builtin('pprint_err', 1, False, True, 'core'),
    'pp_format': Builtin('pp_format', 1, True, False, 'core'),
    'range': Builtin('range', -1, True, False, 'core'),
    'dict': Builtin('dict', -1, True, False, 'core'),
    'set': Builtin('set', -1, True, False, 'core'),
    'len': Builtin('len', 1, True, False, 'core'),
    'str': Builtin('str', 1, True, False, 'core'),
    'int': Builtin('int', 1, True, False, 'core'),
    'float': Builtin('float', 1, True, False, 'core'),
    'sqrt': Builtin('sqrt', 1, True, False, 'core'),
    'tan': Builtin('tan', 1, True, False, 'core'),
    'rand': Builtin('rand', 0, False, False, 'core'),
    'gc_stats': Builtin('gc_stats', 0, True, False, 'GC observability'),
    'gc_collect': Builtin('gc_collect', 0, False, False, 'GC observability'),
    'read_file': Builtin('read_file', 1, False, False, 'files and process'),
    'read_file_opt': Builtin('read_file_opt', 1, False, False, 'files and process'),
    'file_exists': Builtin('file_exists', 1, False, False, 'files and process'),
    'write_file': Builtin('write_file', 2, False, True, 'files and process'),
    'append_file': Builtin('append_file', 2, False, True, 'files and process'),
    'run': Builtin('run', 1, False, False, 'files and process'),
    'argv': Builtin('argv', 0, False, False, 'files and process'),
    'exit': Builtin('exit', 1, False, True, 'files and process'),
    'append': Builtin('append', 2, False, True, 'the stdlib foundation'),
    'slice': Builtin('slice', 3, True, False, 'the stdlib foundation'),
    'freeze': Builtin('freeze', 1, True, False, 'the stdlib foundation'),
    'thaw': Builtin('thaw', 1, True, False, 'the stdlib foundation'),
    'ord': Builtin('ord', 1, True, False, 'the stdlib foundation'),
    'chr': Builtin('chr', 1, True, False, 'the stdlib foundation'),
    'map': Builtin('map', 2, True, False, 'the stdlib foundation'),
    'filter': Builtin('filter', 2, True, False, 'the stdlib foundation'),
    'reduce': Builtin('reduce', 3, True, False, 'the stdlib foundation'),
    'read_line': Builtin('read_line', 0, False, False, 'the stdlib foundation'),
    'read_all': Builtin('read_all', 0, False, False, 'the stdlib foundation'),
    'input': Builtin('input', 1, False, False, 'the stdlib foundation'),
    'write_out': Builtin('write_out', 1, False, True, 'the stdlib foundation'),
    'write_err': Builtin('write_err', 1, False, True, 'the stdlib foundation'),
    'flush': Builtin('flush', 0, False, True, 'the stdlib foundation'),
    'now': Builtin('now', 0, False, False, 'the stdlib foundation'),
    'seed_rand': Builtin('seed_rand', 1, False, True, 'the stdlib foundation'),
    'spawn': Builtin('spawn', 1, False, False, 'green threads and channels'),
    'join': Builtin('join', 1, False, False, 'green threads and channels'),
    'task_done': Builtin('task_done', 1, False, False, 'green threads and channels'),
    'task_stats': Builtin('task_stats', 0, False, False, 'green threads and channels'),
    'task_yield': Builtin('task_yield', 0, False, True, 'green threads and channels'),
    'sleep': Builtin('sleep', 1, False, True, 'green threads and channels'),
    'chan': Builtin('chan', 1, False, False, 'green threads and channels'),
    'send': Builtin('send', 2, False, True, 'green threads and channels'),
    'recv': Builtin('recv', 1, False, False, 'green threads and channels'),
    'chan_close': Builtin('chan_close', 1, False, True, 'green threads and channels'),
    'chan_len': Builtin('chan_len', 1, False, False, 'green threads and channels'),
    'zeros': Builtin('zeros', 1, True, False, 'tensor primitives'),
    'ones': Builtin('ones', 1, True, False, 'tensor primitives'),
    'full': Builtin('full', 2, True, False, 'tensor primitives'),
    'arange': Builtin('arange', 1, True, False, 'tensor primitives'),
    'tensor': Builtin('tensor', 1, True, False, 'tensor primitives'),
    'randn': Builtin('randn', 2, False, False, 'tensor primitives'),
    'exp': Builtin('exp', 1, True, False, 'tensor primitives'),
    'log': Builtin('log', 1, True, False, 'tensor primitives'),
    'tanh': Builtin('tanh', 1, True, False, 'tensor primitives'),
    'relu': Builtin('relu', 1, True, False, 'tensor primitives'),
    'matmul': Builtin('matmul', 2, True, False, 'tensor primitives'),
    'reshape': Builtin('reshape', 2, True, False, 'tensor primitives'),
    'transpose': Builtin('transpose', 1, True, False, 'tensor primitives'),
    'permute': Builtin('permute', 2, True, False, 'tensor primitives'),
    'expand': Builtin('expand', 2, True, False, 'tensor primitives'),
    'sum': Builtin('sum', 2, True, False, 'tensor primitives'),
    'mean': Builtin('mean', 2, True, False, 'tensor primitives'),
    'max': Builtin('max', 2, True, False, 'tensor primitives'),
    'argmax': Builtin('argmax', 2, True, False, 'tensor primitives'),
    'tslice': Builtin('tslice', 4, True, False, 'tensor primitives'),
    'item': Builtin('item', 1, True, False, 'tensor primitives'),
    'shape': Builtin('shape', 1, True, False, 'tensor primitives'),
    'ndim': Builtin('ndim', 1, True, False, 'tensor primitives'),
    'dtype': Builtin('dtype', 1, True, False, 'tensor primitives'),
    'astype': Builtin('astype', 2, True, False, 'tensor primitives'),
}

# Keyword documentation, condensed from docs/grammar.md and docs/type-system.md.
KEYWORDS: dict[str, str] = {
    "def": "Define a function. `def f(x: int) -> int { ... }`; add `pure` to forbid impure calls, `partial` to opt out of termination checking.",
    "if": "Conditional. Braces, not indentation; `elif` and `else` follow.",
    "elif": "Another condition on an `if` chain.",
    "else": "The fallback branch of an `if`.",
    "while": "Loop while a condition holds. Under `--proof` a `while` needs `partial`.",
    "for": "`for x in iterable { ... }`.",
    "in": "Membership in a `for` header.",
    "return": "Return from the enclosing function.",
    "and": "Short-circuiting conjunction.",
    "or": "Short-circuiting disjunction.",
    "not": "Logical negation.",
    "True": "The true boolean, and the literal type `True`.",
    "False": "The false boolean, and the literal type `False`.",
    "None": "The absent value, and its type.",
    "break": "Leave the innermost loop.",
    "continue": "Next iteration of the innermost loop.",
    "pass": "Do nothing.",
    "type": "Type alias: `type Name = <type>`, optionally generic: `type Pair[A, B] = { a: A, b: B }`.",
    "const": "Immutable binding. Assigning again is `E_TYPE_CONST`; the default style for the functional core.",
    "match": "Exhaustive pattern match. The checker proves the arms cover the subject's type.",
    "pure": "Marks a function as pure: it may only call pure code.",
    "partial": "Opts a function out of termination checking.",
    "import": "`import m` binds the module; `import a.b as c` renames it. Top level only.",
    "from": "`from m import x, y as z` lifts names into this module. Top level only.",
    "as": "Rename an import binding.",
    "dim": "Declare nominally distinct dimension names for tensor shapes: `dim Batch, Seq`.",
    "error": "Declare an expected failure: `error NotFound { key: str }`, sugar for a record with a literal `_tag`.",
    "try": "Unwrap a result, or return its failure from the enclosing function. Not exception handling.",
    "catch": "`catch e { Arm x -> ..., _ -> ... }` -- an expression whose value is the success value or a matching arm.",
}

# Type atoms from docs/grammar.md, "Type Expressions".
TYPE_ATOMS: dict[str, str] = {
    "int": "Integer.",
    "float": "Floating point.",
    "str": "String. Immutable: assigning into one is `E_TYPE_IMMUTABLE`.",
    "bool": "Boolean.",
    "any": "The gradual escape hatch.",
    "never": "The empty type; what exhaustiveness proofs reduce to.",
    "list": "`list[T]` -- mutable sequence. Invariant under `--proof`.",
    "seq": "`seq[T]` -- immutable, covariant sequence. `freeze`/`thaw` convert.",
    "Tensor": "`Tensor[dtype, [d1, d2]]` with a static shape, or `Tensor[dtype, ?]` for a dynamic one.",
    "Fin": "`Fin[n]` -- an index provably below `n`.",
    "Eq": "`Eq[a, b]` -- evidence that two dim expressions are equal; `refl` inhabits `Eq[a, a]`.",
}

CONSTANTS: dict[str, str] = {
    "refl": "Evidence of `Eq[a, a]`. Erased at runtime.",
}


def describe(name: str) -> str | None:
    """Markdown documentation for a known name, or None if it is not ours."""
    if name in KEYWORDS:
        return f"**`{name}`** -- keyword\n\n{KEYWORDS[name]}"
    if name in TYPE_ATOMS:
        return f"**`{name}`** -- built-in type\n\n{TYPE_ATOMS[name]}"
    if name in CONSTANTS:
        return f"**`{name}`**\n\n{CONSTANTS[name]}"
    builtin = BUILTINS.get(name)
    if builtin is not None:
        return f"```emerald\n{builtin.signature}\n```\n\n{builtin.documentation()}"
    return None
