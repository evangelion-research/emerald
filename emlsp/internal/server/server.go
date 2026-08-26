package server

import (
	"bufio"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"github.com/evangelion-research/emlsp/internal/compiler"
	"github.com/evangelion-research/emlsp/internal/diagnostics"
	"github.com/evangelion-research/emlsp/internal/features"
	"github.com/evangelion-research/emlsp/internal/lexer"
	"github.com/evangelion-research/emlsp/internal/outline"
	"github.com/evangelion-research/emlsp/internal/positions"
	"github.com/evangelion-research/emlsp/internal/semantic"
)

const ServerName = "emerald-lsp"
const ServerVersion = "0.1.0"
const WorkspaceScanLimit = 2000

type TextDocument struct {
	URI     string
	Source  string
	Version int
	Lines   []string
}

type WorkspaceFolder struct {
	URI  string `json:"uri"`
	Name string `json:"name"`
}

type Server struct {
	mu             sync.Mutex
	settings       compiler.Settings
	compilerPath   *string
	compilerError  *string
	warned         bool
	documents      map[string]*TextDocument
	workspaceFolders map[string]WorkspaceFolder
	published      map[string]bool
	pending        map[string]*time.Timer
	writer         io.Writer
	writerMu       sync.Mutex
	nextID         int
}

func New() *Server {
	return &Server{
		settings:       compiler.DefaultSettings(),
		documents:      make(map[string]*TextDocument),
		workspaceFolders: make(map[string]WorkspaceFolder),
		published:      make(map[string]bool),
		pending:        make(map[string]*time.Timer),
		writer:         os.Stdout,
	}
}

func (s *Server) Configure(opts interface{}) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.settings = compiler.SettingsFromObject(opts)
	s.locateCompilerLocked()
}

func (s *Server) locateCompilerLocked() {
	p, err := compiler.FindCompiler(s.settings)
	if err != nil {
		s.compilerPath = nil
		msg := err.Error()
		s.compilerError = &msg
	} else {
		s.compilerPath = &p
		s.compilerError = nil
	}
}

func (s *Server) LocateCompiler() {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.locateCompilerLocked()
}

func (s *Server) Context(uri string) *features.Context {
	path, ok := positions.URIToPath(uri)
	if !ok {
		return nil
	}
	s.mu.Lock()
	doc, ok := s.documents[uri]
	s.mu.Unlock()
	var source string
	if ok {
		source = doc.Source
	} else {
		data, err := os.ReadFile(path)
		if err != nil {
			return nil
		}
		source = string(data)
	}
	o := outline.Build(source)
	var includePaths []string
	s.mu.Lock()
	ip := s.settings.IncludePaths
	cp := s.compilerPath
	s.mu.Unlock()
	// compute include paths
	includePaths = compiler.IncludePathsFor(path, compiler.Settings{IncludePaths: ip})
	// also need compiler include? ip already includes; add lockfile etc via IncludePathsFor handles
	_ = cp
	return &features.Context{
		Path:         path,
		Source:       source,
		Outline:      o,
		IncludePaths: includePaths,
		Compiler:     s.compilerPath,
	}
}

// ---- transport ----

type rpcRequest struct {
	JSONRPC string          `json:"jsonrpc"`
	ID      *json.RawMessage `json:"id,omitempty"`
	Method  string          `json:"method"`
	Params  json.RawMessage `json:"params,omitempty"`
}

type rpcResponse struct {
	JSONRPC string      `json:"jsonrpc"`
	ID      json.RawMessage `json:"id"`
	Result  interface{} `json:"result,omitempty"`
	Error   interface{} `json:"error,omitempty"`
}

type rpcNotification struct {
	JSONRPC string      `json:"jsonrpc"`
	Method  string      `json:"method"`
	Params  interface{} `json:"params,omitempty"`
}

