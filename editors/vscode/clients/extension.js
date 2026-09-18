const vscode = require('vscode');
const { LanguageClient, TransportKind } = require('vscode-languageclient/node');

let client;

function activate(context) {
  // Point VS Code at a built emerald-lsp; override with emerald.lspPath.
  const config = vscode.workspace.getConfiguration('emerald');
  let serverPath = config.get('lspPath', '');
  if (!serverPath) {
    serverPath = process.env.EMERALD_LSP || 'emerald-lsp';
  }

  const serverOptions = {
    run: { command: serverPath, transport: TransportKind.stdio },
    debug: { command: serverPath, transport: TransportKind.stdio },
  };
  const clientOptions = {
    documentSelector: [{ scheme: 'file', language: 'emerald' }],
  };
  client = new LanguageClient('emerald', 'Emerald Language Server', serverOptions, clientOptions);
  client.start();
}

function deactivate() {
  if (client) return client.stop();
  return undefined;
}

module.exports = { activate, deactivate };
