package diagnostics

import (
	"sort"

	"github.com/evangelion-research/emlsp/internal/lexer"
	"github.com/evangelion-research/emlsp/internal/outline"
	"github.com/evangelion-research/emlsp/internal/positions"
)

const Source = "emeraldc"
const UnusedSource = "emerald-lsp"
const UnusedCode = "E_UNUSED"

const (
	SeverityError       = 1
	SeverityWarning     = 2
	SeverityInformation = 3
	SeverityHint        = 4
)

type Diagnostic struct {
	Range    positions.Range        `json:"range"`
	Message  string                 `json:"message"`
	Severity int                    `json:"severity"`
	Code     *string                `json:"code,omitempty"`
	Source   *string                `json:"source,omitempty"`
	Data     map[string]interface{} `json:"data,omitempty"`
}

var severityMap = map[string]int{
	"error":   SeverityError,
	"warning": SeverityWarning,
	"note":    SeverityInformation,
	"hint":    SeverityHint,
}

func UnusedDiagnostics(o *outline.Outline) []Diagnostic {
	topLevel := map[*outline.Definition]bool{}
	for _, d := range o.Symbols {
		topLevel[d] = true
	}
	parents := definitionParents(o)
	// assign parent ids for grouping
	parentID := map[*outline.Definition]int{}
	nextID := 1
	for _, d := range o.Definitions {
		p := parents[d]
		if p == nil {
			parentID[d] = 0
		} else {
			if _, ok := parentID[p]; !ok {
				parentID[p] = nextID
				nextID++
			}
			// For definition's group, use parent's id
			// But we need mapping for definition -> parent id
			// So ensure entry for d via its parent id
		}
	}
	getParentKey := func(d *outline.Definition) int {
		p := parents[d]
		if p == nil {
			return 0
		}
		if id, ok := parentID[p]; ok {
			return id
		}
		// if parent not yet assigned (parent is not in definitions? should be)
		parentID[p] = nextID
		nextID++
		return parentID[p]
	}

	type gkey struct {
		parent int
		name   string
	}
	groups := map[gkey][]*outline.Definition{}
	for _, d := range o.Definitions {
		if !isUnusedCandidate(d, topLevel) {
			continue
		}
		k := gkey{parent: getParentKey(d), name: d.Name}
		groups[k] = append(groups[k], d)
	}
	if len(groups) == 0 {
		return nil
	}
	declarations := declarationTokens(o)
	declSet := map[string]bool{}
	for _, dt := range declarations {
		declSet[itoa(dt.Line)+","+itoa(dt.Col)+":"+itoa(dt.Offset)] = true
	}
	used := map[gkey]bool{}
	for _, tok := range lexer.Significant(o.Tokens) {
		if tok.Kind != "ident" {
			continue
		}
		key := itoa(tok.Line) + "," + itoa(tok.Col) + ":" + itoa(tok.Offset)
		if declSet[key] {
			continue
		}
		def := o.Resolve(tok.Value, tok.Offset)
		if def == nil {
			continue
		}
		k := gkey{parent: getParentKey(def), name: def.Name}
		if _, ok := groups[k]; ok {
			used[k] = true
		}
	}
	var result []Diagnostic
	for k, defs := range groups {
		if used[k] {
			continue
		}
		sort.Slice(defs, func(i, j int) bool {
			if defs[i].SelectionRange.Start.Line != defs[j].SelectionRange.Start.Line {
				return defs[i].SelectionRange.Start.Line < defs[j].SelectionRange.Start.Line
			}
			return defs[i].SelectionRange.Start.Character < defs[j].SelectionRange.Start.Character
		})
		def := defs[0]
		msg := ""
		if def.IsImport {
			msg = `imported and not used: "` + def.Name + `"`
		} else {
			msg = "declared and not used: " + def.Name
		}
		code := UnusedCode
		src := UnusedSource
		rng := bindingRange(o, def)
		result = append(result, Diagnostic{
			Range:    rng,
			Message:  msg,
			Severity: SeverityError,
			Code:     &code,
			Source:   &src,
			Data:     map[string]interface{}{"kind": "unused"},
		})
	}
	sort.Slice(result, func(i, j int) bool {
		if result[i].Range.Start.Line != result[j].Range.Start.Line {
			return result[i].Range.Start.Line < result[j].Range.Start.Line
		}
		return result[i].Range.Start.Character < result[j].Range.Start.Character
	})
	return result
}

func isUnusedCandidate(d *outline.Definition, topLevel map[*outline.Definition]bool) bool {
	if d.IsImport {
		return true
	}
	if topLevel[d] || d.IsParameter {
		return false
	}
	return d.Kind == outline.SymbolKindVariable || d.Kind == outline.SymbolKindConstant || d.Kind == outline.SymbolKindFunction
}

func definitionParents(o *outline.Outline) map[*outline.Definition]*outline.Definition {
	parents := map[*outline.Definition]*outline.Definition{}
	var visit func(defs []*outline.Definition, parent *outline.Definition)
	visit = func(defs []*outline.Definition, parent *outline.Definition) {
		for _, d := range defs {
			parents[d] = parent
			visit(d.Children, d)
		}
	}
	visit(o.Symbols, nil)
	for _, d := range o.Definitions {
		if _, ok := parents[d]; !ok {
			parents[d] = nil
		}
	}
	return parents
}

