package positions

import "testing"

func TestByteColToCharColASCII(t *testing.T) {
	if c := ByteColToCharCol("abc", 1); c != 0 {
		t.Fatalf("got %d want 0", c)
	}
	if c := ByteColToCharCol("abc", 3); c != 2 {
		t.Fatalf("got %d want 2", c)
	}
}

func TestNonAsciiShifts(t *testing.T) {
	line := "print(\"héllo\", x)"
	// find byte index of x
	bs := []byte(line)
	idx := -1
	for i, b := range bs {
		if b == 'x' {
			idx = i
			break
		}
	}
	byteCol := idx + 1
	// char index of x
	runes := []rune(line)
	want := -1
	for i, r := range runes {
		if r == 'x' {
			want = i
			break
		}
	}
	if got := ByteColToCharCol(line, byteCol); got != want {
		t.Fatalf("got %d want %d", got, want)
	}
}

func TestURIRoundTrip(t *testing.T) {
	uri := PathToURI("/tmp/a b/f.rald")
	if p, ok := URIToPath(uri); !ok || p == "" {
		t.Fatalf("round trip failed: %s -> %v", uri, p)
	}
}
