package main

import (
	"archive/tar"
	"bufio"
	"bytes"
	"compress/gzip"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
	"time"

	"github.com/BurntSushi/toml"
)

const registryURL = "https://evangelion-research.github.io/pme-index"

type pmeError struct {
	Code, Message string
	Exit          int
}

func (e *pmeError) Error() string { return e.Message }
func fail(code, message string, exit ...int) error {
	n := 1
	if len(exit) > 0 {
		n = exit[0]
	}
	return &pmeError{code, message, n}
}

type Version struct {
	Major, Minor, Patch int
	Pre                 []string
}

func parseVersion(s string) (Version, error) {
	var v Version
	base := s
	if i := strings.IndexByte(s, '-'); i >= 0 {
		base = s[:i]
		v.Pre = strings.Split(s[i+1:], ".")
	}
	if strings.Contains(s, "+") || len(v.Pre) > 0 && (v.Pre[0] == "") {
		return v, errors.New("invalid semantic version `" + s + "`")
	}
	if _, e := fmt.Sscanf(base, "%d.%d.%d", &v.Major, &v.Minor, &v.Patch); e != nil || fmt.Sprintf("%d.%d.%d", v.Major, v.Minor, v.Patch) != base || v.Major < 0 || v.Minor < 0 || v.Patch < 0 {
		return v, errors.New("invalid semantic version `" + s + "`")
	}
	for _, p := range v.Pre {
		if p == "" || (len(p) > 1 && p[0] == '0' && digits(p)) {
			return v, errors.New("invalid semantic version `" + s + "`")
		}
	}
	return v, nil
}
func digits(s string) bool {
	for _, r := range s {
		if r < '0' || r > '9' {
			return false
		}
	}
	return s != ""
}
func (v Version) String() string {
	s := fmt.Sprintf("%d.%d.%d", v.Major, v.Minor, v.Patch)
	if len(v.Pre) > 0 {
		s += "-" + strings.Join(v.Pre, ".")
	}
	return s
}
func cmp(a, b Version) int {
	if a.Major != b.Major {
		if a.Major < b.Major {
			return -1
		}
		return 1
	}
	if a.Minor != b.Minor {
		if a.Minor < b.Minor {
			return -1
		}
		return 1
	}
	if a.Patch != b.Patch {
		if a.Patch < b.Patch {
			return -1
		}
		return 1
	}
	if len(a.Pre) == 0 {
		if len(b.Pre) == 0 {
			return 0
		}
		return 1
	}
	if len(b.Pre) == 0 {
		return -1
	}
	for i := 0; i < len(a.Pre) && i < len(b.Pre); i++ {
		x, y := a.Pre[i], b.Pre[i]
		if x == y {
			continue
		}
		dx, dy := digits(x), digits(y)
		if dx && dy {
			if len(x) != len(y) {
				if len(x) < len(y) {
					return -1
				}
				return 1
			}
			if x < y {
				return -1
			}
			return 1
		}
		if dx {
			return -1
		}
		if dy {
			return 1
		}
		if x < y {
			return -1
		}
		return 1
	}
	if len(a.Pre) < len(b.Pre) {
		return -1
	}
	if len(a.Pre) > len(b.Pre) {
		return 1
	}
	return 0
}

type Constraint struct {
	Raw             string
	Lower, Upper    Version
	HasUpper, Exact bool
}

