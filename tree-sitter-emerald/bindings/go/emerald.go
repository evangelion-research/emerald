// Package emerald exposes the Emerald Tree-sitter language for Go programs.
package emerald

// #cgo CFLAGS: -std=c11 -fPIC -I${SRCDIR}/../../src
// #include "../../src/parser.c"
import "C"

import (
	"unsafe"

	treesitter "github.com/tree-sitter/go-tree-sitter"
)

// Language returns the Emerald grammar for use with a Tree-sitter parser.
func Language() *treesitter.Language {
	return treesitter.NewLanguage(unsafe.Pointer(C.tree_sitter_emerald()))
}
