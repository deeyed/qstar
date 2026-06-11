"use strict";

const vscode = require("vscode");
const cp = require("child_process");
const fs = require("fs");
const path = require("path");

class QStarLspClient {
  constructor(output) {
    this.output = output;
    this.process = null;
    this.buffer = "";
    this.nextId = 1;
    this.pending = new Map();
    this.diagnostics = vscode.languages.createDiagnosticCollection("qstar");
  }

  config() {
    return vscode.workspace.getConfiguration("qstar");
  }

  serverPath() {
    return this.config().get("server.path", "qstar");
  }

  traceEnabled() {
    return this.config().get("trace.server", false);
  }

  trace(message) {
    if (this.traceEnabled()) {
      this.output.appendLine(message);
    }
  }

  start() {
    if (this.process) {
      return;
    }
    const exe = this.serverPath();
    // QStar LSP is intentionally started as: qstar lsp --stdio.
    this.process = cp.spawn(exe, ["lsp", "--stdio"], {
      cwd: workspaceRoot(),
      stdio: ["pipe", "pipe", "pipe"]
    });
    this.process.stdout.on("data", chunk => this.onData(chunk));
    this.process.stderr.on("data", chunk => this.output.append(chunk.toString()));
    this.process.on("error", err => {
      this.output.appendLine(`qstar lsp failed to start: ${err.message}`);
      this.process = null;
    });
    this.process.on("exit", (code, signal) => {
      this.output.appendLine(`qstar lsp exited code=${code} signal=${signal}`);
      this.process = null;
      this.pending.clear();
    });
    this.request("initialize", {
      processId: process.pid,
      capabilities: {},
      rootUri: workspaceRootUri()
    }).catch(err => this.output.appendLine(`qstar initialize failed: ${err.message}`));
  }

  stop() {
    if (!this.process) {
      return;
    }
    this.request("shutdown", {}).catch(() => undefined).finally(() => {
      if (this.process) {
        this.notify("exit", {});
        this.process.kill();
        this.process = null;
      }
    });
    this.diagnostics.clear();
  }

  restart() {
    this.stop();
    this.start();
    for (const doc of vscode.workspace.textDocuments) {
      if (isQStarDocument(doc)) {
        this.didOpen(doc);
      }
    }
  }

  send(message) {
    this.start();
    if (!this.process || !this.process.stdin.writable) {
      return;
    }
    const payload = JSON.stringify(message);
    this.trace(`--> ${payload}`);
    this.process.stdin.write(`Content-Length: ${Buffer.byteLength(payload, "utf8")}\r\n\r\n${payload}`);
  }

