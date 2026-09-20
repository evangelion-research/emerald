package language

// Builtin mirrors include/builtins.def
type Builtin struct {
	Name         string
	Arity        int // -1 variadic / specially lowered
	Pure         bool
	ReturnsNone  bool
	Section      string
}

func (b Builtin) Signature() string {
	var args string
	if b.Arity < 0 {
		args = "..."
	} else {
		for i := 0; i < b.Arity; i++ {
			if i > 0 {
				args += ", "
			}
			args += "a" + itoa(i)
		}
	}
	tail := ""
	if b.ReturnsNone {
		tail = " -> None"
	}
	return b.Name + "(" + args + ")" + tail
}

func (b Builtin) Documentation() string {
	purity := "impure"
	note := "not callable from a `pure` function (E_TYPE_PURE_CALL)"
	if b.Pure {
		purity = "pure"
		note = "callable from a `pure` function"
	}
	return "builtin \u00b7 " + b.Section + " \u00b7 " + purity + "\n\n" + note
}

func itoa(i int) string {
	if i == 0 {
		return "0"
	}
	s := ""
	for i > 0 {
		s = string(rune('0'+i%10)) + s
		i /= 10
	}
	return s
}

var Builtins = map[string]Builtin{
	"print":       {"print", -1, false, false, "core"},
	"eprint":      {"eprint", -1, false, false, "core"},
	"pprint":      {"pprint", 1, false, true, "core"},
	"pprint_err":  {"pprint_err", 1, false, true, "core"},
	"pp_format":   {"pp_format", 1, true, false, "core"},
	"range":       {"range", -1, true, false, "core"},
	"dict":        {"dict", -1, true, false, "core"},
	"set":         {"set", -1, true, false, "core"},
	"len":         {"len", 1, true, false, "core"},
	"str":         {"str", 1, true, false, "core"},
	"int":         {"int", 1, true, false, "core"},
	"float":       {"float", 1, true, false, "core"},
	"sqrt":        {"sqrt", 1, true, false, "core"},
	"tan":         {"tan", 1, true, false, "core"},
	"rand":        {"rand", 0, false, false, "core"},
	"time_now":    {"time_now", 0, false, false, "time"},
	"unix_time":   {"unix_time", 0, false, false, "time"},
	"utc_date":    {"utc_date", 1, true, false, "time"},
	"fnv1a":       {"fnv1a", 1, true, false, "hashing"},
	"sha256":      {"sha256", 1, true, false, "hashing"},
	"json_parse":  {"json_parse", 1, true, false, "JSON"},
	"json_stringify": {"json_stringify", 1, true, false, "JSON"},
	"gc_stats":    {"gc_stats", 0, true, false, "GC observability"},
	"gc_collect":  {"gc_collect", 0, false, false, "GC observability"},
	"read_file":     {"read_file", 1, false, false, "files and process"},
	"read_file_opt": {"read_file_opt", 1, false, false, "files and process"},
	"file_exists":   {"file_exists", 1, false, false, "files and process"},
	"write_file":    {"write_file", 2, false, true, "files and process"},
	"append_file":   {"append_file", 2, false, true, "files and process"},
	"run":           {"run", 1, false, false, "files and process"},
	"argv":          {"argv", 0, false, false, "files and process"},
	"exit":          {"exit", 1, false, true, "files and process"},
	"append":    {"append", 2, false, true, "the stdlib foundation"},
	"slice":     {"slice", 3, true, false, "the stdlib foundation"},
	"freeze":    {"freeze", 1, true, false, "the stdlib foundation"},
	"thaw":      {"thaw", 1, true, false, "the stdlib foundation"},
	"ord":       {"ord", 1, true, false, "the stdlib foundation"},
	"chr":       {"chr", 1, true, false, "the stdlib foundation"},
	"map":       {"map", 2, true, false, "the stdlib foundation"},
	"filter":    {"filter", 2, true, false, "the stdlib foundation"},
	"reduce":    {"reduce", 3, true, false, "the stdlib foundation"},
	"read_line": {"read_line", 0, false, false, "the stdlib foundation"},
	"read_all":  {"read_all", 0, false, false, "the stdlib foundation"},
	"input":     {"input", 1, false, false, "the stdlib foundation"},
	"write_out": {"write_out", 1, false, true, "the stdlib foundation"},
	"write_err": {"write_err", 1, false, true, "the stdlib foundation"},
	"flush":     {"flush", 0, false, true, "the stdlib foundation"},
	"now":       {"now", 0, false, false, "the stdlib foundation"},
	"seed_rand": {"seed_rand", 1, false, true, "the stdlib foundation"},
	"spawn":     {"spawn", 1, false, false, "green threads and channels"},
	"join":      {"join", 1, false, false, "green threads and channels"},
	"task_done": {"task_done", 1, false, false, "green threads and channels"},
	"task_stats": {"task_stats", 0, false, false, "green threads and channels"},
	"task_yield": {"task_yield", 0, false, true, "green threads and channels"},
	"sleep":     {"sleep", 1, false, true, "green threads and channels"},
	"chan":      {"chan", 1, false, false, "green threads and channels"},
	"send":      {"send", 2, false, true, "green threads and channels"},
	"recv":      {"recv", 1, false, false, "green threads and channels"},
	"chan_close": {"chan_close", 1, false, true, "green threads and channels"},
	"chan_len":   {"chan_len", 1, false, false, "green threads and channels"},
	"zeros":     {"zeros", 1, true, false, "tensor primitives"},
	"ones":      {"ones", 1, true, false, "tensor primitives"},
	"full":      {"full", 2, true, false, "tensor primitives"},
	"arange":    {"arange", 1, true, false, "tensor primitives"},
	"tensor":    {"tensor", 1, true, false, "tensor primitives"},
	"randn":     {"randn", 2, false, false, "tensor primitives"},
	"exp":       {"exp", 1, true, false, "tensor primitives"},
	"log":       {"log", 1, true, false, "tensor primitives"},
	"tanh":      {"tanh", 1, true, false, "tensor primitives"},
	"relu":      {"relu", 1, true, false, "tensor primitives"},
	"matmul":    {"matmul", 2, true, false, "tensor primitives"},
	"reshape":   {"reshape", 2, true, false, "tensor primitives"},
	"transpose": {"transpose", 1, true, false, "tensor primitives"},
	"permute":   {"permute", 2, true, false, "tensor primitives"},
	"expand":    {"expand", 2, true, false, "tensor primitives"},
	"sum":       {"sum", 2, true, false, "tensor primitives"},
	"mean":      {"mean", 2, true, false, "tensor primitives"},
	"max":       {"max", 2, true, false, "tensor primitives"},
	"argmax":    {"argmax", 2, true, false, "tensor primitives"},
	"tslice":    {"tslice", 4, true, false, "tensor primitives"},
	"item":      {"item", 1, true, false, "tensor primitives"},
	"shape":     {"shape", 1, true, false, "tensor primitives"},
	"ndim":      {"ndim", 1, true, false, "tensor primitives"},
	"dtype":     {"dtype", 1, true, false, "tensor primitives"},
	"astype":    {"astype", 2, true, false, "tensor primitives"},
}

