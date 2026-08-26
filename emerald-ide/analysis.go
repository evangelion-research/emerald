package main

import (
	"strings"
	"unicode"
)

type Stmt struct {
	Kind      string `json:"kind"`
	Start     int    `json:"start"`
	End       int    `json:"end"`
	StartLine int    `json:"start_line"`
	StartCol  int    `json:"start_col"`
	EndLine   int    `json:"end_line"`
	Error     bool   `json:"error"`
}

type Span struct {
	Start int `json:"start"`
	End   int `json:"end"`
	Group int `json:"group"`
}

type Symbol struct {
	Kind   string `json:"kind"`
	Name   string `json:"name"`
	Detail string `json:"detail"`
	Line   int    `json:"line"`
	Stmt   int    `json:"stmt"`
}

type Obligation struct {
	Kind string `json:"kind"`
	Name string `json:"name"`
	Line int    `json:"line"`
	Stmt int    `json:"stmt"`
}

type Analysis struct {
	Statements  []Stmt       `json:"statements"`
	Spans       []Span       `json:"spans"`
	Symbols     []Symbol     `json:"symbols"`
	Obligations []Obligation `json:"obligations"`
	HasError    bool         `json:"has_error"`
}

func Analyze(text string) Analysis {
	analysis := Analysis{Statements: []Stmt{}, Spans: []Span{}, Symbols: []Symbol{}, Obligations: []Obligation{}}
	analysis.Statements = topLevelStatements(text)
	for i, stmt := range analysis.Statements {
		analysis.Symbols = append(analysis.Symbols, symbolsFor(text, stmt, i)...)
	}
	analysis.Obligations = obligationsFor(text, analysis.Statements)
	analysis.Spans = highlightSpans(text)
	for _, stmt := range analysis.Statements {
		analysis.HasError = analysis.HasError || stmt.Error
	}
	return analysis
}

func topLevelStatements(text string) []Stmt {
	var out []Stmt
	start := -1
	depth := 0
	inString := byte(0)
	comment := false
	line := 0
	col := 0
	startLine, startCol := 0, 0
	flush := func(end int, endLine int) {
		if start < 0 {
			return
		}
		segment := text[start:end]
		trimmed := strings.TrimSpace(segment)
		if trimmed == "" {
			start = -1
			return
		}
		kind := statementKind(trimmed)
		error := strings.Count(segment, "{") != strings.Count(segment, "}") || strings.HasSuffix(trimmed, "{")
		out = append(out, Stmt{Kind: kind, Start: start, End: end, StartLine: startLine, StartCol: startCol, EndLine: endLine, Error: error})
		start = -1
	}
	for i := 0; i < len(text); i++ {
		c := text[i]
		if c == '\n' {
			line++
			col = 0
		} else {
			col++
		}
		if comment {
			if c == '\n' {
				comment = false
			}
			continue
		}
		if inString != 0 {
			if c == '\\' {
				i++
				col++
				continue
			}
			if c == inString {
				inString = 0
			}
			continue
		}
		if c == '#' {
			comment = true
			continue
		}
		if c == '"' || c == '\'' {
			inString = c
			if start < 0 {
				start, startLine, startCol = i, line, col-1
			}
			continue
		}
		if start < 0 {
			if unicode.IsSpace(rune(c)) {
				continue
			}
			start, startLine, startCol = i, line, col-1
		}
		if c == '{' {
			depth++
		}
		if c == '}' {
			if depth > 0 {
				depth--
			}
			if depth == 0 {
				flush(i+1, line)
			}
		}
		if depth == 0 && c == '\n' {
			trimmed := strings.TrimSpace(text[start:i])
			if strings.HasPrefix(trimmed, "import ") || strings.HasPrefix(trimmed, "type ") || strings.HasPrefix(trimmed, "error ") || strings.HasPrefix(trimmed, "dimension ") || strings.HasPrefix(trimmed, "const ") || strings.Contains(trimmed, ":") && strings.Contains(trimmed, "=") {
				flush(i, line-1)
			}
		}
	}
	if start >= 0 {
		flush(len(text), line)
	}
	return out
}

func statementKind(text string) string {
	for prefix, kind := range map[string]string{"import ": "import_statement", "type ": "type_definition", "error ": "error_definition", "dimension ": "dimension_definition", "def ": "function_definition", "const ": "const_declaration"} {
		if strings.HasPrefix(text, prefix) {
			return kind
		}
	}
	if strings.Contains(text, ":") && strings.Contains(text, "=") {
		return "annotated_declaration"
	}
	if strings.Contains(text, "=") {
		return "assignment"
	}
	return "statement"
}

