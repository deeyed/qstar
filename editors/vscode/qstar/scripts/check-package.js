"use strict";

const fs = require("fs");
const path = require("path");

const root = path.resolve(process.argv[2] || path.join(__dirname, ".."));
const packagePath = path.join(root, "package.json");

function fail(message) {
  console.error(`qstar-vscode-package-check: ${message}`);
  process.exit(1);
}

function readJson(rel) {
  const file = path.join(root, rel);
  try {
    return JSON.parse(fs.readFileSync(file, "utf8"));
  } catch (err) {
    fail(`cannot parse ${rel}: ${err.message}`);
  }
}

function requireFile(rel) {
  const file = path.join(root, rel);
  if (!fs.existsSync(file) || !fs.statSync(file).isFile()) {
    fail(`missing package file ${rel}`);
  }
}

function requireDir(rel) {
  const file = path.join(root, rel);
  if (!fs.existsSync(file) || !fs.statSync(file).isDirectory()) {
    fail(`missing package directory ${rel}`);
  }
}

function walk(dir, out) {
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const full = path.join(dir, entry.name);
    const rel = path.relative(root, full).split(path.sep).join("/");
    if (entry.isDirectory()) {
      if (entry.name === "node_modules") {
        fail(`node_modules must not be present in extension tree: ${rel}`);
      }
      walk(full, out);
    } else if (entry.isFile()) {
      out.push(rel);
      if (entry.name.endsWith(".vsix")) {
        fail(`VSIX release artifact must not be committed: ${rel}`);
      }
    }
  }
}

const pkg = readJson("package.json");

if (pkg.name !== "qstar-vscode") fail("package name drifted");
if (pkg.main !== "./extension.js") fail("main must stay ./extension.js");
if (!pkg.private) fail("extension package must stay private in-repo");
if (!pkg.scripts || pkg.scripts.check !== "node scripts/check-package.js") {
  fail("missing stable package check script");
}
if (!pkg.scripts || pkg.scripts["package:vsix"] !== "sh scripts/package-vsix.sh") {
  fail("missing stable package:vsix script");
}

const files = new Set(pkg.files || []);
for (const required of [
  ".vscodeignore",
  "README.md",
  "extension.js",
  "language-configuration.json",
  "package.json",
  "samples/**",
  "scripts/**",
  "snippets/**",
  "syntaxes/**",
]) {
  if (!files.has(required)) fail(`package files list missing ${required}`);
}
for (const forbidden of ["node_modules", "node_modules/**", "*.vsix", "dist/**"]) {
  if (files.has(forbidden)) fail(`package files list must not include ${forbidden}`);
}

const languages = (((pkg.contributes || {}).languages) || []);
const qstarLang = languages.find((lang) => lang.id === "qstar");
if (!qstarLang) fail("missing qstar language contribution");
if (!new Set(qstarLang.extensions || []).has(".qs")) fail("missing .qs association");
const filenames = new Set(qstarLang.filenames || []);
if (!filenames.has("qstar.lua")) fail("missing qstar.lua association");
if (!filenames.has("qstar.workspace")) fail("missing qstar.workspace association");

const views = (((pkg.contributes || {}).views) || {}).explorer || [];
if (!views.some((view) => view.id === "qstarGraph")) fail("missing qstarGraph view");
const commands = new Set((((pkg.contributes || {}).commands) || []).map((cmd) => cmd.command));
for (const command of [
  "qstar.checkWorkspace",
  "qstar.refreshGraph",
  "qstar.explainTarget",
  "qstar.listTargets",
  "qstar.dryRunTarget",
  "qstar.buildTarget",
  "qstar.testTarget",
  "qstar.openActionLog",
  "qstar.replayAction",
]) {
  if (!commands.has(command)) fail(`missing command ${command}`);
}

for (const rel of [
  "extension.js",
  "language-configuration.json",
  "syntaxes/qstar.tmLanguage.json",
  "snippets/qstar.json",
  "scripts/package-vsix.sh",
  "samples/workspace/qstar.lua",
  "samples/workspace/app/app.qs",
  "samples/workspace/lib/lib.qs",
  "samples/workspace/app/src/main.c",
  "samples/workspace/lib/include/sample/core.h",
  "samples/workspace/lib/src/core.c",
]) {
  requireFile(rel);
}
requireDir("samples/workspace");

readJson("language-configuration.json");
readJson("syntaxes/qstar.tmLanguage.json");
readJson("snippets/qstar.json");

const allFiles = [];
walk(root, allFiles);
if (!allFiles.includes("README.md")) fail("walk sanity failed");

console.log("qstar-vscode-package-check: ok");