func declarationTokens(o *outline.Outline) []lexer.Token {
	var importRanges []positions.Range
	for _, d := range o.Definitions {
		if d.IsImport {
			importRanges = append(importRanges, d.Range)
		}
	}
	starts := map[string]bool{}
	for _, d := range o.Definitions {
		if !d.IsImport {
			k := itoa(d.SelectionRange.Start.Line) + "," + itoa(d.SelectionRange.Start.Character)
			starts[k] = true
		}
	}
	var out []lexer.Token
	for _, tok := range lexer.Significant(o.Tokens) {
		posKey := itoa(tok.Line) + "," + itoa(tok.Col)
		inImport := false
		for _, rng := range importRanges {
			if (rng.Start.Line < tok.Line || (rng.Start.Line == tok.Line && rng.Start.Character <= tok.Col)) &&
				(tok.Line < rng.End.Line || (tok.Line == rng.End.Line && tok.Col <= rng.End.Character)) {
				inImport = true
				break
			}
		}
		if inImport || starts[posKey] {
			out = append(out, tok)
		}
	}
	return out
}

func itoa(i int) string {
	if i == 0 {
		return "0"
	}
	neg := false
	if i < 0 {
		neg = true
		i = -i
	}
	s := ""
	for i > 0 {
		s = string(rune('0'+i%10)) + s
		i /= 10
	}
	if neg {
		s = "-" + s
	}
	return s
}

func bindingRange(o *outline.Outline, def *outline.Definition) positions.Range {
	if def.IsImport {
		for _, tok := range reverseSignificant(o.Tokens) {
			if tok.Kind == "ident" && tok.Value == def.Name &&
				def.Range.Start.Line <= tok.Line && tok.Line <= def.Range.End.Line &&
				(tok.Line != def.Range.Start.Line || tok.Col >= def.Range.Start.Character) &&
				(tok.Line != def.Range.End.Line || tok.EndCol <= def.Range.End.Character) {
				return positions.TokenRange(tok.Line, tok.Col, tok.EndLine, tok.EndCol)
			}
		}
	}
	return def.SelectionRange
}

func reverseSignificant(tokens []lexer.Token) []lexer.Token {
	sig := lexer.Significant(tokens)
	for i, j := 0, len(sig)-1; i < j; i, j = i+1, j-1 {
		sig[i], sig[j] = sig[j], sig[i]
	}
	return sig
}

func ToLSP(diag map[string]interface{}, sourceLines map[string][]string) *Diagnostic {
	file, ok := diag["file"].(string)
	if !ok {
		return nil
	}
	lineVal, ok := diag["line"]
	if !ok {
		return nil
	}
	var line int
	switch v := lineVal.(type) {
	case int:
		line = v
	case int64:
		line = int(v)
	case float64:
		line = int(v)
	default:
		return nil
	}
	lineno := line - 1
	if lineno < 0 {
		lineno = 0
	}
	lines, ok := sourceLines[file]
	var text string
	if ok {
		if lineno < len(lines) {
			text = lines[lineno]
		}
	} else {
		if q, ok := diag["source_line"].(string); ok {
			lines = []string{q}
			text = q
		} else {
			text = ""
		}
	}
	colVal := diag["column"]
	col := 1
	switch v := colVal.(type) {
	case int:
		col = v
	case int64:
		col = int(v)
	case float64:
		col = int(v)
	}
	start := positions.ByteColToCharCol(text, col)
	end := tokenEnd(text, start)
	message := "error"
	if m, ok := diag["message"].(string); ok && m != "" {
		message = m
	}
	if exp, ok := diag["expected"].(string); ok {
		if act, ok := diag["actual"].(string); ok {
			message = message + "\n  expected: " + exp + "\n  actual:   " + act
		}
	}
	if notes, ok := diag["notes"].([]interface{}); ok {
		for _, n := range notes {
			if m, ok := n.(map[string]interface{}); ok {
				label := "note"
				if l, ok := m["label"].(string); ok {
					label = l
				}
				val := ""
				if v, ok := m["value"].(string); ok {
					val = v
				} else if v, ok := m["value"]; ok {
					val = toString(v)
				}
				message += "\n  " + label + ": " + val
			}
		}
	}
	severityStr, _ := diag["severity"].(string)
	if severityStr == "" {
		severityStr = "error"
	}
	sev := severityMap[severityStr]
	if sev == 0 {
		sev = SeverityError
	}
	var code *string
	if c, ok := diag["code"].(string); ok {
		code = &c
	}
	src := Source
	var data map[string]interface{}
	if k, ok := diag["kind"].(string); ok && k != "" {
		data = map[string]interface{}{"kind": k}
	}
	return &Diagnostic{
		Range:    positions.RangeOf(lineno, start, lineno, end),
		Message:  message,
		Severity: sev,
		Code:     code,
		Source:   &src,
		Data:     data,
	}
}

func toString(v interface{}) string {
	switch x := v.(type) {
	case string:
		return x
	default:
		return ""
	}
}

func tokenEnd(text string, start int) int {
	runes := []rune(text)
	if start >= len(runes) {
		if start+1 > len(runes) {
			return len(runes)
		}
		return start + 1
	}
	sliced := string(runes[start:])
	toks := lexer.Tokenize(sliced)
	if len(toks) > 0 && toks[0].Offset == 0 && toks[0].EndLine == 0 {
		return start + toks[0].EndCol
	}
	return start + 1
}

func GroupByURI(diagnostics []map[string]interface{}, sourceLines map[string][]string) map[string][]Diagnostic {
	out := map[string][]Diagnostic{}
	for _, raw := range diagnostics {
		converted := ToLSP(raw, sourceLines)
		if converted == nil {
			continue
		}
		file, _ := raw["file"].(string)
		if file == "<stdlib>" {
			continue
		}
		uri := positions.PathToURI(file)
		out[uri] = append(out[uri], *converted)
	}
	return out
}
