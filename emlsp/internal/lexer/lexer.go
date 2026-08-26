package lexer

import (
	"strings"
	"unicode"
)

// Token is one token with half-open [start,end) span.
type Token struct {
	Kind    string // ident|keyword|int|float|str|fstr|comment|op|error
	Value   string
	Line    int // 0-based
	Col     int // 0-based, characters (runes)
	EndLine int
	EndCol  int
	Offset  int // 0-based character offset into source
}

var keywords = map[string]bool{
	"def": true, "if": true, "elif": true, "else": true, "while": true,
	"for": true, "in": true, "return": true, "and": true, "or": true, "not": true,
	"True": true, "False": true, "None": true,
	"break": true, "continue": true, "pass": true, "type": true, "const": true,
	"match": true, "pure": true, "partial": true, "import": true, "from": true,
	"as": true, "dim": true, "error": true, "try": true, "catch": true,
}

var operators = []string{
	"//=", "**=",
	"==", "!=", "<=", ">=", "->", "=>", "|>", ">>", "<<", "//", "**",
	"+=", "-=", "*=", "/=",
	"{", "}", "(", ")", "[", "]", ",", ".", ":", ";", "=",
	"|", "&", "^", "+", "-", "*", "/", "%", "<", ">", "?",
}

// Tokenize never raises and never stops early.
func Tokenize(src string) []Token {
	runes := []rune(src)
	var out []Token
	i, line, col := 0, 0, 0
	n := len(runes)

	tok := func(kind string, start, sl, sc int) Token {
		return Token{
			Kind:    kind,
			Value:   string(runes[start:i]),
			Line:    sl,
			Col:     sc,
			EndLine: line,
			EndCol:  col,
			Offset:  start,
		}
	}

	for i < n {
		c := runes[i]
		if c == '\n' {
			i++
			line++
			col = 0
			continue
		}
		if c == ' ' || c == '\t' || c == '\r' {
			i++
			col++
			continue
		}
		start, sl, sc := i, line, col

		if c == '#' {
			for i < n && runes[i] != '\n' {
				i++
				col++
			}
			out = append(out, tok("comment", start, sl, sc))
			continue
		}

		// string / f-string
		quote := rune(0)
		prefix := 0
		if c == '"' || c == '\'' {
			quote = c
			prefix = 1
		} else if c == 'f' && i+1 < n && (runes[i+1] == '"' || runes[i+1] == '\'') {
			quote = runes[i+1]
			prefix = 2
		}
		if quote != 0 {
			i += prefix
			col += prefix
			closed := false
			i, line, col, closed = stringBody(runes, i, line, col, quote)
			if !closed {
				// clip to newline like Python's _stop_at_newline
				end := strings.Index(string(runes[start:]), "\n")
				var newEnd int
				if end == -1 {
					newEnd = n
				} else {
					newEnd = start + end
				}
				// recompute col for clipped end
				i = newEnd
				// col was at start's col + (prefix + body); now adjust to clipped length
				// But Python computes col + (end-start) from start's col. Let's compute directly.
				// Simpler: col = sc + (newEnd - start)
				col = sc + (newEnd - start)
				// line stays same (no newline consumed)
			}
			kind := "error"
			if closed {
				if prefix == 2 {
					kind = "fstr"
				} else {
					kind = "str"
				}
			}
			out = append(out, tok(kind, start, sl, sc))
			continue
		}

		if c == '_' || unicode.IsLetter(c) {
			for i < n && (runes[i] == '_' || unicode.IsLetter(runes[i]) || unicode.IsDigit(runes[i])) {
				i++
				col++
			}
			word := string(runes[start:i])
			kind := "ident"
			if keywords[word] {
				kind = "keyword"
			}
			out = append(out, tok(kind, start, sl, sc))
			continue
		}

		if unicode.IsDigit(c) {
			kind := "int"
			i, col, kind = scanNumber(runes, i, col)
			out = append(out, tok(kind, start, sl, sc))
			continue
		}

		matched := false
		for _, op := range operators {
			rop := []rune(op)
			if i+len(rop) <= n && equalRunes(runes[i:i+len(rop)], rop) {
				i += len(rop)
				col += len(rop)
				out = append(out, tok("op", start, sl, sc))
				matched = true
				break
			}
		}
		if matched {
			continue
		}
		// unknown character
		i++
		col++
		out = append(out, tok("error", start, sl, sc))
	}
	return out
}

func equalRunes(a, b []rune) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

func stringBody(runes []rune, i, line, col int, quote rune) (int, int, int, bool) {
	n := len(runes)
	for i < n && runes[i] != quote {
		if runes[i] == '\\' && i+1 < n {
			i += 2
			col += 2
			continue
		}
		if runes[i] == '\n' {
			i++
			line++
			col = 0
		} else {
			i++
			col++
		}
	}
	if i < n && runes[i] == quote {
		return i + 1, line, col + 1, true
	}
	return i, line, col, false
}

func scanNumber(runes []rune, i, col int) (int, int, string) {
	n := len(runes)
	kind := "int"
	for i < n && unicode.IsDigit(runes[i]) {
		i++
		col++
	}
	if i < n && runes[i] == '.' {
		// not a float if followed by ident start: 1.foo
		if !(i+1 < n && (unicode.IsLetter(runes[i+1]) || runes[i+1] == '_')) {
			kind = "float"
			i++
			col++
			for i < n && unicode.IsDigit(runes[i]) {
				i++
				col++
			}
		}
	}
	if i < n && (runes[i] == 'e' || runes[i] == 'E') {
		saveI, saveCol := i, col
		i++
		col++
		if i < n && (runes[i] == '+' || runes[i] == '-') {
			i++
			col++
		}
		if i < n && unicode.IsDigit(runes[i]) {
			kind = "float"
			for i < n && unicode.IsDigit(runes[i]) {
				i++
				col++
			}
		} else {
			i, col = saveI, saveCol
		}
	}
	return i, col, kind
}

// Significant drops comments
func Significant(tokens []Token) []Token {
	var out []Token
	for _, t := range tokens {
		if t.Kind != "comment" {
			out = append(out, t)
		}
	}
	return out
}

// TokenAt finds token containing position or ending exactly at it
func TokenAt(tokens []Token, line, col int) *Token {
	var touching *Token
	for idx := range tokens {
		t := &tokens[idx]
		if t.Line > line || (t.Line == line && t.Col > col) {
			break
		}
		if (t.Line < line || (t.Line == line && t.Col <= col)) && (line < t.EndLine || (line == t.EndLine && col < t.EndCol)) {
			// contains
			if t.Line <= line && line <= t.EndLine {
				// need tuple compare: (t.line,t.col) <= (line,col) < (t.end_line,t.end_col)
				// already checked lower bound, upper bound is col < endCol when same line
				// but for multi-line need proper. Simpler check:
				if isBeforeOrEq(t.Line, t.Col, line, col) && isBefore(line, col, t.EndLine, t.EndCol) {
					return t
				}
			}
		}
		if t.EndLine == line && t.EndCol == col {
			touching = t
		}
	}
	return touching
}

func isBeforeOrEq(l1, c1, l2, c2 int) bool {
	if l1 < l2 {
		return true
	}
	if l1 == l2 && c1 <= c2 {
		return true
	}
	return false
}
func isBefore(l1, c1, l2, c2 int) bool {
	if l1 < l2 {
		return true
	}
	if l1 == l2 && c1 < c2 {
		return true
	}
	return false
}
