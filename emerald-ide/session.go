package main

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"time"
)

type Diag struct {
	Kind       string `json:"kind"`
	Severity   string `json:"severity"`
	Code       string `json:"code"`
	File       string `json:"file"`
	Message    string `json:"message"`
	Expected   string `json:"expected"`
	Actual     string `json:"actual"`
	SourceLine string `json:"source_line"`
	Line       int    `json:"line"`
	Column     int    `json:"column"`
}
type CheckOutcome struct {
	ExitCode int    `json:"exit_code"`
	TimedOut bool   `json:"timed_out"`
	Millis   int64  `json:"ms"`
	Diags    []Diag `json:"diags"`
	Error    string `json:"error,omitempty"`
}
type CompilerInfo struct {
	Path   string
	Source string
}

type SessionState struct {
	Status   string
	Locus    int
	Failing  int
	Diags    []Diag
	Millis   int64
	Analysis Analysis
}

type Session struct {
	mu                       sync.RWMutex
	text, dir, baseName      string
	compilerAvailable        bool
	generation               uint64
	analyzeTimer, checkTimer *time.Timer
	status                   string
	locus, failing           int
	diags                    []Diag
	millis                   int64
	analysis                 Analysis
	OnUpdate                 func()
}

func NewSession() *Session {
	return &Session{status: "checking", locus: -1, failing: -1, baseName: "untitled.rald"}
}
func (s *Session) State() SessionState {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return SessionState{Status: s.status, Locus: s.locus, Failing: s.failing, Diags: append([]Diag(nil), s.diags...), Millis: s.millis, Analysis: s.analysis}
}
func (s *Session) SetCompiler(info CompilerInfo) {
	s.mu.Lock()
	s.compilerAvailable = info.Path != ""
	if !s.compilerAvailable {
		s.status = "offline"
		s.deriveParseDiagsLocked()
	}
	s.mu.Unlock()
	s.notify()
}
func (s *Session) SetFileContext(dir, name string) {
	s.mu.Lock()
	s.dir, s.baseName = dir, name
	s.mu.Unlock()
}
func (s *Session) SetText(text string, caretByte int, immediate bool) {
	s.mu.Lock()
	edited := text != s.text
	s.text = text
	if edited {
		s.retractToEditLocked(caretByte)
	}
	if s.analyzeTimer != nil {
		s.analyzeTimer.Stop()
	}
	s.analyzeTimer = time.AfterFunc(40*time.Millisecond, func() { s.runAnalyze() })
	if s.checkTimer != nil {
		s.checkTimer.Stop()
	}
	if immediate {
		s.mu.Unlock()
		s.runCheck()
		return
	}
	s.checkTimer = time.AfterFunc(220*time.Millisecond, func() { s.runCheck() })
	s.mu.Unlock()
}
func (s *Session) Advance() {
	s.mu.Lock()
	if s.locus+1 < len(s.analysis.Statements) {
		s.locus++
		s.bumpLocked()
		s.notifyLocked()
		s.mu.Unlock()
		s.runCheck()
		return
	}
	s.mu.Unlock()
}
func (s *Session) Retract() {
	s.mu.Lock()
	if s.locus >= 0 {
		s.locus--
		s.dropDiagsLocked(s.locus + 1)
		s.failing = -1
		if s.status == "failed" {
			s.status = "idle"
		}
		s.bumpLocked()
		s.notifyLocked()
	}
	s.mu.Unlock()
}
func (s *Session) GotoCursor(byteOffset int) {
	s.mu.Lock()
	if len(s.analysis.Statements) == 0 {
		s.mu.Unlock()
		return
	}
	index := 0
	for i, st := range s.analysis.Statements {
		if st.Start <= byteOffset {
			index = i
		} else {
			break
		}
	}
	s.locus = index
	s.bumpLocked()
	s.notifyLocked()
	s.mu.Unlock()
	s.runCheck()
}
func (s *Session) CheckAll() {
	s.mu.Lock()
	if len(s.analysis.Statements) > 0 {
		s.locus = len(s.analysis.Statements) - 1
		s.bumpLocked()
		s.notifyLocked()
		s.mu.Unlock()
		s.runCheck()
		return
	}
	s.mu.Unlock()
}
func (s *Session) Interrupt() {
	s.mu.Lock()
	s.generation++
	if s.compilerAvailable {
		s.status = "idle"
	} else {
		s.status = "offline"
	}
	if s.checkTimer != nil {
		s.checkTimer.Stop()
	}
	s.notifyLocked()
	s.mu.Unlock()
}
func (s *Session) EvalExpression(expr string) CheckOutcome {
	s.mu.RLock()
	text, dir, name, upto := s.text, s.dir, s.baseName, s.acceptedEndLocked()
	s.mu.RUnlock()
	return RunCheck(text, upto, "print("+expr+")", dir, name, 0)
}
func (s *Session) AcceptedEnd() int { s.mu.RLock(); defer s.mu.RUnlock(); return s.acceptedEndLocked() }
func (s *Session) acceptedEndLocked() int {
	if s.locus < 0 || s.locus >= len(s.analysis.Statements) {
		return -1
	}
	return s.analysis.Statements[s.locus].End
}

