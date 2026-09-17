// Command emerald-lsp is the language server for the Emerald programming
// language. It speaks LSP over stdio by default; --tcp --port N is available
// for clients that cannot spawn a process.
package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"strconv"

	"github.com/evangelion-research/emlsp/internal/compiler"
	"github.com/evangelion-research/emlsp/internal/server"
)

func main() { os.Exit(entry()) }

func entry() int {
	var (
		showVersion  bool
		tcp          bool
		port         int
		checkPath    string
		compilerPath string
	)
	flag.Usage = usage
	flag.BoolVar(&showVersion, "version", false, "print the server version and exit")
	flag.BoolVar(&tcp, "tcp", false, "serve over TCP instead of stdio")
	flag.IntVar(&port, "port", 0, "TCP port to listen on (requires --tcp)")
	flag.StringVar(&checkPath, "check", "", "run the per-keystroke check query on FILE and exit")
	flag.StringVar(&compilerPath, "compiler", "", "path to emeraldc (default: $EMERALDC, then PATH)")
	flag.Parse()

	if flag.NArg() > 0 {
		fmt.Fprintf(os.Stderr, "emerald-lsp: unexpected argument %q\n\n", flag.Arg(0))
		usage()
		return 2
	}

	if showVersion {
		fmt.Printf("%s %s\n", server.ServerName, server.ServerVersion)
		return 0
	}

	switch {
	case checkPath != "":
		return runCheck(checkPath, compilerPath)
	case tcp:
		if port <= 0 {
			fmt.Fprintln(os.Stderr, "emerald-lsp: --tcp requires --port N")
			return 2
		}
		server.New().StartTCP("127.0.0.1", port)
		return 0
	default:
		server.New().StartIO()
	}
	return 0
}

// runCheck prints the diagnostics one analysis pass would publish, in the
// same compiler-JSON shape the server consumes, and exits nonzero when the
// document has errors.
func runCheck(path, compilerPath string) int {
	settings := compiler.DefaultSettings()
	if compilerPath != "" {
		settings.CompilerPath = &compilerPath
	}
	result := compiler.Check(path, nil, settings, nil)
	if result.Detail != "" {
		fmt.Fprintf(os.Stderr, "emerald-lsp: %s\n", result.Detail)
		return 1
	}
	out, err := json.MarshalIndent(result.Diagnostics, "", "  ")
	if err != nil {
		fmt.Fprintf(os.Stderr, "emerald-lsp: %v\n", err)
		return 1
	}
	fmt.Println(string(out))
	for _, d := range result.Diagnostics {
		if sev, _ := d["severity"].(string); sev == "error" {
			return 1
		}
	}
	return 0
}

func usage() {
	fmt.Fprint(os.Stderr, `emerald-lsp — language server for the Emerald programming language

Usage:
  emerald-lsp                    serve LSP over stdio (default)
  emerald-lsp --tcp --port N     serve LSP over TCP (debugging clients)
  emerald-lsp --check FILE.rald  run one check pass and print its diagnostics
  emerald-lsp --version          print the server version

Options:
`)

	flag.VisitAll(func(f *flag.Flag) {
		fmt.Fprintf(os.Stderr, "  -%s %s\n      %s\n", f.Name, strconv.Quote(f.DefValue), f.Usage)
	})

	fmt.Fprint(os.Stderr, `
The server needs emeraldc on PATH for type diagnostics; set $EMERALDC or
--compiler if it lives elsewhere. Without it, syntax features still work.
`)
}
