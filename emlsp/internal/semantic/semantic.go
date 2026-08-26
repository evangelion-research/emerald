package semantic

import (
	"github.com/evangelion-research/emlsp/internal/language"
	"github.com/evangelion-research/emlsp/internal/lexer"
)

var TokenTypes = []string{
	"namespace",
	"type",
	"parameter",
	"variable",
	"property",
	"function",
	"keyword",
	"comment",
	"string",
	"number",
	"operator",
}
var TokenModifiers = []string{"declaration", "definition", "readonly", "defaultLibrary"}

var Legend = struct {
	TokenTypes     []string `json:"tokenTypes"`
	TokenModifiers []string `json:"tokenModifiers"`
}{TokenTypes, TokenModifiers}

var typeIndex = func() map[string]int {
	m := map[string]int{}
	for i, n := range TokenTypes {
		m[n] = i
	}
	return m
}()
var modBit = func() map[string]int {
	m := map[string]int{}
	for i, n := range TokenModifiers {
		m[n] = 1 << i
	}
	return m
}()

var simple = map[string]string{
	"str":     "string",
	"fstr":    "string",
	"int":     "number",
	"float":   "number",
	"op":      "operator",
	"keyword": "keyword",
}

func Encode(tokens []lexer.Token, lines []string, modules map[string]bool) []int {
	var data []int
	prevLine, prevCol := 0, 0
	for _, e := range Classify(tokens, lines, modules) {
		dl := e[0] - prevLine
		dc := e[1]
		if dl == 0 {
			dc = e[1] - prevCol
		}
		data = append(data, dl, dc, e[2], e[3], e[4])
		prevLine = e[0]
		prevCol = e[1]
	}
	return data
}

// Classify returns (line,col,length,typeIndex,modsBits)
func Classify(tokens []lexer.Token, lines []string, modules map[string]bool) [][5]int {
	var out [][5]int
	sig := lexer.Significant(tokens)
	idx := -1
	for _, token := range tokens {
		if token.Kind == "comment" {
			mods := 0
			for _, s := range spans(token, lines, typeIndex["comment"], mods) {
				out = append(out, s)
			}
			continue
		}
		idx++
		if token.Kind == "error" {
			continue
		}
		kind, mods := kindOf(token, sig, idx, modules)
		if kind == "" {
			continue
		}
		ti := typeIndex[kind]
		bits := 0
		for _, m := range mods {
			bits |= modBit[m]
		}
		for _, s := range spans(token, lines, ti, bits) {
			out = append(out, s)
		}
	}
	return out
}

func prevTok(sig []lexer.Token, i int) *lexer.Token {
	if 0 < i && i <= len(sig) {
		return &sig[i-1]
	}
	return nil
}
func nextTok(sig []lexer.Token, i int) *lexer.Token {
	if 0 <= i && i+1 < len(sig) {
		return &sig[i+1]
	}
	return nil
}

func kindOf(token lexer.Token, sig []lexer.Token, i int, modules map[string]bool) (string, []string) {
	if s, ok := simple[token.Kind]; ok {
		return s, nil
	}
	if token.Kind != "ident" {
		return "", nil
	}
	prev := prevTok(sig, i)
	nxt := nextTok(sig, i)
	var prevKw string
	if prev != nil && prev.Kind == "keyword" {
		prevKw = prev.Value
	}
	if prevKw == "def" {
		return "function", []string{"declaration", "definition"}
	}
	if prevKw == "type" || prevKw == "error" || prevKw == "dim" {
		return "type", []string{"declaration", "definition"}
	}
	if prevKw == "import" || prevKw == "from" {
		return "namespace", nil
	}
	if prev != nil && prev.Kind == "op" && prev.Value == "." {
		before := prevTok(sig, i-1)
		if before != nil && modules[before.Value] {
			if isCall(nxt) {
				return "function", nil
			}
			return "property", nil
		}
		return "property", nil
	}
	if prevKw == "const" {
		return "variable", []string{"declaration", "readonly"}
	}
	if modules[token.Value] {
		return "namespace", nil
	}
	if _, ok := language.TypeAtoms[token.Value]; ok {
		return "type", nil
	}
	if _, ok := language.Constants[token.Value]; ok {
		return "variable", []string{"readonly", "defaultLibrary"}
	}
	if _, ok := language.Builtins[token.Value]; ok {
		return "function", []string{"defaultLibrary"}
	}
	if isCall(nxt) {
		return "function", nil
	}
	return "variable", nil
}

func isCall(nxt *lexer.Token) bool {
	return nxt != nil && nxt.Kind == "op" && nxt.Value == "("
}

func spans(token lexer.Token, lines []string, typeIndex, mods int) [][5]int {
	if token.EndLine == token.Line {
		return [][5]int{{token.Line, token.Col, token.EndCol - token.Col, typeIndex, mods}}
	}
	var out [][5]int
	for line := token.Line; line <= token.EndLine; line++ {
		text := ""
		if line < len(lines) {
			text = lines[line]
		}
		start := 0
		if line == token.Line {
			start = token.Col
		}
		end := len([]rune(text))
		if line == token.EndLine {
			end = token.EndCol
		}
		if end > start {
			out = append(out, [5]int{line, start, end - start, typeIndex, mods})
		}
	}
	return out
}