func parseConstraint(s string) (Constraint, error) {
	c := Constraint{Raw: strings.TrimSpace(s)}
	t := c.Raw
	op := ""
	for _, x := range []string{">=", "==", "^", "~"} {
		if strings.HasPrefix(t, x) {
			op = x
			t = strings.TrimSpace(strings.TrimPrefix(t, x))
			break
		}
	}
	v, e := parseVersion(t)
	if e != nil {
		return c, e
	}
	c.Lower = v
	switch op {
	case "==":
		c.Upper = v
		c.HasUpper = true
		c.Exact = true
	case "^":
		c.HasUpper = true
		if v.Major > 0 {
			c.Upper = Version{Major: v.Major + 1}
		} else if v.Minor > 0 {
			c.Upper = Version{Minor: v.Minor + 1}
		} else {
			c.Upper = Version{Patch: v.Patch + 1}
		}
	case "~":
		c.HasUpper = true
		c.Upper = Version{Major: v.Major, Minor: v.Minor + 1}
	}
	return c, nil
}
func (c Constraint) allows(v Version) bool {
	return cmp(v, c.Lower) >= 0 && (!c.HasUpper || (c.Exact && cmp(v, c.Upper) == 0) || (!c.Exact && cmp(v, c.Upper) < 0))
}

type depRaw struct {
	Path string `toml:"path"`
}
type manifestRaw struct {
	Package      struct{ Name, Version string }
	Dependencies map[string]interface{} `toml:"dependencies"`
	Dev          map[string]interface{} `toml:"dev-dependencies"`
	Bin          []struct{ Name, Entry string }
	Lib          struct{ Root string }
}
type Dependency struct {
	Name       string
	Constraint *Constraint
	Path       string
}
type Target struct {
	Name, Entry, Kind string
	Output            string
}
type Manifest struct {
	Path, Root, Name string
	Version          Version
	Dependencies     map[string]Dependency
	Dev              map[string]Dependency
	Targets          []Target
}

func nameOK(s string) bool {
	if len(s) < 2 || len(s) > 64 || s[0] < 'a' || s[0] > 'z' {
		return false
	}
	for _, c := range s {
		if !(c >= 'a' && c <= 'z' || c >= '0' && c <= '9' || c == '_') {
			return false
		}
	}
	return true
}
func dependencies(raw map[string]interface{}, root string) (map[string]Dependency, error) {
	out := map[string]Dependency{}
	for n, v := range raw {
		if !nameOK(n) {
			return nil, fail("E_MANIFEST_NAME", "invalid dependency name `"+n+"`")
		}
		switch x := v.(type) {
		case string:
			c, e := parseConstraint(x)
			if e != nil {
				return nil, fail("E_MANIFEST_VERSION", e.Error())
			}
			out[n] = Dependency{Name: n, Constraint: &c}
		case map[string]interface{}:
			p, ok := x["path"].(string)
			if !ok || len(x) != 1 {
				return nil, fail("E_MANIFEST_DEPENDENCY", "dependency `"+n+"` must be a version string or { path = \"...\" }")
			}
			out[n] = Dependency{Name: n, Path: filepath.Join(root, p)}
		default:
			return nil, fail("E_MANIFEST_DEPENDENCY", "invalid dependency `"+n+"`")
		}
	}
	return out, nil
}
func loadManifest(path string) (Manifest, error) {
	p, e := filepath.Abs(path)
	if e != nil {
		return Manifest{}, e
	}
	var r manifestRaw
	if _, e = toml.DecodeFile(p, &r); e != nil {
		return Manifest{}, fail("E_MANIFEST_TOML", "invalid TOML: "+e.Error())
	}
	if !nameOK(r.Package.Name) {
		return Manifest{}, fail("E_MANIFEST_NAME", "package name must match [a-z][a-z0-9_]{1,63}")
	}
	v, e := parseVersion(r.Package.Version)
	if e != nil {
		return Manifest{}, fail("E_MANIFEST_VERSION", e.Error())
	}
	m := Manifest{Path: p, Root: filepath.Dir(p), Name: r.Package.Name, Version: v}
	if m.Dependencies, e = dependencies(r.Dependencies, m.Root); e != nil {
		return m, e
	}
	if m.Dev, e = dependencies(r.Dev, m.Root); e != nil {
		return m, e
	}
	for _, b := range r.Bin {
		if b.Entry == "" {
			return m, fail("E_MANIFEST_TARGET", "each [[bin]] requires an entry")
		}
		if b.Name == "" {
			b.Name = strings.TrimSuffix(filepath.Base(b.Entry), ".rald")
		}
		if !nameOK(b.Name) {
			return m, fail("E_MANIFEST_NAME", "invalid binary name `"+b.Name+"`")
		}
		m.Targets = append(m.Targets, Target{b.Name, filepath.Join(m.Root, b.Entry), "bin", filepath.Join(m.Root, "target", "debug", b.Name)})
	}
	if r.Lib.Root != "" {
		m.Targets = append(m.Targets, Target{m.Name, filepath.Join(m.Root, r.Lib.Root), "lib", ""})
	}
	if len(m.Targets) == 0 {
		return m, fail("E_MANIFEST_TARGET", "at least one of [lib] or [[bin]] is required")
	}
	return m, nil
}
func findManifest() (string, error) {
	p, e := os.Getwd()
	if e != nil {
		return "", e
	}
	for {
		q := filepath.Join(p, "emerald.toml")
		if _, e = os.Stat(q); e == nil {
			return q, nil
		}
		n := filepath.Dir(p)
		if n == p {
			return "", fail("E_MANIFEST_NOT_FOUND", "could not find emerald.toml")
		}
		p = n
	}
}

