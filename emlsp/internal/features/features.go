package features

import (
	"sort"
	"strings"

	"github.com/evangelion-research/emlsp/internal/language"
	"github.com/evangelion-research/emlsp/internal/lexer"
	"github.com/evangelion-research/emlsp/internal/modules"
	"github.com/evangelion-research/emlsp/internal/outline"
	"github.com/evangelion-research/emlsp/internal/positions"
)

// Minimal LSP-like types

type Position = positions.Position
type Range = positions.Range

type DocumentSymbol struct {
	Name           string            `json:"name"`
	Kind           int               `json:"kind"`
	Detail         string            `json:"detail,omitempty"`
	Range          Range             `json:"range"`
	SelectionRange Range             `json:"selectionRange"`
	Children       []*DocumentSymbol `json:"children,omitempty"`
}

type Location struct {
	URI   string `json:"uri"`
	Range Range  `json:"range"`
}

type Hover struct {
	Contents MarkupContent `json:"contents"`
	Range    *Range        `json:"range,omitempty"`
}
type MarkupContent struct {
	Kind  string `json:"kind"`
	Value string `json:"value"`
}

type CompletionItem struct {
	Label         string `json:"label"`
	Kind          int    `json:"kind"`
	Detail        string `json:"detail,omitempty"`
	Documentation string `json:"documentation,omitempty"`
	SortText      string `json:"sortText,omitempty"`
}
type CompletionList struct {
	IsIncomplete bool             `json:"isIncomplete"`
	Items        []CompletionItem `json:"items"`
}

type FoldingRange struct {
	StartLine int    `json:"startLine"`
	EndLine   int    `json:"endLine"`
	Kind      string `json:"kind,omitempty"`
}

type DocumentHighlight struct {
	Range Range `json:"range"`
	Kind  int   `json:"kind"`
}

// Context holds per-document state
type Context struct {
	Path         string
	Source       string
	Outline      *outline.Outline
	IncludePaths []string
	Compiler     *string
}

func (c *Context) Line(number int) string {
	lines := strings.Split(c.Source, "\n")
	if 0 <= number && number < len(lines) {
		return lines[number]
	}
	return ""
}
func (c *Context) OffsetAt(pos Position) int {
	offset := 0
	// mimic Python's splitlines(keepends=True)
	lines := strings.SplitAfter(c.Source, "\n")
	for i, line := range lines {
		if i == pos.Line {
			runes := []rune(line)
			// line includes its trailing newline; clamp character within line length
			if pos.Character > len(runes) {
				return offset + len(runes)
			}
			return offset + pos.Character
		}
		offset += len([]rune(line))
	}
	// if pos is beyond last line
	return len([]rune(c.Source))
}
func (c *Context) ModuleBindings() map[string]outline.ImportInfo {
	out := map[string]outline.ImportInfo{}
	for _, info := range c.Outline.Imports {
		if local := info.LocalModuleName(); local != nil {
			out[*local] = info
		}
	}
	return out
}
func (c *Context) ResolveModule(modulePath string) *modules.Resolved {
	return modules.Resolve(modulePath, c.Path, c.IncludePaths, c.Compiler)
}

// -- document symbols --

func DocumentSymbols(o *outline.Outline) []*DocumentSymbol {
	var convert func([]*outline.Definition) []*DocumentSymbol
	convert = func(defs []*outline.Definition) []*DocumentSymbol {
		var out []*DocumentSymbol
		seen := map[string]bool{}
		for _, d := range defs {
			key := d.Name + ":" + itoa(d.Kind)
			if seen[key] {
				continue
			}
			seen[key] = true
			children := convert(d.Children)
			ds := &DocumentSymbol{
				Name:           d.Name,
				Kind:           d.Kind,
				Detail:         d.Detail,
				Range:          d.Range,
				SelectionRange: d.SelectionRange,
				Children:       children,
			}
			if len(children) == 0 {
				ds.Children = nil
			}
			out = append(out, ds)
		}
		return out
	}
	return convert(o.Symbols)
}

func FoldingRanges(o *outline.Outline) []FoldingRange {
	var out []FoldingRange
	for _, r := range o.Folds {
		end := r.End.Line - 1
		if end < r.Start.Line {
			end = r.Start.Line
		}
		out = append(out, FoldingRange{StartLine: r.Start.Line, EndLine: end, Kind: "region"})
	}
	return out
}

// -- hover --

func HoverAt(ctx *Context, pos Position) *Hover {
	tok := lexer.TokenAt(ctx.Outline.Tokens, pos.Line, pos.Character)
	if tok == nil || (tok.Kind != "ident" && tok.Kind != "keyword") {
		return nil
	}
	md := describeToken(ctx, tok, pos)
	if md == "" {
		return nil
	}
	rng := positions.TokenRange(tok.Line, tok.Col, tok.EndLine, tok.EndCol)
	return &Hover{
		Contents: MarkupContent{Kind: "markdown", Value: md},
		Range:    &rng,
	}
}