  request(method, params) {
    const id = this.nextId++;
    const promise = new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error(`${method} timed out`));
      }, 2000);
      this.pending.set(id, { resolve, reject, timer });
    });
    this.send({ jsonrpc: "2.0", id, method, params });
    return promise;
  }

  notify(method, params) {
    this.send({ jsonrpc: "2.0", method, params });
  }

  onData(chunk) {
    this.buffer += chunk.toString();
    for (;;) {
      const headerEnd = this.buffer.indexOf("\r\n\r\n");
      if (headerEnd < 0) {
        return;
      }
      const header = this.buffer.slice(0, headerEnd);
      const match = /Content-Length:\s*(\d+)/i.exec(header);
      if (!match) {
        this.buffer = "";
        return;
      }
      const length = Number(match[1]);
      const start = headerEnd + 4;
      if (this.buffer.length < start + length) {
        return;
      }
      const payload = this.buffer.slice(start, start + length);
      this.buffer = this.buffer.slice(start + length);
      this.trace(`<-- ${payload}`);
      this.handleMessage(JSON.parse(payload));
    }
  }

  handleMessage(message) {
    if (message.method === "textDocument/publishDiagnostics") {
      this.publishDiagnostics(message.params);
      return;
    }
    if (Object.prototype.hasOwnProperty.call(message, "id")) {
      const pending = this.pending.get(message.id);
      if (!pending) {
        return;
      }
      clearTimeout(pending.timer);
      this.pending.delete(message.id);
      if (message.error) {
        pending.reject(new Error(message.error.message || "qstar lsp request failed"));
      } else {
        pending.resolve(message.result);
      }
    }
  }

  publishDiagnostics(params) {
    const uri = vscode.Uri.parse(params.uri);
    const diagnostics = (params.diagnostics || []).map(diag => {
      const range = new vscode.Range(
        diag.range.start.line,
        diag.range.start.character,
        diag.range.end.line,
        diag.range.end.character
      );
      const item = new vscode.Diagnostic(
        range,
        diag.message,
        diag.severity === 2 ? vscode.DiagnosticSeverity.Warning :
          diag.severity === 3 ? vscode.DiagnosticSeverity.Information :
            vscode.DiagnosticSeverity.Error
      );
      item.code = diag.code;
      item.source = diag.source || "qstar";
      return item;
    });
    this.diagnostics.set(uri, diagnostics);
  }

  didOpen(document) {
    if (!isQStarDocument(document)) {
      return;
    }
    this.notify("textDocument/didOpen", {
      textDocument: {
        uri: document.uri.toString(),
        languageId: "qstar",
        version: document.version,
        text: document.getText()
      }
    });
  }

  didChange(document) {
    if (!isQStarDocument(document)) {
      return;
    }
    this.notify("textDocument/didChange", {
      textDocument: {
        uri: document.uri.toString(),
        version: document.version
      },
      contentChanges: [
        {
          text: document.getText()
        }
      ]
    });
  }

  didSave(document) {
    if (!isQStarDocument(document)) {
      return;
    }
    this.notify("textDocument/didSave", {
      textDocument: {
        uri: document.uri.toString()
      },
      text: document.getText()
    });
  }

  async hover(document, position) {
    const result = await this.request("textDocument/hover", {
      textDocument: {
        uri: document.uri.toString()
      },
      position: {
        line: position.line,
        character: position.character
      }
    });
    if (!result || !result.contents) {
      return undefined;
    }
    const value = typeof result.contents === "string" ? result.contents : result.contents.value;
    return new vscode.Hover(new vscode.MarkdownString(value || ""));
  }

  async completion(document, position) {
    const result = await this.request("textDocument/completion", {
      textDocument: {
        uri: document.uri.toString()
      },
      position: {
        line: position.line,
        character: position.character
      }
    });
    const items = result && Array.isArray(result.items) ? result.items : [];
    return items.map(item => {
      const completion = new vscode.CompletionItem(item.label, vscode.CompletionItemKind.Property);
      completion.detail = item.detail;
      return completion;
    });
  }

  asRange(range) {
    if (!range) {
      return new vscode.Range(0, 0, 0, 1);
    }
    return new vscode.Range(
      range.start.line,
      range.start.character,
      range.end.line,
      range.end.character
    );
  }

  asLocation(location) {
    if (!location || !location.uri) {
      return undefined;
    }
    return new vscode.Location(vscode.Uri.parse(location.uri), this.asRange(location.range));
  }

  async definition(document, position) {
    const result = await this.request("textDocument/definition", {
      textDocument: {
        uri: document.uri.toString()
      },
      position: {
        line: position.line,
        character: position.character
      }
    });
    if (Array.isArray(result)) {
      return result.map(item => this.asLocation(item)).filter(Boolean);
    }
    return this.asLocation(result);
  }

  async references(document, position) {
    const result = await this.request("textDocument/references", {
      textDocument: {
        uri: document.uri.toString()
      },
      position: {
        line: position.line,
        character: position.character
      },
      context: {
        includeDeclaration: false
      }
    });
    return (Array.isArray(result) ? result : []).map(item => this.asLocation(item)).filter(Boolean);
  }

  async documentSymbols(document) {
    const result = await this.request("textDocument/documentSymbol", {
      textDocument: {
        uri: document.uri.toString()
      }
    });
    return (Array.isArray(result) ? result : []).map(item => new vscode.DocumentSymbol(
      item.name,
      item.detail || "",
      item.kind === 13 ? vscode.SymbolKind.Variable : vscode.SymbolKind.Function,
      this.asRange(item.range),
      this.asRange(item.selectionRange)
    ));
  }

  async workspaceSymbols(query) {
    const result = await this.request("workspace/symbol", {
      query: query || ""
    });
    return (Array.isArray(result) ? result : []).map(item => new vscode.SymbolInformation(
      item.name,
      item.kind === 13 ? vscode.SymbolKind.Variable : vscode.SymbolKind.Function,
      item.containerName || "qstar",
      this.asLocation(item.location)
    ));
  }

  async formatDocument(document) {
    const rootFile = findRootFile(document.uri.fsPath);
    const result = await execQStar(["fmt", "--stdout", document.uri.fsPath], rootFile);
    if (result.stdout === document.getText()) {
      return [];
    }
    const end = document.lineAt(document.lineCount - 1).range.end;
    return [vscode.TextEdit.replace(new vscode.Range(0, 0, end.line, end.character), result.stdout)];
  }
}

