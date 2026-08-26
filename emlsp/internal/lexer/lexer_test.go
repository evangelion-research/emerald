package lexer

import "testing"

func kinds(src string) [][2]string {
	var out [][2]string
	for _, t := range Tokenize(src) {
		out = append(out, [2]string{t.Kind, t.Value})
	}
	return out
}

func equalKinds(a, b [][2]string) bool {
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

func TestKeywordsAndIdents(t *testing.T) {
	exp := [][2]string{{"keyword", "def"}, {"ident", "f"}, {"op", "("}, {"ident", "x"}, {"op", ")"}}
	if got := kinds("def f(x)"); !equalKinds(got, exp) {
		t.Fatalf("got %v want %v", got, exp)
	}
}

func TestUnterminatedStringRecovers(t *testing.T) {
	toks := Tokenize("x = \"oops\ny = 1")
	if toks[2].Kind != "error" {
		t.Fatalf("expected error token, got %v", toks[2].Kind)
	}
	found := false
	for _, tok := range toks {
		if tok.Kind == "ident" && tok.Value == "y" {
			found = true
		}
	}
	if !found {
		t.Fatal("expected y ident after unterminated string")
	}
}

func TestNumbers(t *testing.T) {
	exp := [][2]string{{"int", "1"}, {"float", "2.5"}, {"float", "3."}, {"float", "1e-3"}}
	if got := kinds("1 2.5 3. 1e-3"); !equalKinds(got, exp) {
		t.Fatalf("got %v", got)
	}
}

func TestFieldAccessOnIntIsNotFloat(t *testing.T) {
	exp := [][2]string{{"int", "1"}, {"op", "."}, {"ident", "foo"}}
	if got := kinds("1.foo"); !equalKinds(got, exp) {
		t.Fatalf("got %v", got)
	}
}

func TestPositionsCountCharactersNotBytes(t *testing.T) {
	toks := Tokenize("x = \"é\" + y")
	var plus *Token
	for i := range toks {
		if toks[i].Value == "+" {
			plus = &toks[i]
			break
		}
	}
	if plus == nil || plus.Col != 8 {
		t.Fatalf("plus col = %v want 8", plus)
	}
}