var Keywords = map[string]string{
	"def":      "Define a function. `def f(x: int) -> int { ... }`; add `pure` to forbid impure calls, `partial` to opt out of termination checking.",
	"if":       "Conditional. Braces, not indentation; `elif` and `else` follow.",
	"elif":     "Another condition on an `if` chain.",
	"else":     "The fallback branch of an `if`.",
	"while":    "Loop while a condition holds. Under `--proof` a `while` needs `partial`.",
	"for":      "`for x in iterable { ... }`.",
	"in":       "Membership in a `for` header.",
	"return":   "Return from the enclosing function.",
	"and":      "Short-circuiting conjunction.",
	"or":       "Short-circuiting disjunction.",
	"not":      "Logical negation.",
	"True":     "The true boolean, and the literal type `True`.",
	"False":    "The false boolean, and the literal type `False`.",
	"None":     "The absent value, and its type.",
	"break":    "Leave the innermost loop.",
	"continue": "Next iteration of the innermost loop.",
	"pass":     "Do nothing.",
	"type":     "Type alias: `type Name = <type>`, optionally generic: `type Pair[A, B] = { a: A, b: B }`.",
	"const":    "Immutable binding. Assigning again is `E_TYPE_CONST`; the default style for the functional core.",
	"match":    "Exhaustive pattern match. The checker proves the arms cover the subject's type.",
	"pure":     "Marks a function as pure: it may only call pure code.",
	"partial":  "Opts a function out of termination checking.",
	"import":   "`import m` binds the module; `import a.b as c` renames it. Top level only.",
	"from":     "`from m import x, y as z` lifts names into this module. Top level only.",
	"as":       "Rename an import binding.",
	"dim":      "Declare nominally distinct dimension names for tensor shapes: `dim Batch, Seq`.",
	"error":    "Declare an expected failure: `error NotFound { key: str }`, sugar for a record with a literal `_tag`.",
	"try":      "Unwrap a result, or return its failure from the enclosing function. Not exception handling.",
	"catch":    "`catch e { Arm x -> ..., _ -> ... }` -- an expression whose value is the success value or a matching arm.",
}

var TypeAtoms = map[string]string{
	"int":    "Integer.",
	"float":  "Floating point.",
	"str":    "String. Immutable: assigning into one is `E_TYPE_IMMUTABLE`.",
	"bool":   "Boolean.",
	"any":    "The gradual escape hatch.",
	"never":  "The empty type; what exhaustiveness proofs reduce to.",
	"list":   "`list[T]` -- mutable sequence. Invariant under `--proof`.",
	"seq":    "`seq[T]` -- immutable, covariant sequence. `freeze`/`thaw` convert.",
	"Tensor": "`Tensor[dtype, [d1, d2]]` with a static shape, or `Tensor[dtype, ?]` for a dynamic one.",
	"Fin":    "`Fin[n]` -- an index provably below `n`.",
	"Eq":     "`Eq[a, b]` -- evidence that two dim expressions are equal; `refl` inhabits `Eq[a, a]`.",
}

var Constants = map[string]string{
	"refl": "Evidence of `Eq[a, a]`. Erased at runtime.",
}

func Describe(name string) (string, bool) {
	if d, ok := Keywords[name]; ok {
		return "**`" + name + "`** -- keyword\n\n" + d, true
	}
	if d, ok := TypeAtoms[name]; ok {
		return "**`" + name + "`** -- built-in type\n\n" + d, true
	}
	if d, ok := Constants[name]; ok {
		return "**`" + name + "`**\n\n" + d, true
	}
	if b, ok := Builtins[name]; ok {
		return "```emerald\n" + b.Signature() + "\n```\n\n" + b.Documentation(), true
	}
	return "", false
}