func (s *Server) writeMessage(v interface{}) {
	data, _ := json.Marshal(v)
	s.writerMu.Lock()
	defer s.writerMu.Unlock()
	fmt.Fprintf(s.writer, "Content-Length: %d\r\n\r\n%s", len(data), data)
}

func (s *Server) sendNotification(method string, params interface{}) {
	s.writeMessage(rpcNotification{JSONRPC: "2.0", Method: method, Params: params})
}

func (s *Server) sendResponse(id json.RawMessage, result interface{}, errVal interface{}) {
	s.writeMessage(rpcResponse{JSONRPC: "2.0", ID: id, Result: result, Error: errVal})
}

func (s *Server) showMessage(msg string) {
	s.sendNotification("window/showMessage", map[string]interface{}{
		"type": 2, // Warning
		"message": fmt.Sprintf("%s: %s. Syntax features and unused-code diagnostics stay available; compiler diagnostics need emeraldc.", ServerName, msg),
	})
}

func (s *Server) warnOnceLocked(msg string) {
	if s.warned {
		return
	}
	s.warned = true
	// need to send outside lock? We'll send after unlocking
}

func (s *Server) StartIO() {
	s.writer = os.Stdout
	s.serveReader(bufio.NewReader(os.Stdin))
}

func (s *Server) StartTCP(host string, port int) {
	addr := fmt.Sprintf("%s:%d", host, port)
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		log.Fatalf("listen %s: %v", addr, err)
	}
	log.Printf("listening on %s", addr)
	for {
		conn, err := ln.Accept()
		if err != nil {
			continue
		}
		go func(c net.Conn) {
			defer c.Close()
			s2 := New()
			s2.writer = c
			// share? keep settings per connection
			s2.serveReader(bufio.NewReader(c))
		}(conn)
	}
}

func (s *Server) serveReader(r *bufio.Reader) {
	for {
		// read headers
		headers := map[string]string{}
		for {
			line, err := r.ReadString('\n')
			if err != nil {
				if err != io.EOF {
					log.Printf("read header: %v", err)
				}
				return
			}
			line = strings.TrimSpace(line)
			if line == "" {
				break
			}
			parts := strings.SplitN(line, ":", 2)
			if len(parts) == 2 {
				headers[strings.TrimSpace(strings.ToLower(parts[0]))] = strings.TrimSpace(parts[1])
			}
		}
		lengthStr, ok := headers["content-length"]
		if !ok {
			continue
		}
		var length int
		fmt.Sscanf(lengthStr, "%d", &length)
		body := make([]byte, length)
		if _, err := io.ReadFull(r, body); err != nil {
			log.Printf("read body: %v", err)
			return
		}
		var req rpcRequest
		if err := json.Unmarshal(body, &req); err != nil {
			continue
		}
		go s.handleRequest(req)
	}
}

func (s *Server) handleRequest(req rpcRequest) {
	switch req.Method {
	case "initialize":
		s.handleInitialize(req)
	case "initialized":
		// no-op
		if req.ID != nil {
			s.sendResponse(*req.ID, nil, nil)
		}
	case "shutdown":
		if req.ID != nil {
			s.sendResponse(*req.ID, nil, nil)
		}
	case "exit":
		os.Exit(0)
	case "workspace/didChangeConfiguration":
		s.handleDidChangeConfiguration(req)
	case "workspace/didChangeWatchedFiles":
		s.handleWatchedFiles(req)
	case "textDocument/didOpen":
		s.handleDidOpen(req)
		if req.ID != nil {
			s.sendResponse(*req.ID, nil, nil)
		}
	case "textDocument/didChange":
		s.handleDidChange(req)
		if req.ID != nil {
			s.sendResponse(*req.ID, nil, nil)
		}
	case "textDocument/didSave":
		s.handleDidSave(req)
		if req.ID != nil {
			s.sendResponse(*req.ID, nil, nil)
		}
	case "textDocument/didClose":
		s.handleDidClose(req)
		if req.ID != nil {
			s.sendResponse(*req.ID, nil, nil)
		}
	default:
		// textDocument/* handlers expecting response
		switch req.Method {
		case "textDocument/semanticTokens/full":
			s.handleSemanticTokens(req)
		case "textDocument/documentSymbol":
			s.handleDocumentSymbol(req)
		case "textDocument/foldingRange":
			s.handleFolding(req)
		case "textDocument/hover":
			s.handleHover(req)
		case "textDocument/definition":
			s.handleDefinition(req)
		case "textDocument/references":
			s.handleReferences(req)
		case "textDocument/documentHighlight":
			s.handleHighlight(req)
		case "textDocument/completion":
			s.handleCompletion(req)
		case "workspace/symbol":
			s.handleWorkspaceSymbol(req)
		default:
			if req.ID != nil {
				s.sendResponse(*req.ID, nil, nil)
			}
		}
	}
}