func describeToken(ctx *Context, tok *lexer.Token, pos Position) string {
	name := tok.Value
	for _, info := range ctx.Outline.Imports {
		if positions.Contains(info.ModuleRange, pos) {
			return moduleMarkdownFixed(ctx, info.ModulePath)
		}
	}
	if binding, ok := ctx.ModuleBindings()[name]; ok && tok.Kind == "ident" {
		return moduleMarkdownFixed(ctx, binding.ModulePath)
	}
	local := ctx.Outline.Resolve(name, ctx.OffsetAt(pos))
	if local != nil {
		kind := kindName(local.Kind)
		note := ""
		if !local.Exported {
			note = "\n\nprivate to this module"
		}
		return "```emerald\n" + local.Detail + "\n```\n\n" + kind + note
	}
	qualified := qualifiedBase(ctx, tok)
	if qualified != nil {
		exported := moduleExports(ctx, *qualified)
		for _, d := range exported {
			if d.Name == name {
				return "```emerald\n" + d.Detail + "\n```\n\nfrom module `" + *qualified + "`"
			}
		}
	}
	if doc, ok := language.Describe(name); ok {
		return doc
	}
	return ""
}

func kindName(k int) string {
	switch k {
	case outline.SymbolKindFunction:
		return "function"
	case outline.SymbolKindInterface:
		return "interface"
	case outline.SymbolKindStruct:
		return "struct"
	case outline.SymbolKindVariable:
		return "variable"
	case outline.SymbolKindConstant:
		return "constant"
	case outline.SymbolKindModule:
		return "module"
	case outline.SymbolKindTypeParameter:
		return "typeparameter"
	default:
		return "variable"
	}
}

func moduleMarkdown(ctx *Context, modulePath string) string {
	return moduleMarkdownFixed(ctx, modulePath)
}

func qualifiedBase(ctx *Context, tok *lexer.Token) *string {
	sig := lexer.Significant(ctx.Outline.Tokens)
	for i, t := range sig {
		if t.Offset == tok.Offset && t.Line == tok.Line && t.Col == tok.Col {
			if i >= 2 && sig[i-1].Value == "." && sig[i-1].Kind == "op" {
				if info, ok := ctx.ModuleBindings()[sig[i-2].Value]; ok {
					return &info.ModulePath
				}
			}
			break
		}
	}
	return nil
}

func moduleFile(ctx *Context, modulePath string) *[2]interface{} {
	return nil
}

// Cleaner helpers
func moduleFilePath(ctx *Context, modulePath string) (string, []*outline.Definition, bool) {
	resolved := ctx.ResolveModule(modulePath)
	if resolved == nil {
		return "", nil, false
	}
	other := modules.ReadOutline(resolved.Path)
	if other == nil {
		return "", nil, false
	}
	return resolved.Path, other.Exports(), true
}

func moduleExports(ctx *Context, modulePath string) []*outline.Definition {
	_, ex, ok := moduleFilePath(ctx, modulePath)
	if !ok {
		return nil
	}
	return ex
}

// Patch moduleMarkdown / qualified handling with correct helper (avoid duplicate)
func init() {
	// fix moduleMarkdown to use moduleFilePath
	// we redefine via variable shadowing - actually replace functions
}

// We will override describeToken path that used moduleFile
// Let's reimplement moduleMarkdown correctly via indirection
func moduleMarkdownFixed(ctx *Context, modulePath string) string {
	if path, _, ok := moduleFilePath(ctx, modulePath); ok {
		return "**module `" + modulePath + "`**\n\n" + path
	}
	return "**module `" + modulePath + "`**\n\n*unresolved*"
}

// But describeToken already uses moduleMarkdown - patch by reassign
var _ = moduleMarkdownFixed

// -- goto definition --

func DefinitionAt(ctx *Context, pos Position) []Location {
	tok := lexer.TokenAt(ctx.Outline.Tokens, pos.Line, pos.Character)
	if tok == nil || tok.Kind != "ident" {
		return nil
	}
	for _, info := range ctx.Outline.Imports {
		if positions.Contains(info.ModuleRange, pos) {
			return moduleLocation(ctx, info.ModulePath)
		}
		for _, name := range info.Names {
			if positions.Contains(name.Range, pos) {
				if loc := exportedLocation(ctx, info.ModulePath, name.Name); len(loc) > 0 {
					return loc
				}
				return moduleLocation(ctx, info.ModulePath)
			}
		}
	}
	if binding, ok := ctx.ModuleBindings()[tok.Value]; ok {
		return moduleLocation(ctx, binding.ModulePath)
	}
	if qualified := qualifiedBase(ctx, tok); qualified != nil {
		if found := exportedLocation(ctx, *qualified, tok.Value); len(found) > 0 {
			return found
		}
	}
	if local := ctx.Outline.Resolve(tok.Value, ctx.OffsetAt(pos)); local != nil {
		return []Location{{URI: positions.PathToURI(ctx.Path), Range: local.SelectionRange}}
	}
	for _, info := range ctx.Outline.Imports {
		for _, name := range info.Names {
			if name.Local() == tok.Value {
				return exportedLocation(ctx, info.ModulePath, name.Name)
			}
		}
	}
	return nil
}