function isQStarDocument(document) {
  if (!document || document.uri.scheme !== "file") {
    return false;
  }
  if (document.languageId === "qstar") {
    return true;
  }
  const base = path.basename(document.uri.fsPath);
  return base === "qstar.lua" || base.endsWith(".qst");
}

function workspaceRoot() {
  const folder = vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders[0];
  return folder ? folder.uri.fsPath : process.cwd();
}

function workspaceRootUri() {
  const folder = vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders[0];
  return folder ? folder.uri.toString() : null;
}

function findRootFile(startPath) {
  let dir = fs.existsSync(startPath) && fs.statSync(startPath).isDirectory()
    ? startPath
    : path.dirname(startPath);
  const base = path.basename(startPath);
  if (base === "qstar.lua") {
    return startPath;
  }
  for (;;) {
    const rootEntry = path.join(dir, "qstar.lua");
    if (fs.existsSync(rootEntry)) {
      return rootEntry;
    }
    const parent = path.dirname(dir);
    if (parent === dir) {
      return rootEntry;
    }
    dir = parent;
  }
}

function shellQuote(value) {
  if (process.platform === "win32") {
    return `"${String(value).replace(/"/g, '\\"')}"`;
  }
  return `'${String(value).replace(/'/g, "'\\''")}'`;
}

function qstarExecutable() {
  return vscode.workspace.getConfiguration("qstar").get("server.path", "qstar");
}

function activeRootFile() {
  const editor = vscode.window.activeTextEditor;
  if (editor && isQStarDocument(editor.document)) {
    return findRootFile(editor.document.uri.fsPath);
  }
  const folder = workspaceRoot();
  return path.join(folder, "qstar.lua");
}

function terminalCommand(args) {
  const exe = shellQuote(qstarExecutable());
  const root = shellQuote(activeRootFile());
  return `${exe} --file ${root} ${args.join(" ")}`;
}

function runInTerminal(name, args) {
  const terminal = vscode.window.createTerminal(name);
  terminal.show();
  terminal.sendText(terminalCommand(args));
}

async function promptLabel(defaultValue) {
  return vscode.window.showInputBox({
    prompt: "QStar target label",
    value: defaultValue || "//:app"
  });
}

async function promptAction(defaultValue) {
  return vscode.window.showInputBox({
    prompt: "QStar action id",
    value: defaultValue || "//:app:compile:0"
  });
}

function execQStar(args, rootFile) {
  return new Promise((resolve, reject) => {
    const argv = ["--file", rootFile || activeRootFile()].concat(args);
    cp.execFile(qstarExecutable(), argv, { cwd: path.dirname(rootFile || activeRootFile()) }, (error, stdout, stderr) => {
      if (error) {
        error.qstarStdout = stdout;
        error.qstarStderr = stderr;
        reject(error);
        return;
      }
      resolve({ stdout, stderr });
    });
  });
}

function readLastBuildStatus(rootFile) {
  const summaryPath = path.join(path.dirname(rootFile), "build/qstar", "state", "last-summary.json");
  try {
    const summary = JSON.parse(fs.readFileSync(summaryPath, "utf8"));
    return summary.status || "unknown";
  } catch (_) {
    return "unknown";
  }
}

class QStarGraphNode extends vscode.TreeItem {
  constructor(label, collapsibleState, nodeKind, payload) {
    super(label, collapsibleState);
    this.nodeKind = nodeKind;
    this.payload = payload || null;
  }
}

class QStarGraphProvider {
  constructor(output) {
    this.output = output;
    this.emitter = new vscode.EventEmitter();
    this.onDidChangeTreeData = this.emitter.event;
    this.graph = null;
    this.lastStatus = "unknown";
    this.error = null;
  }

  refresh() {
    this.graph = null;
    this.error = null;
    this.emitter.fire();
  }

