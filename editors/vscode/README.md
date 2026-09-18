# Emerald for Visual Studio Code

Syntax highlighting for `.rald` plus an LSP client that launches
`emerald-lsp` (from [`emlsp/`](../../emlsp/)) over stdio.

## Setup

1. Build the language server and put it on your `PATH` (or set
   `emerald.lspPath` / the `EMERALD_LSP` environment variable):

   ```sh
   cd emlsp && go build -o "$(go env GOPATH)/bin/emerald-lsp" ./cmd/emerald-lsp
   ```

2. Package and install the extension:

   ```sh
   cd editors/vscode
   npm install
   npx vsce package
   code --install-extension emerald-lang-0.1.0.vsix
   ```

## What works

- TextMate syntax highlighting (shared grammar, `syntaxes/emerald.tmLanguage.json`)
- Diagnostics, hover, go-to-definition, completion, outline via `emerald-lsp`
  (lexical-analysis limits of the server apply; see `emlsp/README.md`)

Not yet published to the Marketplace; install from the `.vsix`.
