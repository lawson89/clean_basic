'use strict';

const fs = require('fs');
const path = require('path');
const vscode = require('vscode');
const {
  LanguageClient
} = require('vscode-languageclient/node');

let client = null;

/**
 * Resolves the path to the 'cleanbasic' binary:
 * 1. Explicit 'cleanbasic.executablePath' setting if configured.
 * 2. '<workspaceFolder>/build/cleanbasic' if present inside the current workspace.
 * 3. '<extensionRoot>/../../build/cleanbasic' if built inside the cleanbasic repository.
 * 4. Fallback to 'cleanbasic' on system PATH.
 */
function resolveCleanBasicExecutable() {
  const config = vscode.workspace.getConfiguration('cleanbasic');
  const configuredPath = (config.get('executablePath') || '').trim();
  if (configuredPath.length > 0) {
    return configuredPath;
  }

  let exeName = 'cleanbasic';
  if (process.platform === 'win32') {
    exeName = 'cleanbasic.exe';
  }

  const folders = vscode.workspace.workspaceFolders;
  if (folders && folders.length > 0) {
    for (const folder of folders) {
      const candidate = path.join(folder.uri.fsPath, 'build', exeName);
      if (fs.existsSync(candidate)) {
        return candidate;
      }
    }
  }

  const repoBuildCandidate = path.resolve(__dirname, '..', '..', 'build', exeName);
  if (fs.existsSync(repoBuildCandidate)) {
    return repoBuildCandidate;
  }

  return 'cleanbasic';
}

async function startLanguageClient(context) {
  const command = resolveCleanBasicExecutable();

  const serverOptions = {
    run: {
      command: command,
      args: ['lsp']
    },
    debug: {
      command: command,
      args: ['lsp']
    }
  };

  const clientOptions = {
    documentSelector: [
      { scheme: 'file', language: 'cleanbasic' }
    ],
    synchronize: {
      fileEvents: vscode.workspace.createFileSystemWatcher('**/*.cb')
    }
  };

  client = new LanguageClient(
    'cleanbasicLanguageServer',
    'CleanBasic Language Server',
    serverOptions,
    clientOptions
  );

  await client.start();
}

async function stopLanguageClient() {
  if (client) {
    const activeClient = client;
    client = null;
    await activeClient.stop();
  }
}

async function activate(context) {
  const restartDisposable = vscode.commands.registerCommand(
    'cleanbasic.restartServer',
    async () => {
      await stopLanguageClient();
      await startLanguageClient(context);
      vscode.window.showInformationMessage('CleanBasic Language Server restarted.');
    }
  );
  context.subscriptions.push(restartDisposable);

  try {
    await startLanguageClient(context);
  } catch (err) {
    vscode.window.showWarningMessage(
      'Failed to start CleanBasic Language Server (' + err.message + '). ' +
      'Build ./build/cleanbasic or configure "cleanbasic.executablePath" in Settings.'
    );
  }
}

async function deactivate() {
  await stopLanguageClient();
}

module.exports = {
  activate,
  deactivate
};