  async loadGraph() {
    if (this.graph || this.error) {
      return;
    }
    const rootFile = activeRootFile();
    this.lastStatus = readLastBuildStatus(rootFile);
    try {
      const result = await execQStar(["list-targets", "--format", "json"], rootFile);
      const graph = JSON.parse(result.stdout);
      if (graph.schema !== "qstar-targets-v1") {
        throw new Error(`unexpected qstar target schema: ${graph.schema || "<missing>"}`);
      }
      this.graph = graph;
    } catch (error) {
      this.error = error.qstarStderr || error.message;
      this.output.appendLine(`qstar graph load failed: ${this.error}`);
    }
  }

  getTreeItem(element) {
    return element;
  }

  async getChildren(element) {
    await this.loadGraph();
    if (this.error) {
      const item = new QStarGraphNode("QStar graph unavailable", vscode.TreeItemCollapsibleState.None, "error");
      item.description = this.error.split("\n")[0];
      item.iconPath = new vscode.ThemeIcon("error");
      return [item];
    }
    if (!this.graph) {
      return [];
    }
    if (!element) {
      const sections = [
        new QStarGraphNode("Targets", vscode.TreeItemCollapsibleState.Collapsed, "section-targets"),
        new QStarGraphNode("Generated Actions", vscode.TreeItemCollapsibleState.Collapsed, "section-generated"),
        new QStarGraphNode("Tests", vscode.TreeItemCollapsibleState.Collapsed, "section-tests"),
        new QStarGraphNode("Installable Artifacts", vscode.TreeItemCollapsibleState.Collapsed, "section-installable")
      ];
      sections[0].description = `last build: ${this.lastStatus}`;
      sections[0].iconPath = new vscode.ThemeIcon(this.lastStatus === "success" ? "check" : this.lastStatus === "failure" ? "error" : "circle-outline");
      sections[1].iconPath = new vscode.ThemeIcon("gear");
      sections[2].iconPath = new vscode.ThemeIcon("beaker");
      sections[3].iconPath = new vscode.ThemeIcon("package");
      return sections;
    }
    if (element.nodeKind === "section-targets") {
      return (this.graph.targets || []).map(target => this.targetItem(target));
    }
    if (element.nodeKind === "section-generated") {
      return (this.graph.generated_actions || []).map(action => this.generatedItem(action));
    }
    if (element.nodeKind === "section-tests") {
      return (this.graph.targets || []).filter(target => target.is_test).map(target => this.targetItem(target));
    }
    if (element.nodeKind === "section-installable") {
      return (this.graph.targets || []).filter(target => target.installable).map(target => this.targetItem(target));
    }
    return [];
  }

  targetItem(target) {
    const item = new QStarGraphNode(target.label, vscode.TreeItemCollapsibleState.None, "target", target);
    item.description = target.kind;
    item.tooltip = `${target.label}\nkind: ${target.kind}\norigin: ${target.origin_file}:${target.origin_line}\nsources: ${(target.sources || []).join(", ")}`;
    item.contextValue = target.is_test ? "qstarTest" : "qstarTarget";
    item.iconPath = new vscode.ThemeIcon(target.is_test ? "beaker" : target.installable ? "package" : "symbol-method");
    item.command = {
      command: "qstar.explainTarget",
      title: "QStar: Explain Target",
      arguments: [item]
    };
    return item;
  }

  generatedItem(action) {
    const item = new QStarGraphNode(action.label, vscode.TreeItemCollapsibleState.None, "generated", action);
    item.description = action.config_header ? "configure_file" : "custom_target";
    item.tooltip = `${action.label}\norigin: ${action.origin_file}:${action.origin_line}\noutputs: ${(action.outputs || []).join(", ")}`;
    item.contextValue = "qstarGenerated";
    item.iconPath = new vscode.ThemeIcon("gear");
    return item;
  }
}

function nodeLabel(node, fallback) {
  if (node && node.payload && node.payload.label) {
    return node.payload.label;
  }
  return fallback;
}

async function targetLabelFromNode(node, fallback) {
  const label = nodeLabel(node, null);
  if (label) {
    return label;
  }
  return promptLabel(fallback || "//:app");
}