// ---- handlers ----

func (s *Server) handleInitialize(req rpcRequest) {
	var params map[string]interface{}
	_ = json.Unmarshal(req.Params, &params)
	if params != nil {
		if opts, ok := params["initializationOptions"]; ok {
			s.Configure(opts)
		} else if initOpts, ok := params["initialization_options"]; ok {
			s.Configure(initOpts)
		}
		// rootUri / workspaceFolders
		if folders, ok := params["workspaceFolders"]; ok {
			if arr, ok := folders.([]interface{}); ok {
				s.mu.Lock()
				for _, f := range arr {
					if m, ok := f.(map[string]interface{}); ok {
						uri, _ := m["uri"].(string)
						name, _ := m["name"].(string)
						if uri != "" {
							s.workspaceFolders[uri] = WorkspaceFolder{URI: uri, Name: name}
						}
					}
				}
				s.mu.Unlock()
			}
		} else if rootUri, ok := params["rootUri"].(string); ok && rootUri != "" {
			s.mu.Lock()
			s.workspaceFolders[rootUri] = WorkspaceFolder{URI: rootUri}
			s.mu.Unlock()
		}
	}
	if s.compilerPath != nil {
		if v := compiler.Probe(*s.compilerPath); v != nil {
			log.Printf("using %s (%s)", *s.compilerPath, *v)
		}
	}
	result := map[string]interface{}{
		"capabilities": map[string]interface{}{
			"textDocumentSync": map[string]interface{}{
				"openClose": true,
				"change":    2, // incremental
			},
			"semanticTokensProvider": map[string]interface{}{
				"legend": map[string]interface{}{
					"tokenTypes":     semantic.TokenTypes,
					"tokenModifiers": semantic.TokenModifiers,
				},
				"full": true,
			},
			"documentSymbolProvider": true,
			"foldingRangeProvider":   true,
			"hoverProvider":          true,
			"definitionProvider":     true,
			"referencesProvider":     true,
			"documentHighlightProvider": true,
			"completionProvider": map[string]interface{}{
				"triggerCharacters": []string{".", " "},
			},
			"workspaceSymbolProvider": true,
		},
		"serverInfo": map[string]string{"name": ServerName, "version": ServerVersion},
	}
	s.sendResponse(*req.ID, result, nil)
}

func (s *Server) handleDidChangeConfiguration(req rpcRequest) {
	var params map[string]interface{}
	_ = json.Unmarshal(req.Params, &params)
	s.Configure(params["settings"])
	// re-check all docs
	s.mu.Lock()
	uris := make([]string, 0, len(s.documents))
	for u := range s.documents {
		uris = append(uris, u)
	}
	s.mu.Unlock()
	for _, u := range uris {
		s.scheduleCheck(u)
	}
}

func (s *Server) handleWatchedFiles(req rpcRequest) {
	s.mu.Lock()
	uris := make([]string, 0, len(s.documents))
	for u := range s.documents {
		uris = append(uris, u)
	}
	s.mu.Unlock()
	for _, u := range uris {
		s.scheduleCheck(u)
	}
}