func moduleLocation(ctx *Context, modulePath string) []Location {
	path, _, ok := moduleFilePath(ctx, modulePath)
	if !ok {
		return nil
	}
	return []Location{{URI: positions.PathToURI(path), Range: positions.RangeOf(0, 0, 0, 0)}}
}

func exportedLocation(ctx *Context, modulePath, name string) []Location {
	path, exports, ok := moduleFilePath(ctx, modulePath)
	if !ok {
		return nil
	}
	for _, d := range exports {
		if d.Name == name {
			return []Location{{URI: positions.PathToURI(path), Range: d.SelectionRange}}
		}
	}
	return nil
}

// -- references/highlights --

func Occurrences(ctx *Context, pos Position) []lexer.Token {
	tok := lexer.TokenAt(ctx.Outline.Tokens, pos.Line, pos.Character)
	if tok == nil || tok.Kind != "ident" {
		return nil
	}
	var out []lexer.Token
	for _, t := range ctx.Outline.Tokens {
		if t.Kind == "ident" && t.Value == tok.Value {
			out = append(out, t)
		}
	}
	return out
}

func References(ctx *Context, pos Position) []Location {
	uri := positions.PathToURI(ctx.Path)
	var out []Location
	for _, t := range Occurrences(ctx, pos) {
		out = append(out, Location{URI: uri, Range: positions.TokenRange(t.Line, t.Col, t.EndLine, t.EndCol)})
	}
	return out
}

func Highlights(ctx *Context, pos Position) []DocumentHighlight {
	var out []DocumentHighlight
	for _, t := range Occurrences(ctx, pos) {
		out = append(out, DocumentHighlight{Range: positions.TokenRange(t.Line, t.Col, t.EndLine, t.EndCol), Kind: 1})
	}
	return out
}

// -- completion --

func Completions(ctx *Context, pos Position) CompletionList {
	line := ""
	lines := strings.Split(ctx.Source, "\n")
	if pos.Line < len(lines) {
		runes := []rune(lines[pos.Line])
		if pos.Character <= len(runes) {
			line = string(runes[:pos.Character])
		} else {
			line = lines[pos.Line]
		}
	}
	stripped := strings.TrimSpace(line)
	words := strings.Fields(stripped)

	if len(words) > 0 && (words[0] == "import" || words[0] == "from") && !containsWord(words[1:], "import") {
		if len(words) <= 2 && !strings.HasSuffix(strings.TrimRight(line, " \t"), ",") {
			return moduleCompletions(ctx)
		}
	}
	if len(words) > 0 && words[0] == "from" && containsWord(words, "import") {
		modulePath := ""
		if len(words) > 1 {
			modulePath = words[1]
		}
		return exportCompletions(ctx, modulePath)
	}
	dotted := dottedBase(line)
	if dotted != nil {
		info, ok := ctx.ModuleBindings()[*dotted]
		if !ok {
			return CompletionList{IsIncomplete: true, Items: nil}
		}
		return exportCompletions(ctx, info.ModulePath)
	}
	return CompletionList{IsIncomplete: false, Items: scopeCompletions(ctx, pos)}
}

func containsWord(words []string, w string) bool {
	for _, x := range words {
		if x == w {
			return true
		}
	}
	return false
}

