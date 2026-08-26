package modules

import (
	"os"
	"path/filepath"
	"sort"
	"strings"

	"github.com/evangelion-research/emlsp/internal/outline"
)

const Suffix = ".rald"

type Resolved struct {
	Path      string
	Root      string
	Ambiguous bool
}

func StdlibRoot(compiler *string) *string {
	if env := os.Getenv("EMERALD_STDLIB"); env != "" {
		return &env
	}
	if compiler != nil {
		exe := filepath.Dir(*compiler)
		// resolve symlinks for exe dir
		if real, err := filepath.EvalSymlinks(exe); err == nil {
			exe = real
		}
		candidates := []string{filepath.Join(exe, "stdlib"), filepath.Join(filepath.Dir(exe), "stdlib")}
		for _, c := range candidates {
			if info, err := os.Stat(c); err == nil && info.IsDir() {
				return &c
			}
		}
	}
	return nil
}

func SrcRoot(start string) *string {
	here := start
	if fi, err := os.Stat(start); err == nil && !fi.IsDir() {
		here = filepath.Dir(start)
	}
	// walk up
	dir := here
	for {
		if filepath.Base(dir) == "src" {
			return &dir
		}
		candidate := filepath.Join(dir, "src")
		if info, err := os.Stat(candidate); err == nil && info.IsDir() {
			return &candidate
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			break
		}
		dir = parent
	}
	return nil
}

func RootsFor(importer string, includePaths []string, compiler *string) []string {
	var roots []string
	roots = append(roots, filepath.Dir(importer))
	if src := SrcRoot(importer); src != nil {
		roots = append(roots, *src)
	}
	roots = append(roots, includePaths...)
	if std := StdlibRoot(compiler); std != nil {
		roots = append(roots, *std)
	}
	// dedup by resolved path
	seen := map[string]bool{}
	var out []string
	for _, r := range roots {
		key := r
		if abs, err := filepath.EvalSymlinks(r); err == nil {
			key = abs
		} else if abs, err := filepath.Abs(r); err == nil {
			key = abs
		}
		if !seen[key] {
			seen[key] = true
			out = append(out, r)
		}
	}
	return out
}

func Resolve(modulePath, importer string, includePaths []string, compiler *string) *Resolved {
	nested := filepath.Join(strings.Split(modulePath, ".")...)
	nested += Suffix
	flat := modulePath + Suffix
	var spellings []string
	if nested == flat {
		spellings = []string{nested}
	} else {
		spellings = []string{nested, flat}
	}
	for _, root := range RootsFor(importer, includePaths, compiler) {
		var found []string
		var hits []string
		for _, s := range spellings {
			h := filepath.Join(root, s)
			hits = append(hits, h)
			if _, err := os.Stat(h); err == nil {
				found = append(found, h)
			}
		}
		if len(found) > 0 {
			return &Resolved{Path: found[0], Root: root, Ambiguous: len(found) > 1}
		}
		_ = hits
	}
	return nil
}

func ModuleCandidates(importer string, includePaths []string, compiler *string) []string {
	set := map[string]bool{}
	for _, root := range RootsFor(importer, includePaths, compiler) {
		for _, n := range listModules(root, 2) {
			set[n] = true
		}
	}
	var out []string
	for k := range set {
		out = append(out, k)
	}
	sort.Strings(out)
	return out
}

func listModules(root string, depth int) []string {
	entries, err := os.ReadDir(root)
	if err != nil {
		return nil
	}
	sort.Slice(entries, func(i, j int) bool { return entries[i].Name() < entries[j].Name() })
	var out []string
	for _, e := range entries {
		name := e.Name()
		if strings.HasPrefix(name, ".") || strings.HasPrefix(name, "_") {
			continue
		}
		if !e.IsDir() && filepath.Ext(name) == Suffix {
			out = append(out, strings.TrimSuffix(name, Suffix))
		} else if e.IsDir() && depth > 1 {
			sub := filepath.Join(root, name)
			for _, child := range listModules(sub, depth-1) {
				out = append(out, name+"."+child)
			}
		}
	}
	return out
}

func ReadOutline(path string) *outline.Outline {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil
	}
	return outline.Build(string(data))
}