function activate(context) {
  const output = vscode.window.createOutputChannel("QStar");
  const client = new QStarLspClient(output);
  const selector = [{ language: "qstar", scheme: "file" }];
  const graphProvider = new QStarGraphProvider(output);

  client.start();
  for (const document of vscode.workspace.textDocuments) {
    client.didOpen(document);
  }

  context.subscriptions.push(output);
  context.subscriptions.push(client.diagnostics);
  context.subscriptions.push(vscode.window.registerTreeDataProvider("qstarGraph", graphProvider));
  context.subscriptions.push(vscode.workspace.onDidOpenTextDocument(doc => client.didOpen(doc)));
  context.subscriptions.push(vscode.workspace.onDidChangeTextDocument(event => client.didChange(event.document)));
  context.subscriptions.push(vscode.workspace.onDidSaveTextDocument(doc => client.didSave(doc)));
  context.subscriptions.push(vscode.workspace.onDidSaveTextDocument(doc => {
    if (isQStarDocument(doc)) {
      graphProvider.refresh();
    }
  }));
  context.subscriptions.push(vscode.workspace.onDidChangeConfiguration(event => {
    if (event.affectsConfiguration("qstar.server.path") || event.affectsConfiguration("qstar.trace.server")) {
      client.restart();
    }
  }));
  context.subscriptions.push(vscode.languages.registerHoverProvider(selector, {
    provideHover(document, position) {
      return client.hover(document, position);
    }
  }));
  context.subscriptions.push(vscode.languages.registerCompletionItemProvider(selector, {
    provideCompletionItems(document, position) {
      return client.completion(document, position);
    }
  }, ".", "q", "/", ":", "\""));
  context.subscriptions.push(vscode.languages.registerDefinitionProvider(selector, {
    provideDefinition(document, position) {
      return client.definition(document, position);
    }
  }));
  context.subscriptions.push(vscode.languages.registerReferenceProvider(selector, {
    provideReferences(document, position) {
      return client.references(document, position);
    }
  }));
  context.subscriptions.push(vscode.languages.registerDocumentSymbolProvider(selector, {
    provideDocumentSymbols(document) {
      return client.documentSymbols(document);
    }
  }));
  context.subscriptions.push(vscode.languages.registerWorkspaceSymbolProvider({
    provideWorkspaceSymbols(query) {
      return client.workspaceSymbols(query);
    }
  }));
  context.subscriptions.push(vscode.languages.registerDocumentFormattingEditProvider(selector, {
    provideDocumentFormattingEdits(document) {
      return client.formatDocument(document);
    }
  }));

  context.subscriptions.push(vscode.commands.registerCommand("qstar.checkWorkspace", () => {
    runInTerminal("QStar Check", ["check"]);
  }));
  context.subscriptions.push(vscode.commands.registerCommand("qstar.refreshGraph", () => {
    graphProvider.refresh();
  }));
  context.subscriptions.push(vscode.commands.registerCommand("qstar.explainTarget", async node => {
    const label = await targetLabelFromNode(node, "//:app");
    if (label) {
      runInTerminal("QStar Explain", ["explain", shellQuote(label)]);
    }
  }));
  context.subscriptions.push(vscode.commands.registerCommand("qstar.dryRunTarget", async node => {
    const label = await targetLabelFromNode(node, "//:app");
    if (label) {
      runInTerminal("QStar Dry Run", ["dry-run", shellQuote(label)]);
    }
  }));
  context.subscriptions.push(vscode.commands.registerCommand("qstar.listTargets", () => {
    runInTerminal("QStar List Targets", ["list-targets"]);
  }));
  context.subscriptions.push(vscode.commands.registerCommand("qstar.buildTarget", async node => {
    const label = await targetLabelFromNode(node, "//:app");
    if (label) {
      runInTerminal("QStar Build", ["build", shellQuote(label)]);
    }
  }));
  context.subscriptions.push(vscode.commands.registerCommand("qstar.testTarget", async node => {
    const label = await targetLabelFromNode(node, "//:unit");
    if (label) {
      runInTerminal("QStar Test", ["test", shellQuote(label)]);
    }
  }));
  context.subscriptions.push(vscode.commands.registerCommand("qstar.openActionLog", async () => {
    const action = await promptAction("//:app:compile:0");
    if (action) {
      runInTerminal("QStar Action Log", ["action-log", shellQuote(action)]);
    }
  }));
  context.subscriptions.push(vscode.commands.registerCommand("qstar.replayAction", async () => {
    const action = await promptAction("//:app:compile:0");
    if (action) {
      runInTerminal("QStar Replay", ["replay", shellQuote(action)]);
    }
  }));

  context.subscriptions.push({
    dispose() {
      client.stop();
    }
  });
}

function deactivate() {}

module.exports = {
  activate,
  deactivate
};
