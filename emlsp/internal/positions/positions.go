package positions

import (
	"net/url"
	"os"
	"path/filepath"
	"runtime"
	"strings"
)

// lsp-like types minimal
type Position struct {
	Line      int `json:"line"`
	Character int `json:"character"`
}
type Range struct {
	Start Position `json:"start"`
	End   Position `json:"end"`
}

func RangeOf(sl, sc, el, ec int) Range {
	return Range{Start: Position{Line: sl, Character: sc}, End: Position{Line: el, Character: ec}}
}

func Contains(r Range, p Position) bool {
	start := p.Line > r.Start.Line || (p.Line == r.Start.Line && p.Character >= r.Start.Character)
	end := p.Line < r.End.Line || (p.Line == r.End.Line && p.Character <= r.End.Character)
	return start && end
}

func SpanTokens(first, last interface {
	GetLine() int; GetCol() int; GetEndLine() int; GetEndCol() int
}) Range {
	// not used directly; outline will use helper
	return Range{}
}

// URI <-> path

func URIToPath(uri string) (string, bool) {
	u, err := url.Parse(uri)
	if err != nil || u.Scheme != "file" {
		return "", false
	}
	p, err := url.PathUnescape(u.Path)
	if err != nil {
		p = u.Path
	}
	if runtime.GOOS == "windows" && strings.HasPrefix(p, "/") && len(p) > 2 && p[2] == ':' {
		p = p[1:]
	}
	return Canonical(p), true
}

func PathToURI(path string) string {
	c := Canonical(path)
	// filepath to URL
	// Use url.PathEscape but preserve slashes
	// Simplest: use filepath -> url.URL
	u := url.URL{Scheme: "file", Path: filepath.ToSlash(c)}
	return u.String()
}

func Canonical(path string) string {
	if real, err := filepath.EvalSymlinks(path); err == nil {
		return real
	}
	if abs, err := filepath.Abs(path); err == nil {
		return abs
	}
	return path
}

func ByteColToCharCol(lineText string, byteCol int) int {
	byteOffset := byteCol - 1
	if byteOffset < 0 {
		byteOffset = 0
	}
	encoded := []byte(lineText)
	if byteOffset >= len(encoded) {
		return len([]rune(lineText))
	}
	return len([]rune(string(encoded[:byteOffset])))
}

func LineText(lines []string, line int) string {
	if 0 <= line && line < len(lines) {
		return lines[line]
	}
	return ""
}

// Helpers for outline/positions bridge with Token

func Span(firstLine, firstCol, lastEndLine, lastEndCol int) Range {
	return RangeOf(firstLine, firstCol, lastEndLine, lastEndCol)
}

func TokenRange(line, col, endLine, endCol int) Range {
	return RangeOf(line, col, endLine, endCol)
}

// For diagnostics
func FilePathFromURI(uri string) string {
	if p, ok := URIToPath(uri); ok {
		return p
	}
	return uri
}

// Extra util: expand ~ and dedup
func ExpandPath(p string) string {
	if strings.HasPrefix(p, "~/") || p == "~" {
		home, _ := os.UserHomeDir()
		if strings.HasPrefix(p, "~/") {
			return filepath.Join(home, p[2:])
		}
		return home
	}
	return p
}
