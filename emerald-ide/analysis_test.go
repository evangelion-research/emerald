package main

import "testing"

const testSource = `import math

type Shape = Circle | Square

def contradiction(p: int) -> never pure {
    return fail("unreachable")
}

x: never = contradiction(1)
`

func TestAnalyzeFindsStatementsSymbolsAndObligations(t *testing.T) {
	a := Analyze(testSource)
	if len(a.Statements) != 4 {
		t.Fatalf("got %d statements, want 4: %#v", len(a.Statements), a.Statements)
	}
	if a.Statements[0].Kind != "import_statement" || a.Statements[1].Kind != "type_definition" {
		t.Fatalf("unexpected statement kinds: %#v", a.Statements)
	}
	if len(a.Symbols) != 3 {
		t.Fatalf("got %d symbols, want 3: %#v", len(a.Symbols), a.Symbols)
	}
	if len(a.Obligations) != 2 {
		t.Fatalf("got %d obligations, want 2: %#v", len(a.Obligations), a.Obligations)
	}
	if a.Spans == nil || len(a.Spans) == 0 {
		t.Fatal("expected syntax highlight spans")
	}
}

func TestAnalyzeDetectsUnbalancedBraces(t *testing.T) {
	a := Analyze("def broken() {\n")
	if !a.HasError || len(a.Statements) != 1 || !a.Statements[0].Error {
		t.Fatalf("expected parse error, got %#v", a)
	}
}

func TestPositionAtUsesUTF8ByteOffsets(t *testing.T) {
	text := "λ = 1\nnext"
	line, col := positionAt(text, len("λ = "))
	if line != 0 || col != len("λ = ") {
		t.Fatalf("got %d:%d, want 0:%d", line, col, len("λ = "))
	}
	line, col = positionAt(text, len("λ = 1\n"))
	if line != 1 || col != 0 {
		t.Fatalf("got %d:%d, want 1:0", line, col)
	}
}

func TestDiagJSONAcceptsColumnAliases(t *testing.T) {
	d := diagFromJSON(map[string]any{"message": "bad type", "line": float64(4), "col": float64(9)})
	if d.Message != "bad type" || d.Line != 4 || d.Column != 9 {
		t.Fatalf("unexpected diagnostic: %#v", d)
	}
}

func TestSessionRetractsOnEdit(t *testing.T) {
	s := NewSession()
	s.analysis = Analyze(testSource)
	s.text = testSource
	s.locus = len(s.analysis.Statements) - 1
	s.status = "idle"
	s.retractToEditLocked(s.analysis.Statements[1].Start)
	if s.locus != 0 {
		t.Fatalf("got locus %d, want 0", s.locus)
	}
}
