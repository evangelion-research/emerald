# Tree-sitter Emerald

A focused Tree-sitter grammar for the Emerald (`.rald`) language, with Go
bindings as its public integration surface.

Tree-sitter grammars are written in JavaScript and generated into C because
that is the format required by the Tree-sitter runtime. Go consumers use the
binding in `bindings/go` directly; they do not need Node.js at runtime.

## Develop the grammar

```sh
npm install
npm run generate
npm test
go test ./...
```

`grammar.js` is the only parser source. Files under `src/` are generated and
must be regenerated after every grammar change.

## Use from Go

```go
package main

import (
    treesitter "github.com/tree-sitter/go-tree-sitter"
    emerald "github.com/evangelion-research/tree-sitter-emerald/bindings/go"
)

func main() {
    parser := treesitter.NewParser()
    defer parser.Close()
    _ = parser.SetLanguage(emerald.Language())
    tree := parser.Parse([]byte("const answer = 42"), nil)
    defer tree.Close()
}
```

The grammar covers Emerald declarations, structural types, functions and
control flow, patterns, collections and comprehensions, lambdas, errors,
strings, postfix expressions, and the documented operator precedence.