func symbolsFor(text string, stmt Stmt, index int) []Symbol {
	body := text[stmt.Start:stmt.End]
	line := stmt.StartLine
	trim := strings.TrimSpace(body)
	if strings.HasPrefix(trim, "def ") {
		name, rest := declarationName(trim[4:])
		end := strings.Index(rest, "{")
		detail := strings.TrimSpace(rest)
		if end >= 0 {
			detail = strings.TrimSpace(rest[:end])
		}
		return []Symbol{{Kind: "def", Name: name, Detail: detail, Line: line, Stmt: index}}
	}
	if strings.HasPrefix(trim, "type ") {
		name, rest := declarationName(trim[5:])
		return []Symbol{{Kind: "type", Name: name, Detail: "= " + strings.TrimSpace(strings.TrimPrefix(rest, "=")), Line: line, Stmt: index}}
	}
	if strings.HasPrefix(trim, "error ") {
		name, _ := declarationName(trim[6:])
		return []Symbol{{Kind: "error", Name: name, Line: line, Stmt: index}}
	}
	if strings.HasPrefix(trim, "dimension ") {
		var out []Symbol
		for _, token := range strings.Fields(strings.TrimPrefix(trim, "dimension ")) {
			if isIdentifier(token) {
				out = append(out, Symbol{Kind: "dim", Name: token, Line: line, Stmt: index})
			}
		}
		return out
	}
	if colon := strings.Index(trim, ":"); colon > 0 {
		name := strings.TrimSpace(trim[:colon])
		typ := strings.TrimSpace(strings.SplitN(trim[colon+1:], "=", 2)[0])
		kind := "binding"
		if strings.HasPrefix(trim, "const ") {
			kind = "const"
			name = strings.TrimSpace(strings.TrimPrefix(name, "const "))
		}
		return []Symbol{{Kind: kind, Name: name, Detail: ": " + typ, Line: line, Stmt: index}}
	}
	return nil
}

func declarationName(text string) (string, string) {
	text = strings.TrimSpace(text)
	end := 0
	for end < len(text) && (unicode.IsLetter(rune(text[end])) || unicode.IsDigit(rune(text[end])) || text[end] == '_') {
		end++
	}
	return text[:end], strings.TrimSpace(text[end:])
}

func isIdentifier(s string) bool {
	if s == "" {
		return false
	}
	for i, r := range s {
		if !(unicode.IsLetter(r) || r == '_' || i > 0 && unicode.IsDigit(r)) {
			return false
		}
	}
	return true
}

func obligationsFor(text string, statements []Stmt) []Obligation {
	var out []Obligation
	for i, stmt := range statements {
		body := text[stmt.Start:stmt.End]
		if strings.HasPrefix(strings.TrimSpace(body), "def ") && strings.Contains(functionHeader(body), "never") {
			name, _ := declarationName(strings.TrimSpace(strings.TrimPrefix(strings.TrimSpace(body), "def ")))
			out = append(out, Obligation{Kind: "negation fn", Name: name, Line: stmt.StartLine, Stmt: i})
		}
		for offset := 0; ; {
			rel := strings.Index(body[offset:], ": never")
			if rel < 0 {
				break
			}
			at := offset + rel
			name := strings.TrimSpace(body[:at])
			if eq := strings.LastIndex(name, "\n"); eq >= 0 {
				name = strings.TrimSpace(name[eq+1:])
			}
			if colon := strings.LastIndex(name, ":"); colon >= 0 {
				name = strings.TrimSpace(name[:colon])
			}
			if isIdentifier(name) {
				out = append(out, Obligation{Kind: "never binding", Name: name, Line: stmt.StartLine + strings.Count(body[:at], "\n"), Stmt: i})
			}
			offset = at + len(": never")
		}
	}
	return out
}

func functionHeader(text string) string {
	if brace := strings.Index(text, "{"); brace >= 0 {
		return text[:brace]
	}
	return text
}

var keywordGroups = map[string]int{
	"import": 5, "type": 5, "def": 5, "error": 5, "dimension": 5, "const": 5, "let": 5, "match": 5, "return": 5, "pure": 5, "never": 7, "true": 4, "false": 4, "fail": 8,
}

func highlightSpans(text string) []Span {
	var spans []Span
	for i := 0; i < len(text); {
		if text[i] == '#' {
			end := i
			for end < len(text) && text[end] != '\n' {
				end++
			}
			spans = append(spans, Span{i, end, 0})
			i = end
			continue
		}
		if text[i] == '"' || text[i] == '\'' {
			quote := text[i]
			end := i + 1
			for end < len(text) && text[end] != quote {
				if text[end] == '\\' {
					end++
				}
				end++
			}
			if end < len(text) {
				end++
			}
			spans = append(spans, Span{i, end, 1})
			i = end
			continue
		}
		if unicode.IsDigit(rune(text[i])) {
			end := i + 1
			for end < len(text) && (unicode.IsDigit(rune(text[end])) || text[end] == '.') {
				end++
			}
			spans = append(spans, Span{i, end, 3})
			i = end
			continue
		}
		if unicode.IsLetter(rune(text[i])) || text[i] == '_' {
			end := i + 1
			for end < len(text) && (unicode.IsLetter(rune(text[end])) || unicode.IsDigit(rune(text[end])) || text[end] == '_') {
				end++
			}
			if group, ok := keywordGroups[text[i:end]]; ok {
				spans = append(spans, Span{i, end, group})
			}
			i = end
			continue
		}
		if strings.ContainsRune("+-*/=<>!|&^", rune(text[i])) {
			end := i + 1
			for end < len(text) && strings.ContainsRune("+-*/=<>!|&^", rune(text[end])) {
				end++
			}
			spans = append(spans, Span{i, end, 12})
			i = end
			continue
		}
		i++
	}
	return spans
}
