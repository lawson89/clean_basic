'use strict';

const cp = require('child_process');
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

/**
 * Resolves the path to the 'cbfmt' formatter binary:
 * 1. Explicit 'cleanbasic.formatterPath' setting if configured.
 * 2. '<workspaceFolder>/build/cbfmt' if present inside the current workspace.
 * 3. '<extensionRoot>/../../build/cbfmt' if built inside the cleanbasic repository.
 * 4. Auto-compiles 'tools/cbfmt.cb' to 'build/cbfmt' using 'cleanbasic' if 'tools/cbfmt.cb' is present.
 * 5. Fallback to 'cbfmt' on system PATH.
 */
function resolveCbFmtExecutable() {
  const config = vscode.workspace.getConfiguration('cleanbasic');
  const configuredPath = (config.get('formatterPath') || '').trim();
  if (configuredPath.length > 0) {
    return configuredPath;
  }

  let exeName = 'cbfmt';
  if (process.platform === 'win32') {
    exeName = 'cbfmt.exe';
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

  const repoRoot = path.resolve(__dirname, '..', '..');
  const repoBuildCandidate = path.join(repoRoot, 'build', exeName);
  if (fs.existsSync(repoBuildCandidate)) {
    return repoBuildCandidate;
  }

  const candidateRoots = [];
  if (folders && folders.length > 0) {
    for (const folder of folders) {
      candidateRoots.push(folder.uri.fsPath);
    }
  }
  candidateRoots.push(repoRoot);

  for (const rootDir of candidateRoots) {
    const srcPath = path.join(rootDir, 'tools', 'cbfmt.cb');
    if (fs.existsSync(srcPath)) {
      const buildDir = path.join(rootDir, 'build');
      const binPath = path.join(buildDir, exeName);
      const outCpp = path.join(buildDir, 'cbfmt_build.cpp');
      try {
        if (!fs.existsSync(buildDir)) {
          fs.mkdirSync(buildDir, { recursive: true });
        }
        const cbExe = resolveCleanBasicExecutable();
        cp.execFileSync(cbExe, ['build', srcPath, '-o', outCpp, '-b', binPath], {
          cwd: rootDir,
          stdio: 'ignore'
        });
        if (fs.existsSync(binPath)) {
          return binPath;
        }
      } catch (_err) {
        // Fall through to PATH lookup if auto-build fails
      }
    }
  }

  return 'cbfmt';
}

function formatDocumentWithCbFmt(document, options) {
  return new Promise((resolve) => {
    const formatterCmd = resolveCbFmtExecutable();
    let tabSize = 4;
    if (options && typeof options.tabSize === 'number' && options.tabSize >= 0 && options.tabSize <= 16) {
      tabSize = Math.floor(options.tabSize);
    }

    const originalText = document.getText();
    const child = cp.spawn(formatterCmd, ['-i', String(tabSize)], {
      stdio: ['pipe', 'pipe', 'pipe']
    });

    const stdoutChunks = [];
    child.stdout.on('data', (chunk) => {
      stdoutChunks.push(chunk);
    });

    child.on('error', () => {
      resolve([]);
    });

    child.on('close', (code) => {
      if (code !== 0) {
        resolve([]);
        return;
      }
      const formattedText = Buffer.concat(stdoutChunks).toString('utf8');
      if (formattedText.length === 0 && originalText.trim().length > 0) {
        resolve([]);
        return;
      }
      if (formattedText === originalText) {
        resolve([]);
        return;
      }
      const lastLine = document.lineCount - 1;
      const lastChar = document.lineAt(lastLine).text.length;
      const fullRange = new vscode.Range(0, 0, lastLine, lastChar);
      resolve([vscode.TextEdit.replace(fullRange, formattedText)]);
    });

    child.stdin.on('error', () => {});
    child.stdin.end(originalText, 'utf8');
  });
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

  const formatterDisposable = vscode.languages.registerDocumentFormattingEditProvider(
    { language: 'cleanbasic' },
    {
      provideDocumentFormattingEdits(document, options) {
        return formatDocumentWithCbFmt(document, options);
      }
    }
  );
  context.subscriptions.push(formatterDisposable);

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