type locked struct {
	Name, Version, Checksum string
	Deps                    []string
	URL                     string
}
type lockRaw struct {
	Version int
	Package []locked
}

func loadLock(path string) (map[string]locked, error) {
	var r lockRaw
	if _, e := toml.DecodeFile(path, &r); e != nil {
		return nil, fail("E_LOCK_NOT_FOUND", "lockfile not found: "+path)
	}
	if r.Version != 1 {
		return nil, fail("E_LOCK_VERSION", "unsupported lockfile version")
	}
	out := map[string]locked{}
	for _, p := range r.Package {
		if _, e := parseVersion(p.Version); e != nil || p.Name == "" || p.Checksum == "" {
			return nil, fail("E_LOCK_PACKAGE", "invalid lockfile package")
		}
		if _, ok := out[p.Name]; ok {
			return nil, fail("E_LOCK_PACKAGE", "duplicate locked package `"+p.Name+"`")
		}
		sort.Strings(p.Deps)
		out[p.Name] = p
	}
	return out, nil
}
func lockText(pkgs map[string]locked) string {
	names := make([]string, 0, len(pkgs))
	for n := range pkgs {
		names = append(names, n)
	}
	sort.Strings(names)
	var b strings.Builder
	b.WriteString("# Generated by pme. Do not edit.\nversion = 1\n\n")
	for _, n := range names {
		p := pkgs[n]
		fmt.Fprintf(&b, "[[package]]\nname = %q\nversion = %q\nchecksum = %q\ndeps = [", p.Name, p.Version, p.Checksum)
		for i, d := range p.Deps {
			if i > 0 {
				b.WriteString(", ")
			}
			fmt.Fprintf(&b, "%q", d)
		}
		b.WriteString("]\n")
		if p.URL != "" {
			fmt.Fprintf(&b, "url = %q\n", p.URL)
		}
		b.WriteByte('\n')
	}
	return b.String()
}
func writeLock(path string, p map[string]locked) error {
	return os.WriteFile(path, []byte(lockText(p)), 0644)
}

type release struct {
	Name, Version, Checksum, URL string
	Deps                         map[string]string
	Yanked                       bool
}

