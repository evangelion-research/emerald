package outline

import (
	"strings"

	"github.com/evangelion-research/emlsp/internal/lexer"
	"github.com/evangelion-research/emlsp/internal/positions"
)

// LSP SymbolKind values actually used by the outline
const (
	SymbolKindModule        = 2
	SymbolKindInterface     = 11
	SymbolKindFunction      = 12
	SymbolKindVariable      = 13
	SymbolKindConstant      = 14
	SymbolKindString        = 15
	SymbolKindNumber        = 16
	SymbolKindBoolean       = 17
	SymbolKindArray         = 18
	SymbolKindObject        = 19
	SymbolKindEnumMember    = 22
	SymbolKindStruct        = 23
	SymbolKindTypeParameter = 25
)

type Definition struct {
	Name          string
	Kind          int
	Detail        string
	Range         positions.Range
	SelectionRange positions.Range
	ScopeStart    int
	ScopeEnd      int
	Exported      bool
	Children      []*Definition
	IsParameter   bool
	IsImport      bool
}

type ImportedName struct {
	Name  string
	Alias *string
	Range positions.Range
}

func (n ImportedName) Local() string {
	if n.Alias != nil {
		return *n.Alias
	}
	return n.Name
}

type ImportInfo struct {
	Kind        string // import|from
	ModulePath  string
	ModuleRange positions.Range
	Alias       *string
	Names       []ImportedName
	Range       positions.Range
}

func (i ImportInfo) LocalModuleName() *string {
	if i.Kind != "import" {
		return nil
	}
	parts := strings.Split(i.ModulePath, ".")
	last := parts[len(parts)-1]
	if i.Alias != nil {
		return i.Alias
	}
	return &last
}

type Outline struct {
	Tokens      []lexer.Token
	Symbols     []*Definition
	Definitions []*Definition
	Imports     []ImportInfo
	Folds       []positions.Range
}

func (o *Outline) VisibleAt(offset int) []*Definition {
	var hits []*Definition
	for _, d := range o.Definitions {
		if d.ScopeStart <= offset && offset <= d.ScopeEnd {
			hits = append(hits, d)
		}
	}
	// sort descending scope_start
	for i := 0; i < len(hits); i++ {
		for j := i + 1; j < len(hits); j++ {
			if hits[j].ScopeStart > hits[i].ScopeStart {
				hits[i], hits[j] = hits[j], hits[i]
			}
		}
	}
	return hits
}

func (o *Outline) Resolve(name string, offset int) *Definition {
	for _, d := range o.VisibleAt(offset) {
		if d.Name == name {
			return d
		}
	}
	return nil
}

func (o *Outline) Exports() []*Definition {
	var out []*Definition
	for _, d := range o.Symbols {
		if d.Exported {
			out = append(out, d)
		}
	}
	return out
}

func Build(source string) *Outline {
	b := &builder{
		source:    source,
		allTokens: lexer.Tokenize(source),
	}
	b.toks = lexer.Significant(b.allTokens)
	return b.run()
}

type frame struct {
	owner      *Definition
	depth    int
	scopeStart int
	scopeEnd   int
}

var topLevelKind = map[string]int{
	"def":   SymbolKindFunction,
	"type":  SymbolKindInterface,
	"error": SymbolKindStruct,
}

var closers = map[string]string{
	"{": "}",
	"(": ")",
	"[": "]",
}

type builder struct {
	source    string
	allTokens []lexer.Token
	toks      []lexer.Token
	defs      []*Definition
	roots     []*Definition
	imports   []ImportInfo
	folds     []positions.Range
	stack     []frame
}

func (b *builder) at(i int) *lexer.Token {
	if 0 <= i && i < len(b.toks) {
		return &b.toks[i]
	}
	return nil
}
func (b *builder) isOp(i int, ops ...string) bool {
	t := b.at(i)
	if t == nil || t.Kind != "op" {
		return false
	}
	for _, o := range ops {
		if t.Value == o {
			return true
		}
	}
	return false
}
func (b *builder) isKw(i int, words ...string) bool {
	t := b.at(i)
	if t == nil || t.Kind != "keyword" {
		return false
	}
	for _, w := range words {
		if t.Value == w {
			return true
		}
	}
	return false
}
func (b *builder) matching(i int) int {
	openTok := b.toks[i]
	close := closers[openTok.Value]
	depth := 0
	for j := i; j < len(b.toks); j++ {
		t := b.toks[j]
		if t.Kind != "op" {
			continue
		}
		if t.Value == openTok.Value {
			depth++
		} else if t.Value == close {
			depth--
			if depth == 0 {
				return j
			}
		}
	}
	return len(b.toks) - 1
}