func (s *Session) runAnalyze() {
	s.mu.RLock()
	text := s.text
	generation := s.generation
	s.mu.RUnlock()
	result := Analyze(text)
	s.mu.Lock()
	if generation != s.generation {
		s.mu.Unlock()
		return
	}
	s.analysis = result
	if s.locus >= len(result.Statements) {
		s.locus = len(result.Statements) - 1
	}
	if !s.compilerAvailable {
		s.deriveParseDiagsLocked()
	}
	s.notifyLocked()
	s.mu.Unlock()
}
func (s *Session) runCheck() {
	s.mu.Lock()
	if !s.compilerAvailable {
		s.deriveParseDiagsLocked()
		s.notifyLocked()
		s.mu.Unlock()
		return
	}
	upto := s.acceptedEndLocked()
	if upto < 0 {
		s.diags = nil
		s.failing = -1
		s.status = "idle"
		s.notifyLocked()
		s.mu.Unlock()
		return
	}
	s.generation++
	generation := s.generation
	text, dir, name := s.text, s.dir, s.baseName
	s.status = "checking"
	s.notifyLocked()
	s.mu.Unlock()
	outcome := RunCheck(text, upto, "", dir, name, 10000)
	s.mu.Lock()
	defer s.mu.Unlock()
	if generation != s.generation {
		return
	}
	s.millis = outcome.Millis
	s.diags = outcome.Diags
	if outcome.Error != "" && outcome.ExitCode == -1 && !outcome.TimedOut {
		s.status = "offline"
	} else if len(outcome.Diags) > 0 {
		s.status = "failed"
		s.failing = s.statementOfDiagLocked(outcome.Diags[0])
	} else {
		s.status = "idle"
		s.failing = -1
	}
	s.notifyLocked()
}
func (s *Session) bumpLocked() { s.generation++ }
func (s *Session) retractToEditLocked(byteOffset int) {
	keep := -1
	for i, st := range s.analysis.Statements {
		if st.End <= byteOffset {
			keep = i
		} else {
			break
		}
	}
	if keep < s.locus {
		s.locus = keep
		s.dropDiagsLocked(keep + 1)
		s.failing = -1
		if s.status == "failed" {
			s.status = "idle"
		}
	}
}
func (s *Session) dropDiagsLocked(index int) {
	if index < 0 || index >= len(s.analysis.Statements) {
		return
	}
	cut := s.analysis.Statements[index].Start
	kept := s.diags[:0]
	for _, d := range s.diags {
		if d.Line <= 0 || lineOfStatement(s.analysis.Statements, d.Line-1) < index {
			kept = append(kept, d)
		}
	}
	_ = cut
	s.diags = kept
}
func lineOfStatement(stmts []Stmt, line int) int {
	for i, st := range stmts {
		if line >= st.StartLine && line <= st.EndLine {
			return i
		}
	}
	return -1
}
func (s *Session) statementOfDiagLocked(d Diag) int {
	if i := lineOfStatement(s.analysis.Statements, d.Line-1); i >= 0 {
		return i
	}
	return s.locus
}
func (s *Session) deriveParseDiagsLocked() {
	s.diags = nil
	for _, st := range s.analysis.Statements {
		if st.Error {
			s.diags = append(s.diags, Diag{Kind: "parse", Severity: "error", Code: "E-SYNTAX", Message: "syntax error", Line: st.StartLine + 1, Column: st.StartCol + 1})
		}
	}
	s.failing = -1
	if len(s.diags) > 0 {
		s.failing = s.statementOfDiagLocked(s.diags[0])
		s.status = "failed"
	} else {
		s.status = "idle"
	}
}
func (s *Session) IsLocusLine(line int) bool {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.locus >= 0 && s.locus < len(s.analysis.Statements) && s.analysis.Statements[s.locus].StartLine == line
}
func (s *Session) IsObligationLine(line int) bool {
	s.mu.RLock()
	defer s.mu.RUnlock()
	for _, o := range s.analysis.Obligations {
		if o.Line == line {
			return true
		}
	}
	return false
}
func (s *Session) IsDiagnosticLine(line int) bool {
	s.mu.RLock()
	defer s.mu.RUnlock()
	for _, d := range s.diags {
		if d.Line-1 == line {
			return true
		}
	}
	return false
}
func (s *Session) notifyLocked() {
	if s.OnUpdate != nil {
		go s.OnUpdate()
	}
}
func (s *Session) notify() {
	s.mu.RLock()
	f := s.OnUpdate
	s.mu.RUnlock()
	if f != nil {
		go f()
	}
}