func (s *Server) handleDidOpen(req rpcRequest) {
	var params struct {
		TextDocument struct {
			URI     string `json:"uri"`
			Text    string `json:"text"`
			Version int    `json:"version"`
		} `json:"textDocument"`
	}
	_ = json.Unmarshal(req.Params, &params)
	lines := strings.Split(params.TextDocument.Text, "\n")
	// Keep as stored; for diagnostics need splitlines without keepends? Use normal split
	doc := &TextDocument{URI: params.TextDocument.URI, Source: params.TextDocument.Text, Version: params.TextDocument.Version, Lines: lines}
	s.mu.Lock()
	s.documents[params.TextDocument.URI] = doc
	s.mu.Unlock()
	s.scheduleCheck(params.TextDocument.URI)
}

func (s *Server) handleDidChange(req rpcRequest) {
	var params struct {
		TextDocument struct {
			URI     string `json:"uri"`
			Version int    `json:"version"`
		} `json:"textDocument"`
		ContentChanges []struct {
			Text string `json:"text"`
			Range *struct {
				Start positions.Position `json:"start"`
				End   positions.Position `json:"end"`
			} `json:"range,omitempty"`
		} `json:"contentChanges"`
	}
	_ = json.Unmarshal(req.Params, &params)
	// For simplicity, if incremental, we approximate by full text if range not provided?
	// If we get full text, use it; otherwise apply patches naively
	s.mu.Lock()
	doc, ok := s.documents[params.TextDocument.URI]
	s.mu.Unlock()
	if !ok {
		return
	}
	newText := doc.Source
	if len(params.ContentChanges) > 0 {
		// if any change has no Range, it's full sync
		if params.ContentChanges[0].Range == nil {
			newText = params.ContentChanges[0].Text
		} else {
			// apply incremental patches sequentially
			for _, ch := range params.ContentChanges {
				if ch.Range == nil {
					newText = ch.Text
					continue
				}
				newText = applyChange(newText, *ch.Range, ch.Text)
			}
		}
	}
	s.mu.Lock()
	doc.Source = newText
	doc.Version = params.TextDocument.Version
	doc.Lines = strings.Split(newText, "\n")
	s.mu.Unlock()
	s.scheduleCheck(params.TextDocument.URI)
}

func applyChange(src string, r struct {
	Start positions.Position `json:"start"`
	End   positions.Position `json:"end"`
}, text string) string {
	// Convert positions to offsets
	// Use helper similar to OffsetAt but for arbitrary src
	offStart := offsetFor(src, r.Start)
	offEnd := offsetFor(src, r.End)
	runes := []rune(src)
	if offStart > len(runes) {
		offStart = len(runes)
	}
	if offEnd > len(runes) {
		offEnd = len(runes)
	}
	return string(runes[:offStart]) + text + string(runes[offEnd:])
}

func offsetFor(src string, pos positions.Position) int {
	lines := strings.SplitAfter(src, "\n")
	offset := 0
	for i, line := range lines {
		if i == pos.Line {
			runes := []rune(line)
			if pos.Character > len(runes) {
				return offset + len(runes)
			}
			return offset + pos.Character
		}
		offset += len([]rune(line))
	}
	return len([]rune(src))
}

func (s *Server) handleDidSave(req rpcRequest) {
	var params struct {
		TextDocument struct{ URI string `json:"uri"` } `json:"textDocument"`
	}
	_ = json.Unmarshal(req.Params, &params)
	s.scheduleCheck(params.TextDocument.URI)
}