func releases(name string) ([]release, error) {
	base := strings.TrimRight(os.Getenv("PME_REGISTRY"), "/")
	if base == "" {
		base = registryURL
	}
	url := fmt.Sprintf("%s/index/%s/%s/%s.json", base, name[:2], name[2:4], name)
	resp, e := http.Get(url)
	if e != nil {
		return nil, fail("E_REGISTRY_FETCH", e.Error(), 3)
	}
	defer resp.Body.Close()
	if resp.StatusCode == 404 {
		return nil, nil
	}
	if resp.StatusCode < 200 || resp.StatusCode > 299 {
		return nil, fail("E_REGISTRY_FETCH", resp.Status, 3)
	}
	var result []release
	scanner := bufio.NewScanner(resp.Body)
	for scanner.Scan() {
		line := scanner.Bytes()
		var r release
		if e = json.Unmarshal(line, &r); e != nil {
			return nil, fail("E_REGISTRY_FETCH", e.Error(), 3)
		}
		result = append(result, r)
	}
	if e := scanner.Err(); e != nil {
		return nil, fail("E_REGISTRY_FETCH", e.Error(), 3)
	}
	return result, nil
}
func resolve(deps map[string]Dependency) (map[string]locked, error) {
	req := map[string][]Constraint{}
	for n, d := range deps {
		if d.Constraint != nil {
			req[n] = append(req[n], *d.Constraint)
		}
	}
	out := map[string]locked{}
	for changed := true; changed; {
		changed = false
		names := make([]string, 0, len(req))
		for n := range req {
			names = append(names, n)
		}
		sort.Strings(names)
		for _, n := range names {
			rs, e := releases(n)
			if e != nil {
				return nil, e
			}
			var chosen *release
			for i := range rs {
				v, e := parseVersion(rs[i].Version)
				if e != nil || rs[i].Yanked {
					continue
				}
				ok := true
				for _, c := range req[n] {
					if !c.allows(v) {
						ok = false
					}
				}
				if ok && (chosen == nil || cmp(v, mustVersion(chosen.Version)) < 0) {
					chosen = &rs[i]
				}
			}
			if chosen == nil {
				return nil, fail("E_RESOLVE_NOT_FOUND", "no published version of `"+n+"` satisfies requirements")
			}
			next := locked{chosen.Name, chosen.Version, chosen.Checksum, nil, chosen.URL}
			for child, s := range chosen.Deps {
				next.Deps = append(next.Deps, child)
				c, e := parseConstraint(s)
				if e != nil {
					return nil, e
				}
				have := false
				for _, x := range req[child] {
					if x.Raw == c.Raw {
						have = true
					}
				}
				if !have {
					req[child] = append(req[child], c)
					changed = true
				}
			}
			sort.Strings(next.Deps)
			if old, ok := out[n]; !ok || old.Version != next.Version {
				out[n] = next
				changed = true
			}
		}
	}
	return out, nil
}
func mustVersion(s string) Version { v, _ := parseVersion(s); return v }