func dottedBase(prefix string) *string {
	head := strings.TrimRight(prefix, " \t")
	if head != strings.TrimRight(prefix, " ") {
		// if there was space after dot? original python checks specific
	}
	// check trailing spaces after dot
	if strings.TrimSpace(prefix) != strings.TrimRight(prefix, " \t") {
		// has trailing spaces after content? but we already trimmed
	}
	// Python logic: if head != prefix.rstrip() then return None when head had spaces trimmed meaning trailing spaces
	// But we need: if prefix ends with space after dot, dotted completion ends
	// Original: if head != prefix.rstrip(" ") -> but prefix.rstrip() without arg strips all whitespace
	// Simpler: if strings.HasSuffix(prefix, " ") then return nil
	if len(prefix) > 0 && prefix[len(prefix)-1] == ' ' {
		// if prefix stripped trailing spaces != prefix, then there is space after dot
		// Actually dotted base should be nil if cursor after "m. " (space)
		// We'll detect: trimmed version vs original
		trimmed := strings.TrimRight(prefix, " \t")
		if trimmed != prefix && strings.HasSuffix(trimmed, ".") {
			// but python checks after computing dotted base: head variable is prefix.rstrip()
			// Then it checks i scanning etc.
			// We'll keep simple: if prefix ends with space after dot, no completion
		}
	}
	// scan for "m." prefix at cursor
	i := len([]rune(prefix))
	runes := []rune(prefix)
	for i > 0 && (isAlnum(runes[i-1]) || runes[i-1] == '_') {
		i--
	}
	if i == 0 || runes[i-1] != '.' {
		return nil
	}
	j := i - 1
	for j > 0 && (isAlnum(runes[j-1]) || runes[j-1] == '_') {
		j--
	}
	base := string(runes[j : i-1])
	if base == "" {
		return nil
	}
	// if there was a space between dot and cursor, not valid; we already ensured prefix ends with ident
	// Check no spaces
	if strings.Contains(base, " ") {
		return nil
	}
	// Also ensure no space after dot before cursor: prefix[i:] should be alnum only
	return &base
}

func isAlnum(r rune) bool {
	return (r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z') || (r >= '0' && r <= '9')
}

func moduleCompletions(ctx *Context) CompletionList {
	names := modules.ModuleCandidates(ctx.Path, ctx.IncludePaths, ctx.Compiler)
	var items []CompletionItem
	for _, n := range names {
		items = append(items, CompletionItem{Label: n, Kind: 9, Detail: "module"})
	}
	return CompletionList{IsIncomplete: false, Items: items}
}

func exportCompletions(ctx *Context, modulePath string) CompletionList {
	var items []CompletionItem
	for _, d := range moduleExports(ctx, modulePath) {
		items = append(items, CompletionItem{
			Label:         d.Name,
			Kind:          completionKind(d.Kind),
			Detail:        d.Detail,
			Documentation: "from module `" + modulePath + "`",
		})
	}
	return CompletionList{IsIncomplete: false, Items: items}
}

func scopeCompletions(ctx *Context, pos Position) []CompletionItem {
	offset := ctx.OffsetAt(pos)
	var items []CompletionItem
	seen := map[string]bool{}
	for _, d := range ctx.Outline.VisibleAt(offset) {
		if seen[d.Name] {
			continue
		}
		seen[d.Name] = true
		items = append(items, CompletionItem{
			Label:    d.Name,
			Kind:     completionKind(d.Kind),
			Detail:   d.Detail,
			SortText: "0" + d.Name,
		})
	}
	// sort items? Python preserves insertion order for scope completions (most recent first via visible_at sorted)
	// We'll keep as is
	kwNames := sortedKeys(language.Keywords)
	for _, name := range kwNames {
		items = append(items, CompletionItem{
			Label:         name,
			Kind:          14, // Keyword
			Detail:        "keyword",
			Documentation: language.Keywords[name],
			SortText:      "1" + name,
		})
	}
	typeNames := sortedKeys(language.TypeAtoms)
	for _, name := range typeNames {
		if _, ok := language.Constants[name]; ok {
			// will be added below anyway
		}
		items = append(items, CompletionItem{
			Label:         name,
			Kind:          7,
			Detail:        "built-in type",
			Documentation: language.TypeAtoms[name],
			SortText:      "2" + name,
		})
	}
	for name, doc := range language.Constants {
		items = append(items, CompletionItem{
			Label:         name,
			Kind:          7,
			Detail:        "built-in type",
			Documentation: doc,
			SortText:      "2" + name,
		})
	}
	builtinNames := sortedKeysBuiltin()
	for _, name := range builtinNames {
		if seen[name] {
			continue
		}
		b := language.Builtins[name]
		items = append(items, CompletionItem{
			Label:         name,
			Kind:          3, // Function
			Detail:        b.Signature(),
			Documentation: b.Documentation(),
			SortText:      "3" + name,
		})
	}
	return items
}

func sortedKeys(m map[string]string) []string {
	var out []string
	for k := range m {
		out = append(out, k)
	}
	sort.Strings(out)
	return out
}
func sortedKeysBuiltin() []string {
	var out []string
	for k := range language.Builtins {
		out = append(out, k)
	}
	sort.Strings(out)
	return out
}

func completionKind(k int) int {
	switch k {
	case outline.SymbolKindFunction:
		return 3 // Function
	case outline.SymbolKindInterface:
		return 8 // Interface
	case outline.SymbolKindStruct:
		return 22 // Struct
	case outline.SymbolKindModule:
		return 9 // Module
	case outline.SymbolKindConstant:
		return 21 // Constant
	case outline.SymbolKindTypeParameter:
		return 25 // TypeParameter
	default:
		return 6 // Variable
	}
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