func (s *Server) handleDidClose(req rpcRequest) {
	var params struct {
		TextDocument struct{ URI string `json:"uri"` } `json:"textDocument"`
	}
	_ = json.Unmarshal(req.Params, &params)
	s.mu.Lock()
	delete(s.documents, params.TextDocument.URI)
	if t, ok := s.pending[params.TextDocument.URI]; ok {
		t.Stop()
		delete(s.pending, params.TextDocument.URI)
	}
	delete(s.published, params.TextDocument.URI)
	s.mu.Unlock()
	s.sendNotification("textDocument/publishDiagnostics", map[string]interface{}{"uri": params.TextDocument.URI, "diagnostics": []interface{}{}})
}

// ---- diagnostics scheduling ----

func (s *Server) scheduleCheck(uri string) {
	s.mu.Lock()
	if s.settings.DebounceMs <= 0 {
		s.mu.Unlock()
		go s.checkNow(uri)
		return
	}
	if t, ok := s.pending[uri]; ok {
		t.Stop()
	}
	// capture settings snapshot for debounce duration
	ds := s.settings.DebounceMs
	s.pending[uri] = time.AfterFunc(time.Duration(ds)*time.Millisecond, func() {
		s.mu.Lock()
		delete(s.pending, uri)
		s.mu.Unlock()
		s.checkNow(uri)
	})
	s.mu.Unlock()
}

func (s *Server) checkNow(uri string) {
	path, ok := positions.URIToPath(uri)
	if !ok {
		return
	}
	s.mu.Lock()
	doc, ok := s.documents[uri]
	s.mu.Unlock()
	var source string
	if ok {
		source = doc.Source
	} else {
		data, err := os.ReadFile(path)
		if err != nil {
			return
		}
		source = string(data)
	}
	ol := outline.Build(source)
	local := diagnostics.UnusedDiagnostics(ol)

	s.mu.Lock()
	cp := s.compilerPath
	ce := s.compilerError
	s.mu.Unlock()

	if cp == nil {
		// try locate again
		s.LocateCompiler()
		s.mu.Lock()
		cp = s.compilerPath
		ce = s.compilerError
		s.mu.Unlock()
		if cp == nil {
			if ce != nil && !s.warned {
				s.mu.Lock()
				s.warned = true
				s.mu.Unlock()
				s.showMessage(*ce)
			}
			s.publish(map[string][]diagnostics.Diagnostic{uri: local})
			return
		}
	}
	// check if source matches disk
	var onDisk string
	if data, err := os.ReadFile(path); err == nil {
		onDisk = string(data)
	}
	var srcPtr *string
	if source != onDisk {
		srcPtr = &source
	}
	s.mu.Lock()
	settings := s.settings
	s.mu.Unlock()
	result := compiler.Check(path, srcPtr, settings, cp)
	if !result.Ok {
		if !s.warned {
			s.mu.Lock()
			s.warned = true
			s.mu.Unlock()
			s.showMessage(result.Detail)
		}
		s.publish(map[string][]diagnostics.Diagnostic{uri: local})
		return
	}
	// group
	linesMap := map[string][]string{path: strings.Split(source, "\n")}
	grouped := diagnostics.GroupByURI(result.Diagnostics, linesMap)
	if _, ok := grouped[uri]; !ok {
		grouped[uri] = []diagnostics.Diagnostic{}
	}
	grouped[uri] = append(grouped[uri], local...)
	s.publish(grouped)
}

func (s *Server) publish(grouped map[string][]diagnostics.Diagnostic) {
	s.mu.Lock()
	// stale clearing
	for stale := range s.published {
		if _, ok := grouped[stale]; !ok {
			s.sendNotification("textDocument/publishDiagnostics", map[string]interface{}{"uri": stale, "diagnostics": []interface{}{}})
		}
	}
	newPublished := map[string]bool{}
	for u, diags := range grouped {
		// convert to LSP shape
		var out []map[string]interface{}
		for _, d := range diags {
			code := ""
			if d.Code != nil {
				code = *d.Code
			}
			src := ""
			if d.Source != nil {
				src = *d.Source
			}
			out = append(out, map[string]interface{}{
				"range": map[string]interface{}{
					"start": map[string]int{"line": d.Range.Start.Line, "character": d.Range.Start.Character},
					"end":   map[string]int{"line": d.Range.End.Line, "character": d.Range.End.Character},
				},
				"message":  d.Message,
				"severity": d.Severity,
				"code":     code,
				"source":   src,
				"data":     d.Data,
			})
		}
		if out == nil {
			out = []map[string]interface{}{}
		}
		s.sendNotification("textDocument/publishDiagnostics", map[string]interface{}{"uri": u, "diagnostics": out})
		newPublished[u] = true
	}
	s.published = newPublished
	s.mu.Unlock()
}

