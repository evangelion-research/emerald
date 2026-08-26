package emerald

import (
	"testing"

	treesitter "github.com/tree-sitter/go-tree-sitter"
)

func TestParsesEmerald(t *testing.T) {
	parser := treesitter.NewParser()
	defer parser.Close()

	if err := parser.SetLanguage(Language()); err != nil {
		t.Fatalf("set Emerald language: %v", err)
	}

	tree := parser.Parse([]byte(`
from math import sqrt
type Point = { x: float, y: float }
pure def magnitude(p: Point) -> float {
  return sqrt(p.x * p.x + p.y * p.y)
}
`), nil)
	defer tree.Close()

	if tree.RootNode().HasError() {
		t.Fatalf("unexpected syntax error: %s", tree.RootNode().ToSexp())
	}
}