func ResolveCompiler() CompilerInfo {
	if env := os.Getenv("EMERALDC"); env != "" && executable(env) {
		return CompilerInfo{env, "EMERALDC"}
	}
	if path, err := exec.LookPath("emeraldc"); err == nil {
		return CompilerInfo{path, "PATH"}
	}
	sibling := filepath.Clean(filepath.Join("..", "emerald", "bin", "emeraldc"))
	if executable(sibling) {
		return CompilerInfo{sibling, "sibling"}
	}
	return CompilerInfo{}
}
func executable(path string) bool {
	info, err := os.Stat(path)
	if err != nil || info.IsDir() {
		return false
	}
	return runtime.GOOS == "windows" || info.Mode()&0111 != 0
}
func RunCheck(text string, upto int, suffix, dir, baseName string, timeoutMS int) CheckOutcome {
	compiler := ResolveCompiler()
	if compiler.Path == "" {
		return CheckOutcome{ExitCode: -1, Error: "no emeraldc found", Diags: []Diag{}}
	}
	body := text
	if upto >= 0 && upto <= len(text) {
		body = text[:upto]
	}
	if suffix != "" {
		body += "\n" + suffix + "\n"
	}
	tmpDir := dir
	if tmpDir == "" {
		tmpDir = os.TempDir()
	}
	if baseName == "" {
		baseName = "untitled.rald"
	}
	path := filepath.Join(tmpDir, "."+baseName+".eide.rald")
	if err := os.WriteFile(path, []byte(body), 0600); err != nil {
		return CheckOutcome{ExitCode: -1, Error: err.Error(), Diags: []Diag{}}
	}
	defer os.Remove(path)
	ctxTimeout := 10 * time.Second
	if timeoutMS > 0 {
		ctxTimeout = time.Duration(timeoutMS) * time.Millisecond
	}
	ctx, cancel := context.WithTimeout(context.Background(), ctxTimeout)
	defer cancel()
	cmd := exec.CommandContext(ctx, compiler.Path, "--check", "--json", path)
	var stdout, stderr bytes.Buffer
	cmd.Stdout = &stdout
	cmd.Stderr = &stderr
	started := time.Now()
	err := cmd.Run()
	millis := time.Since(started).Milliseconds()
	out := CheckOutcome{Millis: millis, Diags: []Diag{}}
	if ctx.Err() != nil {
		out.TimedOut = true
		out.ExitCode = -1
		out.Error = fmt.Sprintf("emeraldc timed out after %d ms", ctxTimeout.Milliseconds())
		return out
	}
	if err != nil {
		if exit, ok := err.(*exec.ExitError); ok {
			out.ExitCode = exit.ExitCode()
		} else {
			out.ExitCode = -1
			out.Error = err.Error()
		}
	} else {
		out.ExitCode = 0
	}
	var raw []map[string]any
	if jsonErr := json.Unmarshal(bytes.TrimSpace(stdout.Bytes()), &raw); jsonErr == nil {
		for _, item := range raw {
			out.Diags = append(out.Diags, diagFromJSON(item))
		}
		return out
	}
	if out.ExitCode != 0 {
		out.Error = fmt.Sprintf("emeraldc exited %d with no JSON diagnostics", out.ExitCode)
		if line := strings.TrimSpace(strings.Split(stderr.String(), "\n")[0]); line != "" {
			out.Error += ": " + line
		}
	}
	return out
}
func diagFromJSON(v map[string]any) Diag {
	d := Diag{Kind: stringValue(v, "kind"), Severity: stringValue(v, "severity"), Code: stringValue(v, "code"), File: stringValue(v, "file"), Message: stringValue(v, "message"), Expected: stringValue(v, "expected"), Actual: stringValue(v, "actual"), SourceLine: stringValue(v, "source_line"), Line: intValue(v, "line"), Column: intValue(v, "column")}
	if d.Column == 0 {
		d.Column = intValue(v, "col")
	}
	return d
}
func stringValue(v map[string]any, key string) string { x, _ := v[key].(string); return x }
func intValue(v map[string]any, key string) int {
	x, ok := v[key].(float64)
	if !ok {
		return 0
	}
	return int(x)
}
