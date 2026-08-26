package compiler

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
	"time"

	"github.com/BurntSushi/toml"
)

const DefaultBinary = "emeraldc"
const Lockfile = "emerald.lock"

type Settings struct {
	CompilerPath       *string
	IncludePaths       []string
	DiagnosticsEnabled bool
	Proof              bool
	DebounceMs         int
	TimeoutS           float64
}

func DefaultSettings() Settings {
	return Settings{
		DiagnosticsEnabled: true,
		DebounceMs:         250,
		TimeoutS:           10,
	}
}

func SettingsFromObject(obj interface{}) Settings {
	s := DefaultSettings()
	m, ok := obj.(map[string]interface{})
	if !ok {
		return s
	}
	emerald := m
	if e, ok := m["emerald"]; ok {
		if em, ok := e.(map[string]interface{}); ok {
			emerald = em
		}
	}
	if v, ok := emerald["compilerPath"]; ok {
		if str, ok := v.(string); ok && str != "" {
			s.CompilerPath = &str
		}
	}
	if v, ok := emerald["includePaths"]; ok {
		if arr, ok := v.([]interface{}); ok {
			for _, el := range arr {
				if str, ok := el.(string); ok {
					s.IncludePaths = append(s.IncludePaths, str)
				}
			}
		} else if arr2, ok := v.([]string); ok {
			s.IncludePaths = append(s.IncludePaths, arr2...)
		}
	}
	if v, ok := emerald["diagnostics"]; ok {
		if dm, ok := v.(map[string]interface{}); ok {
			if en, ok := dm["enabled"]; ok {
				if b, ok := en.(bool); ok {
					s.DiagnosticsEnabled = b
				}
			}
		}
	}
	if v, ok := emerald["proof"]; ok {
		if b, ok := v.(bool); ok {
			s.Proof = b
		}
	}
	if v, ok := emerald["debounceMs"]; ok {
		switch x := v.(type) {
		case int:
			s.DebounceMs = x
		case int64:
			s.DebounceMs = int(x)
		case float64:
			s.DebounceMs = int(x)
		}
	}
	if v, ok := emerald["timeoutSeconds"]; ok {
		switch x := v.(type) {
		case int:
			s.TimeoutS = float64(x)
		case int64:
			s.TimeoutS = float64(x)
		case float64:
			s.TimeoutS = x
		}
	}
	return s
}

type CheckResult struct {
	Diagnostics []map[string]interface{}
	Ok          bool
	Detail      string
}

type CompilerNotFoundError struct{ Msg string }

func (e *CompilerNotFoundError) Error() string { return e.Msg }

func FindCompiler(s Settings) (string, error) {
	candidates := []*string{s.CompilerPath}
	if env := os.Getenv("EMERALDC"); env != "" {
		candidates = append(candidates, &env)
	}
	for _, c := range candidates {
		if c == nil || *c == "" {
			continue
		}
		if p, err := exec.LookPath(*c); err == nil {
			return p, nil
		}
		if _, err := os.Stat(*c); err == nil {
			return *c, nil
		}
		return "", &CompilerNotFoundError{Msg: fmt.Sprintf("configured compiler not found: %s", *c)}
	}
	if p, err := exec.LookPath(DefaultBinary); err == nil {
		return p, nil
	}
	return "", &CompilerNotFoundError{Msg: "emeraldc is not on PATH; set emerald.compilerPath or $EMERALDC"}
}

func StoreRoot() string {
	if h := os.Getenv("EMERALD_HOME"); h != "" {
		return filepath.Join(h, "store")
	}
	home, _ := os.UserHomeDir()
	return filepath.Join(home, ".emerald", "store")
}