func home() string {
	if h := os.Getenv("PME_HOME"); h != "" {
		return h
	}
	h, _ := os.UserHomeDir()
	return filepath.Join(h, ".emerald")
}
func storePath(p locked) (string, error) {
	if !strings.HasPrefix(p.Checksum, "sha256:") || len(p.Checksum) != 71 {
		return "", fail("E_STORE_CHECKSUM", "unsupported checksum `"+p.Checksum+"`")
	}
	return filepath.Join(home(), "store", fmt.Sprintf("%s-%s-%s", p.Name, p.Version, p.Checksum[7:19])), nil
}
func install(pkgs map[string]locked) error {
	for _, p := range pkgs {
		dest, e := storePath(p)
		if e != nil {
			return e
		}
		if st, e := os.Stat(dest); e == nil && st.IsDir() {
			continue
		}
		if p.URL == "" {
			return fail("E_STORE_MISSING", "locked package `"+p.Name+"` has no download URL")
		}
		r, e := http.Get(p.URL)
		if e != nil {
			return fail("E_REGISTRY_DOWNLOAD", e.Error(), 3)
		}
		data, e := io.ReadAll(r.Body)
		r.Body.Close()
		if e != nil {
			return e
		}
		sum := sha256.Sum256(data)
		if "sha256:"+hex.EncodeToString(sum[:]) != p.Checksum {
			return fail("E_STORE_CHECKSUM", "checksum mismatch for `"+p.Name+"`")
		}
		tmp, e := os.MkdirTemp(filepath.Join(home(), "store"), ".extract-")
		if e != nil {
			return e
		}
		if e = extract(data, tmp); e != nil {
			os.RemoveAll(tmp)
			return e
		}
		if e = os.MkdirAll(filepath.Dir(dest), 0755); e != nil {
			return e
		}
		if e = os.Rename(tmp, dest); e != nil {
			return e
		}
	}
	return nil
}
func extract(data []byte, dest string) error {
	g, e := gzip.NewReader(bytes.NewReader(data))
	if e != nil {
		return e
	}
	defer g.Close()
	t := tar.NewReader(g)
	for {
		h, e := t.Next()
		if e == io.EOF {
			break
		}
		if e != nil {
			return e
		}
		target := filepath.Join(dest, h.Name)
		if !strings.HasPrefix(filepath.Clean(target), filepath.Clean(dest)+string(os.PathSeparator)) || h.Typeflag == tar.TypeSymlink || h.Typeflag == tar.TypeLink {
			return fail("E_STORE_ARCHIVE", "unsafe path in package archive")
		}
		if h.Typeflag == tar.TypeDir {
			if e = os.MkdirAll(target, 0755); e != nil {
				return e
			}
			continue
		}
		if e = os.MkdirAll(filepath.Dir(target), 0755); e != nil {
			return e
		}
		f, e := os.OpenFile(target, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0644)
		if e != nil {
			return e
		}
		_, e = io.Copy(f, t)
		f.Close()
		if e != nil {
			return e
		}
	}
	return nil
}

func roots(m Manifest, lock map[string]locked) ([]string, error) {
	var out []string
	seen := map[string]bool{}
	var walk func(string) error
	walk = func(n string) error {
		if seen[n] {
			return nil
		}
		seen[n] = true
		p, ok := lock[n]
		if !ok {
			return fail("E_LOCK_PACKAGE", "locked dependency `"+n+"` is missing")
		}
		for _, d := range p.Deps {
			if e := walk(d); e != nil {
				return e
			}
		}
		x, e := storePath(p)
		if e != nil {
			return e
		}
		x = filepath.Join(x, "src")
		if s, e := os.Stat(x); e != nil || !s.IsDir() {
			return fail("E_STORE_MISSING", "store entry for `"+n+"` is missing; run pme install")
		}
		out = append(out, x)
		return nil
	}
	names := make([]string, 0, len(lock))
	for n := range lock {
		names = append(names, n)
	}
	sort.Strings(names)
	for _, n := range names {
		if e := walk(n); e != nil {
			return nil, e
		}
	}
	var pathWalk func(Manifest) error
	pathWalk = func(x Manifest) error {
		names := make([]string, 0)
		for n, d := range x.Dependencies {
			if d.Path != "" {
				names = append(names, n)
			}
		}
		sort.Strings(names)
		for _, n := range names {
			d := x.Dependencies[n]
			child, e := loadManifest(filepath.Join(d.Path, "emerald.toml"))
			if e != nil {
				return e
			}
			if e = pathWalk(child); e != nil {
				return e
			}
			out = append(out, filepath.Join(child.Root, "src"))
		}
		return nil
	}
	if e := pathWalk(m); e != nil {
		return nil, e
	}
	return out, nil
}
func build(m Manifest, lock map[string]locked, selected, mode string, proof, keep bool, jsonOut bool) error {
	rs, e := roots(m, lock)
	if e != nil {
		return e
	}
	compiler := os.Getenv("PME_EMERALDC")
	if compiler == "" {
		compiler = "emeraldc"
	}
	bin, e := exec.LookPath(compiler)
	if e != nil {
		return fail("E_RESOLVE_COMPILER", "could not find emeraldc on PATH")
	}
	for _, t := range m.Targets {
		if selected != "" && t.Name != selected {
			continue
		}
		if _, e := os.Stat(t.Entry); e != nil {
			return fail("E_BUILD_ENTRY", "target entry does not exist: "+t.Entry)
		}
		args := []string{}
		for _, r := range rs {
			args = append(args, "-I", r)
		}
		actual := mode
		if t.Kind == "lib" && mode == "build" {
			actual = "check"
		}
		if actual == "check" {
			args = append(args, "--check")
		} else if actual == "emit-c" {
			args = append(args, "--emit-c")
		} else {
			if e = os.MkdirAll(filepath.Dir(t.Output), 0755); e != nil {
				return e
			}
			args = append(args, "-o", t.Output)
		}
		if keep {
			args = append(args, "--keep-c")
		}
		if proof {
			args = append(args, "--proof")
		}
		if jsonOut {
			args = append(args, "--json")
		}
		args = append(args, t.Entry)
		c := exec.Command(bin, args...)
		c.Dir = m.Root
		c.Stdin = os.Stdin
		c.Stdout = os.Stdout
		c.Stderr = os.Stderr
		if e = c.Run(); e != nil {
			return fail("E_BUILD_COMPILER", "emeraldc failed for target `"+t.Name+"`")
		}
	}
	return nil
}