// ---- feature handlers ----

func (s *Server) handleSemanticTokens(req rpcRequest) {
	var params struct {
		TextDocument struct{ URI string `json:"uri"` } `json:"textDocument"`
	}
	_ = json.Unmarshal(req.Params, &params)
	ctx := s.Context(params.TextDocument.URI)
	if ctx == nil {
		s.sendResponse(*req.ID, map[string]interface{}{"data": []int{}}, nil)
		return
	}
	lines := strings.Split(ctx.Source, "\n")
	modulesBound := map[string]bool{}
	for k := range ctx.ModuleBindings() {
		modulesBound[k] = true
	}
	data := semantic.Encode(ctx.Outline.Tokens, lines, modulesBound)
	s.sendResponse(*req.ID, map[string]interface{}{"data": data}, nil)
}

func (s *Server) handleDocumentSymbol(req rpcRequest) {
	var params struct {
		TextDocument struct{ URI string `json:"uri"` } `json:"textDocument"`
	}
	_ = json.Unmarshal(req.Params, &params)
	ctx := s.Context(params.TextDocument.URI)
	if ctx == nil {
		s.sendResponse(*req.ID, []interface{}{}, nil)
		return
	}
	syms := features.DocumentSymbols(ctx.Outline)
	// marshal as LSP symbols
	s.sendResponse(*req.ID, syms, nil)
}

func (s *Server) handleFolding(req rpcRequest) {
	var params struct {
		TextDocument struct{ URI string `json:"uri"` } `json:"textDocument"`
	}
	_ = json.Unmarshal(req.Params, &params)
	ctx := s.Context(params.TextDocument.URI)
	if ctx == nil {
		s.sendResponse(*req.ID, []interface{}{}, nil)
		return
	}
	folds := features.FoldingRanges(ctx.Outline)
	s.sendResponse(*req.ID, folds, nil)
}

func (s *Server) handleHover(req rpcRequest) {
	var params struct {
		TextDocument struct{ URI string `json:"uri"` } `json:"textDocument"`
		Position     positions.Position `json:"position"`
	}
	_ = json.Unmarshal(req.Params, &params)
	ctx := s.Context(params.TextDocument.URI)
	if ctx == nil {
		s.sendResponse(*req.ID, nil, nil)
		return
	}
	h := features.HoverAt(ctx, params.Position)
	s.sendResponse(*req.ID, h, nil)
}

func (s *Server) handleDefinition(req rpcRequest) {
	var params struct {
		TextDocument struct{ URI string `json:"uri"` } `json:"textDocument"`
		Position     positions.Position `json:"position"`
	}
	_ = json.Unmarshal(req.Params, &params)
	ctx := s.Context(params.TextDocument.URI)
	if ctx == nil {
		s.sendResponse(*req.ID, []interface{}{}, nil)
		return
	}
	locs := features.DefinitionAt(ctx, params.Position)
	if locs == nil {
		locs = []features.Location{}
	}
	s.sendResponse(*req.ID, locs, nil)
}

func (s *Server) handleReferences(req rpcRequest) {
	var params struct {
		TextDocument struct{ URI string `json:"uri"` } `json:"textDocument"`
		Position     positions.Position `json:"position"`
	}
	_ = json.Unmarshal(req.Params, &params)
	ctx := s.Context(params.TextDocument.URI)
	if ctx == nil {
		s.sendResponse(*req.ID, []interface{}{}, nil)
		return
	}
	locs := features.References(ctx, params.Position)
	s.sendResponse(*req.ID, locs, nil)
}

