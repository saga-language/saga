import * as vscode from 'vscode';
import {
  LanguageClient,
  LanguageClientOptions,
  ServerOptions,
  State,
} from 'vscode-languageclient/node';

let client: LanguageClient | undefined;
let statusBar: vscode.StatusBarItem | undefined;

type ServerState = 'starting' | 'running' | 'stopped';

const STATUS: Record<ServerState, { text: string; tooltip: string; color?: vscode.ThemeColor }> = {
  starting: {
    text:    '$(sync~spin) Saga',
    tooltip: 'Saga language server is starting…',
  },
  running: {
    text:    '$(check) Saga',
    tooltip: 'Saga language server is running',
  },
  stopped: {
    text:    '$(x) Saga',
    tooltip: 'Saga language server stopped — make sure `saga` is on your PATH',
    color:   new vscode.ThemeColor('statusBarItem.errorBackground'),
  },
};

function setStatus(state: ServerState): void {
  if (!statusBar) return;
  const s = STATUS[state];
  statusBar.text    = s.text;
  statusBar.tooltip = s.tooltip;
  statusBar.backgroundColor = s.color;
  statusBar.show();
}

export function activate(context: vscode.ExtensionContext): void {
  // Status bar item — right-aligned, low priority so it sits near the end
  statusBar = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Right, 100);
  statusBar.command = 'saga.showOutput';
  context.subscriptions.push(statusBar);

  // Command: clicking the status bar item opens the LSP output channel
  context.subscriptions.push(
    vscode.commands.registerCommand('saga.showOutput', () => client?.outputChannel.show()),
  );

  const serverOptions: ServerOptions = {
    command: 'saga',
    args: ['lsp'],
  };

  const clientOptions: LanguageClientOptions = {
    documentSelector: [{ scheme: 'file', language: 'saga' }],
    synchronize: {
      fileEvents: vscode.workspace.createFileSystemWatcher('**/*.sg'),
    },
  };

  client = new LanguageClient(
    'saga',
    'Saga Language Server',
    serverOptions,
    clientOptions,
  );

  client.onDidChangeState((event) => {
    switch (event.newState) {
      case State.Starting:
        setStatus('starting');
        break;

      case State.Running:
        setStatus('running');
        break;

      case State.Stopped:
        setStatus('stopped');
        void vscode.window
          .showErrorMessage(
            'Saga language server stopped. Make sure `saga` is on your PATH.',
            'Show Output',
          )
          .then((choice) => {
            if (choice === 'Show Output') client?.outputChannel.show();
          });
        break;
    }
  });

  setStatus('starting');
  client.start();
  context.subscriptions.push(client);
}

export function deactivate(): Thenable<void> | undefined {
  statusBar?.hide();
  return client?.stop();
}
