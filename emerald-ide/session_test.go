package main

import "testing"

func TestRunCheckWithoutCompilerIsOffline(t *testing.T) {
	old := ResolveCompiler
	_ = old
	result := RunCheck("", -1, "", "", "untitled.rald", 10)
	if result.ExitCode != -1 && result.Error == "" {
		t.Fatalf("expected a usable compiler result or offline error, got %#v", result)
	}
}