func FindLockfile(start string) *string {
	here := start
	if fi, err := os.Stat(start); err == nil && !fi.IsDir() {
		here = filepath.Dir(start)
	}
	dir := here
	for {
		candidate := filepath.Join(dir, Lockfile)
		if _, err := os.Stat(candidate); err == nil {
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

func LockIncludePaths(lock string) []string {
	data, err := os.ReadFile(lock)
	if err != nil {
		return nil
	}
	var parsed map[string]interface{}
	if _, err := toml.Decode(string(data), &parsed); err != nil {
		return nil
	}
	pkgsRaw, ok := parsed["package"]
	if !ok {
		return nil
	}
	pkgs, ok := pkgsRaw.([]map[string]interface{})
	if !ok {
		// BurntSushi decodes as []interface{} containing maps
		if arr, ok := pkgsRaw.([]interface{}); ok {
			for _, el := range arr {
				if m, ok := el.(map[string]interface{}); ok {
					pkgs = append(pkgs, m)
				}
			}
		} else {
			return nil
		}
	}
	byName := map[string]map[string]interface{}{}
	for _, e := range pkgs {
		if name, ok := e["name"].(string); ok {
			byName[name] = e
		}
	}
	ordered := []string{}
	seen := map[string]bool{}
	store := StoreRoot()
	lockDir := filepath.Dir(lock)

	var visit func(name string, stack map[string]bool)
	visit = func(name string, stack map[string]bool) {
		if seen[name] || stack[name] {
			return
		}
		entry, ok := byName[name]
		if !ok {
			return
		}
		var deps []string
		if d, ok := entry["dependencies"]; ok {
			if arr, ok := d.([]interface{}); ok {
				for _, el := range arr {
					if s, ok := el.(string); ok {
						deps = append(deps, s)
					}
				}
			}
		} else if d, ok := entry["deps"]; ok {
			if arr, ok := d.([]interface{}); ok {
				for _, el := range arr {
					if s, ok := el.(string); ok {
						deps = append(deps, s)
					}
				}
			}
		}
		sort.Strings(deps)
		newStack := map[string]bool{}
		for k, v := range stack {
			newStack[k] = v
		}
		newStack[name] = true
		for _, dep := range deps {
			base := strings.Fields(dep)[0]
			visit(base, newStack)
		}
		seen[name] = true
		if src := packageSrc(entry, store, lockDir); src != nil {
			ordered = append(ordered, *src)
		}
	}
	// sorted names
	var names []string
	for n := range byName {
		names = append(names, n)
	}
	sort.Strings(names)
	for _, n := range names {
		visit(n, map[string]bool{})
	}
	return ordered
}

func packageSrc(entry map[string]interface{}, store, lockDir string) *string {
	if p, ok := entry["path"].(string); ok {
		var base string
		if filepath.IsAbs(p) {
			base = p
		} else {
			base = filepath.Join(lockDir, p)
		}
		s := filepath.Join(base, "src")
		return &s
	}
	name, nOk := entry["name"].(string)
	version, vOk := entry["version"].(string)
	if nOk && vOk {
		s := filepath.Join(store, fmt.Sprintf("%s-%s", name, version), "src")
		return &s
	}
	return nil
}

func IncludePathsFor(path string, s Settings) []string {
	var roots []string
	roots = append(roots, s.IncludePaths...)
	if lock := FindLockfile(path); lock != nil {
		roots = append(roots, LockIncludePaths(*lock)...)
	}
	// dedup + expand + abspath
	seen := map[string]bool{}
	var out []string
	for _, r := range roots {
		expanded := r
		if strings.HasPrefix(r, "~/") || r == "~" {
			home, _ := os.UserHomeDir()
			if r == "~" {
				expanded = home
			} else {
				expanded = filepath.Join(home, r[2:])
			}
		}
		abs, err := filepath.Abs(expanded)
		if err != nil {
			abs = expanded
		}
		if !seen[abs] {
			seen[abs] = true
			out = append(out, abs)
		}
	}
	return out
}

func overlayPath(path string) string {
	dir := filepath.Dir(path)
	base := filepath.Base(path)
	ext := filepath.Ext(base)
	stem := strings.TrimSuffix(base, ext)
	return filepath.Join(dir, fmt.Sprintf(".%s.emlsp-%d%s", stem, os.Getpid(), ext))
}

func Check(path string, source *string, s Settings, compiler *string) CheckResult {
	var binary string
	if compiler != nil {
		binary = *compiler
	} else {
		b, err := FindCompiler(s)
		if err != nil {
			return CheckResult{Ok: false, Detail: err.Error()}
		}
		binary = b
	}
	roots := IncludePathsFor(path, s)
	return checkInner(path, source, s, binary, roots)
}

func checkInner(path string, source *string, s Settings, binary string, roots []string) CheckResult {
	runTarget := func(target string) CheckResult {
		argv := []string{"--check", "--json"}
		for _, r := range roots {
			argv = append(argv, "-I", r)
		}
		if s.Proof {
			argv = append(argv, "--proof")
		}
		argv = append(argv, target)
		ctx, cancel := context.WithTimeout(context.Background(), time.Duration(s.TimeoutS*float64(time.Second)))
		defer cancel()
		cmd := exec.CommandContext(ctx, binary, argv...)
		cmd.Dir = filepath.Dir(path)
		stdout, _ := cmd.StdoutPipe()
		_ = stdout
		// Use Combined? Let's use Output with separate pipes manually
		var outBuf, errBuf strings.Builder
		cmd.Stdout = &outBuf
		cmd.Stderr = &errBuf
		err := cmd.Run()
		if ctx.Err() == context.DeadlineExceeded {
			return CheckResult{Ok: false, Detail: fmt.Sprintf("emeraldc timed out after %gs", s.TimeoutS)}
		}
		if err != nil {
			// Check if it's exit error vs OSError
			if _, ok := err.(*exec.Error); ok {
				return CheckResult{Ok: false, Detail: fmt.Sprintf("could not run %s: %v", binary, err)}
			}
		}
		stdoutStr := outBuf.String()
		stderrStr := errBuf.String()
		code := 0
		if err != nil {
			if ee, ok := err.(*exec.ExitError); ok {
				code = ee.ExitCode()
			} else {
				code = 1
			}
		}
		return parseOutput(stdoutStr, stderrStr, code)
	}
	if source == nil {
		return runTarget(path)
	}
	temp := overlayPath(path)
	if err := os.WriteFile(temp, []byte(*source), 0644); err != nil {
		return CheckResult{Ok: false, Detail: fmt.Sprintf("could not write temp: %v", err)}
	}
	defer os.Remove(temp)
	result := runTarget(temp)
	remap(result.Diagnostics, filepath.Base(temp), path)
	return result
}

func parseOutput(stdout, stderr string, code int) CheckResult {
	text := strings.TrimSpace(stdout)
	if text == "" {
		if code == 0 {
			return CheckResult{Diagnostics: nil, Ok: true}
		}
		msg := strings.TrimSpace(stderr)
		if msg == "" {
			msg = fmt.Sprintf("emeraldc exited %d", code)
		}
		return CheckResult{Ok: false, Detail: msg}
	}
	var data interface{}
	if err := json.Unmarshal([]byte(text), &data); err != nil {
		return CheckResult{Ok: false, Detail: fmt.Sprintf("unparseable output from emeraldc: %s", text[:min(200, len(text))])}
	}
	arr, ok := data.([]interface{})
	if !ok {
		return CheckResult{Ok: false, Detail: "expected a JSON array of diagnostics"}
	}
	var diags []map[string]interface{}
	for _, el := range arr {
		if m, ok := el.(map[string]interface{}); ok {
			diags = append(diags, m)
		}
	}
	return CheckResult{Diagnostics: diags, Ok: true}
}

func remap(diagnostics []map[string]interface{}, tempName, realPath string) {
	for _, d := range diagnostics {
		if f, ok := d["file"].(string); ok {
			if filepath.Base(f) == tempName {
				d["file"] = realPath
			}
		}
	}
}

func Probe(binary string) *string {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, binary, "--version")
	var outBuf, errBuf strings.Builder
	cmd.Stdout = &outBuf
	cmd.Stderr = &errBuf
	_ = cmd.Run()
	s := strings.TrimSpace(outBuf.String() + errBuf.String())
	if s == "" {
		return nil
	}
	m := strings.TrimSpace(s)
	return &m
}

func min(a, b int) int {
	if a < b {
		return a
	}
	return b
}

// Export for tests
func OverlayPath(path string) string { return overlayPath(path) }