func (b *builder) add(d *Definition) {
	b.defs = append(b.defs, d)
	if len(b.stack) > 0 {
		b.stack[len(b.stack)-1].owner.Children = append(b.stack[len(b.stack)-1].owner.Children, d)
	} else {
		b.roots = append(b.roots, d)
	}
}

func (b *builder) run() *Outline {
	depth := 0
	i := 0
	for i < len(b.toks) {
		t := b.toks[i]
		if t.Kind == "op" && t.Value == "{" {
			depth++
			i++
			continue
		}
		if t.Kind == "op" && t.Value == "}" {
			depth--
			for len(b.stack) > 0 && depth < b.stack[len(b.stack)-1].depth {
				b.stack = b.stack[:len(b.stack)-1]
			}
			i++
			continue
		}
		if t.Kind == "keyword" {
			switch t.Value {
			case "def":
				i = b.funcDef(i, depth)
				continue
			case "type", "error":
				i = b.typeLike(i, t.Value, depth)
				continue
			case "dim":
				i = b.dims(i, depth)
				continue
			case "const":
				i = b.binding(i+1, depth, true)
				continue
			case "import", "from":
				i = b.importStmt(i)
				continue
			}
			i++
			continue
		}
		if t.Kind == "ident" && b.assigns(i) {
			i = b.binding(i, depth, false)
			continue
		}
		i++
	}
	b.foldBlocks()
	return &Outline{
		Tokens:      b.allTokens,
		Symbols:     b.roots,
		Definitions: b.defs,
		Imports:     b.imports,
		Folds:       b.folds,
	}
}

func (b *builder) assigns(i int) bool {
	if b.isOp(i+1, "=") {
		return true
	}
	if !b.isOp(i+1, ":") {
		return false
	}
	for j := i + 2; j < i+40 && j < len(b.toks); j++ {
		if b.isOp(j, "=") {
			return true
		}
		if b.isOp(j, "{", "}", ";") || b.toks[j].Line != b.toks[i].Line {
			return false
		}
	}
	return false
}

func (b *builder) scopeBounds() (int, int) {
	if len(b.stack) > 0 {
		f := b.stack[len(b.stack)-1]
		return f.scopeStart, f.scopeEnd
	}
	return 0, len([]rune(b.source))
}

func (b *builder) funcDef(i, depth int) int {
	nameTok := b.at(i + 1)
	if nameTok == nil || nameTok.Kind != "ident" {
		return i + 1
	}
	j := i + 2
	var brace *int
	for j < len(b.toks) {
		if b.isOp(j, "{") {
			brace = &j
			break
		}
		if b.toks[j].Kind == "keyword" && b.toks[j].Value == "def" {
			break
		}
		j++
	}
	var endTok lexer.Token
	if brace != nil {
		endTok = b.toks[b.matching(*brace)]
	} else {
		endTok = *nameTok
	}
	var braceIdx int
	if brace != nil {
		braceIdx = *brace
	} else {
		braceIdx = j
	}
	// slice detail
	var detailEnd lexer.Token
	if brace != nil {
		detailEnd = b.toks[braceIdx-1]
	} else {
		if j-1 >= 0 && j-1 < len(b.toks) {
			detailEnd = b.toks[j-1]
		} else {
			detailEnd = *nameTok
		}
	}
	// if detailEnd not valid (when j == i+2 and no brace), use nameTok
	detail := slice(b.source, b.toks[i], detailEnd)
	outerStart, outerEnd := b.scopeBounds()
	d := &Definition{
		Name:           nameTok.Value,
		Kind:           SymbolKindFunction,
		Detail:         detail,
		Range:          positions.Span(b.toks[i].Line, b.toks[i].Col, endTok.EndLine, endTok.EndCol),
		SelectionRange: positions.TokenRange(nameTok.Line, nameTok.Col, nameTok.EndLine, nameTok.EndCol),
		ScopeStart:     outerStart,
		ScopeEnd:       outerEnd,
		Exported:       depth == 0 && !strings.HasPrefix(nameTok.Value, "_"),
	}
	b.add(d)
	if brace == nil {
		return i + 2
	}
	bodyStart := b.toks[*brace].Offset
	bodyEnd := endTok.Offset + len([]rune(endTok.Value))
	b.params(i+2, *brace, bodyStart, bodyEnd)
	b.stack = append(b.stack, frame{owner: d, depth: depth + 1, scopeStart: bodyStart, scopeEnd: bodyEnd})
	return i + 2
}

