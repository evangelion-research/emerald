package main

import "testing"

func TestSemverPrecedence(t *testing.T) {
	values := []string{"1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-beta", "1.0.0"}
	for i := range values[:len(values)-1] {
		left, _ := parseVersion(values[i])
		right, _ := parseVersion(values[i+1])
		if cmp(left, right) >= 0 {
			t.Fatalf("%s should precede %s", left, right)
		}
	}
}

func TestConstraints(t *testing.T) {
	c, _ := parseConstraint("^1.2.3")
	ok, _ := parseVersion("1.9.0")
	bad, _ := parseVersion("2.0.0")
	if !c.allows(ok) || c.allows(bad) {
		t.Fatal("caret range was evaluated incorrectly")
	}
}