func (s *Server) handleHighlight(req rpcRequest) {
	var params struct {
		TextDocument struct{ URI string `json:"uri"` } `json:"textDocument"`
		Position     positions.Position `json:"position"`
	}
	_ = json.Unmarshal(req.Params, &params)
	ctx := s.Context(params.TextDocument.URI)
	if ctx == nil {
		s.sendResponse(*req.ID, []interface{}{}, nil)
		return
	}
	hls := features.Highlights(ctx, params.Position)
	s.sendResponse(*req.ID, hls, nil)
}

func (s *Server) handleCompletion(req rpcRequest) {
	var params struct {
		TextDocument struct{ URI string `json:"uri"` } `json:"textDocument"`
		Position     positions.Position `json:"position"`
	}
	_ = json.Unmarshal(req.Params, &params)
	ctx := s.Context(params.TextDocument.URI)
	if ctx == nil {
		s.sendResponse(*req.ID, map[string]interface{}{"isIncomplete": false, "items": []interface{}{}}, nil)
		return
	}
	list := features.Completions(ctx, params.Position)
	s.sendResponse(*req.ID, list, nil)
}

func (s *Server) handleWorkspaceSymbol(req rpcRequest) {
	var params struct {
		Query string `json:"query"`
	}
	_ = json.Unmarshal(req.Params, &params)
	query := strings.ToLower(params.Query)
	var out []map[string]interface{}
	s.mu.Lock()
	folders := make([]string, 0, len(s.workspaceFolders))
	for u := range s.workspaceFolders {
		folders = append(folders, u)
	}
	// also include open doc dirs as fallback?
	if len(folders) == 0 {
		for uri := range s.documents {
			if p, ok := positions.URIToPath(uri); ok {
				folders = append(folders, positions.PathToURI(filepath.Dir(p)))
				break
			}
		}
	}
	s.mu.Unlock()
	seen := 0
	for _, folderURI := range folders {
		root, ok := positions.URIToPath(folderURI)
		if !ok {
			continue
		}
		filepath.Walk(root, func(path string, info os.FileInfo, err error) error {
			if seen >= WorkspaceScanLimit {
				return filepath.SkipDir
			}
			if err != nil {
				return nil
			}
			if info.IsDir() && strings.HasPrefix(info.Name(), ".") {
				return filepath.SkipDir
			}
			if !strings.HasSuffix(path, ".rald") {
				return nil
			}
			seen++
			// read file or use open buffer
			uri := positions.PathToURI(path)
			s.mu.Lock()
			doc, ok := s.documents[uri]
			s.mu.Unlock()
			var src string
			if ok {
				src = doc.Source
			} else {
				data, err := os.ReadFile(path)
				if err != nil {
					return nil
				}
				src = string(data)
			}
			ol := outline.Build(src)
			for _, sym := range ol.Symbols {
				if query != "" && !strings.Contains(strings.ToLower(sym.Name), query) {
					continue
				}
				out = append(out, map[string]interface{}{
					"name": sym.Name,
					"kind": sym.Kind,
					"location": map[string]interface{}{
						"uri": uri,
						"range": map[string]interface{}{
							"start": map[string]int{"line": sym.SelectionRange.Start.Line, "character": sym.SelectionRange.Start.Character},
							"end":   map[string]int{"line": sym.SelectionRange.End.Line, "character": sym.SelectionRange.End.Character},
						},
					},
					"containerName": filepath.Base(path),
				})
			}
			return nil
		})
	}
	if out == nil {
		out = []map[string]interface{}{}
	}
	s.sendResponse(*req.ID, out, nil)
}

// unused import fix
var _ = lexer.Tokenize