func (b *builder) params(start, brace, scopeStart, scopeEnd int) {
	i := start
	for i < brace && !b.isOp(i, "(") {
		i++
	}
	if i >= brace {
		return
	}
	closeIdx := b.matching(i)
	depth := 0
	for j := i; j < closeIdx; j++ {
		t := b.toks[j]
		if t.Kind == "op" && (t.Value == "(" || t.Value == "[" || t.Value == "{") {
			depth++
		} else if t.Kind == "op" && (t.Value == ")" || t.Value == "]" || t.Value == "}") {
			depth--
		} else if depth == 1 && t.Kind == "ident" && b.isOp(j-1, "(", ",") {
			b.defs = append(b.defs, &Definition{
				Name:           t.Value,
				Kind:           SymbolKindVariable,
				Detail:         paramDetail(b.source, b.toks, j, closeIdx),
				Range:          positions.TokenRange(t.Line, t.Col, t.EndLine, t.EndCol),
				SelectionRange: positions.TokenRange(t.Line, t.Col, t.EndLine, t.EndCol),
				ScopeStart:     scopeStart,
				ScopeEnd:       scopeEnd,
				Exported:       false,
				IsParameter:    true,
			})
		}
	}
}

func (b *builder) typeLike(i int, keyword string, depth int) int {
	nameTok := b.at(i + 1)
	if nameTok == nil || nameTok.Kind != "ident" {
		return i + 1
	}
	end := *nameTok
	if keyword == "type" {
		j := i + 2
		for j < len(b.toks) && b.toks[j].Line == nameTok.Line {
			end = b.toks[j]
			j++
		}
	} else if b.isOp(i+2, "{") {
		end = b.toks[b.matching(i+2)]
	}
	start, stop := b.scopeBounds()
	b.add(&Definition{
		Name:           nameTok.Value,
		Kind:           topLevelKind[keyword],
		Detail:         slice(b.source, b.toks[i], end),
		Range:          positions.Span(b.toks[i].Line, b.toks[i].Col, end.EndLine, end.EndCol),
		SelectionRange: positions.TokenRange(nameTok.Line, nameTok.Col, nameTok.EndLine, nameTok.EndCol),
		ScopeStart:     start,
		ScopeEnd:       stop,
		Exported:       depth == 0 && !strings.HasPrefix(nameTok.Value, "_"),
	})
	return i + 2
}

func (b *builder) dims(i, depth int) int {
	start, stop := b.scopeBounds()
	j := i + 1
	for j < len(b.toks) {
		t := b.toks[j]
		if t.Kind != "ident" {
			break
		}
		b.add(&Definition{
			Name:           t.Value,
			Kind:           SymbolKindTypeParameter,
			Detail:         "dim " + t.Value,
			Range:          positions.TokenRange(t.Line, t.Col, t.EndLine, t.EndCol),
			SelectionRange: positions.TokenRange(t.Line, t.Col, t.EndLine, t.EndCol),
			ScopeStart:     start,
			ScopeEnd:       stop,
			Exported:       depth == 0 && !strings.HasPrefix(t.Value, "_"),
		})
		if !b.isOp(j+1, ",") {
			break
		}
		j += 2
	}
	return j + 1
}

func (b *builder) binding(i, depth int, isConst bool) int {
	nameTok := b.at(i)
	if nameTok == nil || nameTok.Kind != "ident" {
		return i + 1
	}
	end := *nameTok
	j := i + 1
	for j < len(b.toks) && b.toks[j].Line == nameTok.Line {
		if b.isOp(j, "}", ";") {
			break
		}
		end = b.toks[j]
		j++
	}
	_, stop := b.scopeBounds()
	var detail string
	if isConst {
		detail = slice(b.source, b.toks[i-1], end)
	} else {
		detail = slice(b.source, *nameTok, end)
	}
	kind := SymbolKindVariable
	if isConst {
		kind = SymbolKindConstant
	}
	b.add(&Definition{
		Name:           nameTok.Value,
		Kind:           kind,
		Detail:         detail,
		Range:          positions.Span(nameTok.Line, nameTok.Col, end.EndLine, end.EndCol),
		SelectionRange: positions.TokenRange(nameTok.Line, nameTok.Col, nameTok.EndLine, nameTok.EndCol),
		ScopeStart:     nameTok.Offset,
		ScopeEnd:       stop,
		Exported:       depth == 0 && !strings.HasPrefix(nameTok.Value, "_"),
	})
	return j
}