func usage() {
	fmt.Fprintln(os.Stderr, "usage: pme [--json] [-q] <init|install|build|check|emit-c|run|test|tree|why|clean|verify> [args]")
}
func main() {
	args := os.Args[1:]
	jsonOut := false
	quiet := false
	for len(args) > 0 && (args[0] == "--json" || args[0] == "-q" || args[0] == "--quiet") {
		if args[0] == "--json" {
			jsonOut = true
		} else {
			quiet = true
		}
		args = args[1:]
	}
	if len(args) == 0 {
		usage()
		os.Exit(2)
	}
	err := run(args, jsonOut, quiet)
	if err != nil {
		if e, ok := err.(*pmeError); ok {
			if jsonOut {
				b, _ := json.Marshal(map[string]string{"error": e.Code, "message": e.Message})
				fmt.Fprintln(os.Stderr, string(b))
			} else {
				fmt.Fprintf(os.Stderr, "error[%s]: %s\n", e.Code, e.Message)
			}
			os.Exit(e.Exit)
		}
		fmt.Fprintln(os.Stderr, "error:", err)
		os.Exit(1)
	}
}
func run(a []string, jsonOut, quiet bool) error {
	cmd := a[0]
	if cmd == "--version" || cmd == "version" {
		fmt.Println("pme 0.1.0")
		return nil
	}
	if cmd == "init" {
		name := ""
		if len(a) > 1 {
			name = a[1]
		}
		root, _ := os.Getwd()
		if name == "" {
			name = filepath.Base(root)
		}
		if !nameOK(name) {
			return fail("E_MANIFEST_NAME", "invalid package name `"+name+"`")
		}
		if _, e := os.Stat("emerald.toml"); e == nil {
			return fail("E_INIT_EXISTS", "emerald.toml already exists")
		}
		os.MkdirAll("src", 0755)
		if err := os.WriteFile("emerald.toml", []byte(fmt.Sprintf("[package]\nname = %q\nversion = \"0.1.0\"\n\n[dependencies]\n\n[[bin]]\nname = %q\nentry = \"src/main.rald\"\n", name, name)), 0644); err != nil {
			return err
		}
		if _, err := os.Stat(filepath.Join("src", "main.rald")); os.IsNotExist(err) {
			return os.WriteFile(filepath.Join("src", "main.rald"), []byte("# Welcome to Emerald.\nprint(\"Hello, world!\")\n"), 0644)
		}
		return nil
	}
	p, e := findManifest()
	if e != nil {
		return e
	}
	m, e := loadManifest(p)
	if e != nil {
		return e
	}
	lockPath := filepath.Join(m.Root, "emerald.lock")
	lock := map[string]locked{}
	if cmd == "install" {
		lock, e = resolve(m.Dependencies)
		if e != nil {
			return e
		}
		if e = writeLock(lockPath, lock); e != nil {
			return e
		}
		if e = install(lock); e != nil {
			return e
		}
		return emit(jsonOut, quiet, "locked", map[string]any{"packages": len(lock)})
	}
	if len(m.Dependencies) > 0 {
		lock, e = loadLock(lockPath)
		if e != nil {
			return e
		}
		for name, dep := range m.Dependencies {
			if dep.Constraint == nil {
				continue
			}
			pkg, ok := lock[name]
			version, parseErr := parseVersion(pkg.Version)
			if !ok || parseErr != nil || !dep.Constraint.allows(version) {
				return fail("E_LOCK_STALE", "emerald.lock is out of date; run `pme install`")
			}
		}
	}
	switch cmd {
	case "build", "check", "emit-c":
		selected := ""
		if len(a) > 1 && !strings.HasPrefix(a[1], "-") {
			selected = a[1]
		}
		return build(m, lock, selected, cmd, false, contains(a, "--keep-c"), jsonOut)
	case "run":
		selected := ""
		if len(a) > 1 && !strings.HasPrefix(a[1], "-") {
			selected = a[1]
		}
		if selected == "" {
			for _, t := range m.Targets {
				if t.Kind == "bin" {
					if selected != "" {
						return fail("E_BUILD_TARGET", "select a binary target")
					}
					selected = t.Name
				}
			}
		}
		if e = build(m, lock, selected, "build", false, false, jsonOut); e != nil {
			return e
		}
		return exec.Command(filepath.Join(m.Root, "target", "debug", selected)).Run()
	case "tree":
		names := make([]string, 0, len(lock))
		for name := range lock {
			names = append(names, name)
		}
		sort.Strings(names)
		for _, name := range names {
			p := lock[name]
			fmt.Printf("%s %s\n", p.Name, p.Version)
		}
		return nil
	case "test":
		files, err := filepath.Glob(filepath.Join(m.Root, "tests", "*.rald"))
		if err != nil {
			return err
		}
		for _, entry := range files {
			name := "test-" + strings.TrimSuffix(filepath.Base(entry), ".rald")
			copy := m
			copy.Targets = []Target{{Name: name, Entry: entry, Kind: "bin", Output: filepath.Join(m.Root, "target", "debug", "tests", strings.TrimSuffix(filepath.Base(entry), ".rald"))}}
			if err := build(copy, lock, name, "build", false, false, jsonOut); err != nil {
				return err
			}
			if err := exec.Command(copy.Targets[0].Output).Run(); err != nil {
				return fail("E_TEST_FAILED", "test `"+filepath.Base(entry)+"` failed")
			}
		}
		return emit(jsonOut, quiet, fmt.Sprintf("passed %d test(s)", len(files)), map[string]any{"tests": len(files)})
	case "clean":
		return os.RemoveAll(filepath.Join(m.Root, "target"))
	case "verify":
		for _, p := range lock {
			x, e := storePath(p)
			if e != nil {
				return e
			}
			if _, e = os.Stat(x); e != nil {
				return fail("E_STORE_MISSING", "store entry for `"+p.Name+"` is missing")
			}
		}
		return emit(jsonOut, quiet, "verified", map[string]any{"packages": len(lock)})
	default:
		usage()
		return fail("E_USAGE", "unknown command `"+cmd+"`", 2)
	}
}
func contains(a []string, s string) bool {
	for _, x := range a {
		if x == s {
			return true
		}
	}
	return false
}
func emit(j, q bool, message string, data map[string]any) error {
	if q {
		return nil
	}
	if j {
		data["status"] = "ok"
		data["message"] = message
		b, _ := json.Marshal(data)
		fmt.Println(string(b))
	} else {
		fmt.Println(message)
	}
	return nil
}

var _ = time.Now