func (b *builder) importStmt(i int) int {
	kw := b.toks[i]
	j := i + 1
	var parts []lexer.Token
	for j < len(b.toks) {
		t := b.toks[j]
		if t.Kind == "ident" {
			parts = append(parts, t)
			j++
			if b.isOp(j, ".") {
				j++
				continue
			}
		}
		break
	}
	if len(parts) == 0 {
		return i + 1
	}
	var vals []string
	for _, p := range parts {
		vals = append(vals, p.Value)
	}
	modulePath := strings.Join(vals, ".")
	moduleRange := positions.Span(parts[0].Line, parts[0].Col, parts[len(parts)-1].EndLine, parts[len(parts)-1].EndCol)
	var alias *string
	var names []ImportedName
	end := parts[len(parts)-1]
	if kw.Value == "import" {
		if b.isKw(j, "as") {
			if a := b.at(j + 1); a != nil && a.Kind == "ident" {
				s := a.Value
				alias = &s
				end = *a
				j += 2
			}
		}
	} else {
		if b.isKw(j, "import") {
			j++
			for j < len(b.toks) {
				t := b.at(j)
				if t == nil || t.Kind != "ident" {
					break
				}
				nm := t.Value
				var na *string
				j++
				if b.isKw(j, "as") {
					if a := b.at(j + 1); a != nil && a.Kind == "ident" {
						s := a.Value
						na = &s
						end = *a
						j += 2
					}
				} else {
					end = *t
				}
				names = append(names, ImportedName{Name: nm, Alias: na, Range: positions.TokenRange(t.Line, t.Col, t.EndLine, t.EndCol)})
				if !b.isOp(j, ",") {
					break
				}
				j++
			}
		}
	}
	info := ImportInfo{
		Kind:        kw.Value,
		ModulePath:  modulePath,
		ModuleRange: moduleRange,
		Alias:       alias,
		Names:       names,
		Range:       positions.Span(kw.Line, kw.Col, end.EndLine, end.EndCol),
	}
	b.imports = append(b.imports, info)
	var bound []struct {
		name string
		rng  positions.Range
	}
	if info.Kind == "import" {
		if local := info.LocalModuleName(); local != nil {
			bound = append(bound, struct {
				name string
				rng  positions.Range
			}{*local, moduleRange})
		}
	} else {
		for _, n := range names {
			bound = append(bound, struct {
				name string
				rng  positions.Range
			}{n.Local(), n.Range})
		}
	}
	for _, bn := range bound {
		kind := SymbolKindModule
		if info.Kind != "import" {
			kind = SymbolKindVariable
		}
		detail := slice(b.source, kw, end)
		b.add(&Definition{
			Name:           bn.name,
			Kind:           kind,
			Detail:         detail,
			Range:          info.Range,
			SelectionRange: bn.rng,
			ScopeStart:     0,
			ScopeEnd:       len([]rune(b.source)),
			Exported:       false,
			IsImport:       true,
		})
	}
	return j
}

func (b *builder) foldBlocks() {
	var stack []lexer.Token
	for _, t := range b.toks {
		if t.Kind != "op" {
			continue
		}
		if t.Value == "{" {
			stack = append(stack, t)
		} else if t.Value == "}" && len(stack) > 0 {
			openTok := stack[len(stack)-1]
			stack = stack[:len(stack)-1]
			if t.Line > openTok.Line {
				b.folds = append(b.folds, positions.RangeOf(openTok.Line, openTok.Col, t.Line, t.EndCol))
			}
		}
	}
}

func slice(source string, first, last lexer.Token) string {
	runes := []rune(source)
	if first.Offset >= len(runes) {
		return ""
	}
	end := last.Offset + len([]rune(last.Value))
	if end > len(runes) {
		end = len(runes)
	}
	text := string(runes[first.Offset:end])
	// emulate " ".join(text.split())
	parts := strings.Fields(text)
	return strings.Join(parts, " ")
}

func paramDetail(source string, toks []lexer.Token, i, closeIdx int) string {
	end := i
	j := i + 1
	if j < closeIdx && toks[j].Kind == "op" && toks[j].Value == ":" {
		depth := 0
		for j < closeIdx {
			t := toks[j]
			if t.Kind == "op" && (t.Value == "(" || t.Value == "[" || t.Value == "{") {
				depth++
			} else if t.Kind == "op" && (t.Value == ")" || t.Value == "]" || t.Value == "}") {
				depth--
			} else if t.Kind == "op" && t.Value == "," && depth == 0 {
				break
			}
			end = j
			j++
		}
	}
	return slice(source, toks[i], toks[end])
}
