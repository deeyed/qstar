"use strict";

const fs = require("fs");
const path = require("path");
const childProcess = require("child_process");

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

function trackedFilesUnderRoot() {
  try {
    const toplevel = childProcess
      .execFileSync("git", ["-C", root, "rev-parse", "--show-toplevel"], { encoding: "utf8" })
      .trim();
    const prefix = path.relative(toplevel, root).split(path.sep).join("/");
    const out = childProcess.execFileSync("git", ["-C", toplevel, "ls-files"], {
      encoding: "utf8",
    });
    return out
      .split(/\r?\n/)
      .filter(Boolean)
      .filter((file) => file === prefix || file.startsWith(`${prefix}/`))
      .map((file) => file.slice(prefix.length + 1));
  } catch (_err) {
    return [];
  }
}

function walk(dir, out) {
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const full = path.join(dir, entry.name);
    const rel = path.relative(root, full).split(path.sep).join("/");
    if (entry.isDirectory()) {
      if (entry.name === "node_modules" || entry.name === "dist") {
        continue;
      }
      walk(full, out);
    } else if (entry.isFile()) {
      out.push(rel);
    }
  }
}

const pkg = readJson("package.json");
const sourceLogoPath = path.resolve(root, "../../../assets/qstar_logo.png");
const sourceLogoSvgPath = path.resolve(root, "../../../assets/qstar_logo.svg");

if (pkg.name !== "qstar-vscode") fail("package name drifted");
if (pkg.version !== "0.3.0") fail("extension package version must be 0.3.0 for QStar v0.4 seal");
if (pkg.main !== "./extension.js") fail("main must stay ./extension.js");
if (!pkg.private) fail("extension package must stay private in-repo");
if (!pkg.repository || pkg.repository.directory !== "qstar/editors/vscode/qstar") {
  fail("repository metadata must point at the extension subdirectory");
}
if (pkg.icon !== "media/qstar_logo.png") fail("extension icon must use QStar logo");
if (!pkg.scripts || pkg.scripts.check !== "node scripts/check-package.js") {
  fail("missing stable package check script");
}
if (!pkg.scripts || pkg.scripts["package:vsix"] !== "sh scripts/package-vsix.sh") {
  fail("missing stable package:vsix script");
}

if (Object.prototype.hasOwnProperty.call(pkg, "files")) {
  fail("VSCE cannot combine package.json files with .vscodeignore");
}

const vscodeIgnore = fs.readFileSync(path.join(root, ".vscodeignore"), "utf8");
for (const requiredPattern of [
  "*.vsix",
  "dist/",
  "node_modules/",
  "build/qstar/",
  "**/build/qstar/**",
  "build/",
  "**/build/**",
  "compile_commands.json",
  "**/compile_commands.json",
  ".gitignore",
]) {
  if (!vscodeIgnore.includes(requiredPattern)) {
    fail(`.vscodeignore missing ${requiredPattern}`);
  }
}

const languages = (((pkg.contributes || {}).languages) || []);
const qstarLang = languages.find((lang) => lang.id === "qstar");
if (!qstarLang) fail("missing qstar language contribution");
if (!new Set(qstarLang.extensions || []).has(".qst")) fail("missing .qst association");
if (!new Set(qstarLang.extensions || []).has(".qsm")) fail("missing .qsm association");
if (new Set(qstarLang.extensions || []).has(".qs")) fail(".qs association must stay removed");
if (!new Set(qstarLang.filenamePatterns || []).has("*.qst")) {
  fail("missing *.qst filename pattern association");
}
if (!new Set(qstarLang.filenamePatterns || []).has("*.qsm")) {
  fail("missing *.qsm filename pattern association");
}
if (new Set(qstarLang.filenamePatterns || []).has("*.qs")) {
  fail("*.qs filename pattern association must stay removed");
}
const filenames = new Set(qstarLang.filenames || []);
if (!filenames.has("qstar.lua")) fail("missing qstar.lua association");
if (filenames.has("qstar.workspace")) fail("qstar.workspace association must stay removed");
if (!qstarLang.icon || qstarLang.icon.light !== "./media/qstar_logo.svg" ||
    qstarLang.icon.dark !== "./media/qstar_logo.svg") {
  fail("qstar language contribution must use QStar logo");
}

const iconThemes = (((pkg.contributes || {}).iconThemes) || []);
const qstarIconTheme = iconThemes.find((theme) => theme.id === "qstar-file-icons");
if (!qstarIconTheme) fail("missing qstar file icon theme");
if (qstarIconTheme.path !== "./icons/qstar-icon-theme.json") {
  fail("qstar file icon theme path drifted");
}
const iconTheme = readJson("icons/qstar-icon-theme.json");
if (!iconTheme.iconDefinitions || !iconTheme.iconDefinitions._qstar_file ||
    iconTheme.iconDefinitions._qstar_file.iconPath !== "../media/qstar_logo.svg") {
  fail("qstar file icon theme must reference packaged QStar logo");
}
if (!iconTheme.fileExtensions || iconTheme.fileExtensions.qst !== "_qstar_file" ||
    iconTheme.fileExtensions.qsm !== "_qstar_file") {
  fail("qstar file icon theme must map .qst and .qsm");
}
if (iconTheme.fileExtensions.qs) fail("qstar file icon theme must not map .qs");
if (!iconTheme.fileNames || iconTheme.fileNames["qstar.lua"] !== "_qstar_file") {
  fail("qstar file icon theme must map qstar.lua");
}
if (iconTheme.fileNames["qstar.workspace"]) {
  fail("qstar file icon theme must not map qstar.workspace");
}
if (!iconTheme.languageIds || iconTheme.languageIds.qstar !== "_qstar_file") {
  fail("qstar file icon theme must map qstar language id");
}

const configDefaults = ((pkg.contributes || {}).configurationDefaults) || {};
const fileAssociations = configDefaults["files.associations"] || {};
if (fileAssociations["*.qst"] !== "qstar" ||
    fileAssociations["*.qsm"] !== "qstar" ||
    fileAssociations["qstar.lua"] !== "qstar") {
  fail("qstar extension must default QStar file associations");
}
if (fileAssociations["*.qs"] || fileAssociations["qstar.workspace"]) {
  fail("removed QStar file associations must stay removed");
}

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
  "media/qstar_logo.png",
  "media/qstar_logo.svg",
  "icons/qstar-icon-theme.json",
  "syntaxes/qstar.tmLanguage.json",
  "snippets/qstar.json",
  "scripts/package-vsix.sh",
  "samples/workspace/qstar.lua",
  "samples/workspace/app/app.qst",
  "samples/workspace/lib/lib.qst",
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

if (!fs.existsSync(sourceLogoPath)) {
  fail("missing source QStar logo at qstar/assets/qstar_logo.png");
}
if (!fs.existsSync(sourceLogoSvgPath)) {
  fail("missing source QStar logo at qstar/assets/qstar_logo.svg");
}
if (!fs.readFileSync(path.join(root, "media/qstar_logo.png")).equals(
    fs.readFileSync(sourceLogoPath))) {
  fail("packaged QStar logo must match qstar/assets/qstar_logo.png");
}
if (!fs.readFileSync(path.join(root, "media/qstar_logo.svg")).equals(
    fs.readFileSync(sourceLogoSvgPath))) {
  fail("packaged QStar SVG logo must match qstar/assets/qstar_logo.svg");
}

const allFiles = [];
walk(root, allFiles);
if (!allFiles.includes("README.md")) fail("walk sanity failed");
for (const rel of trackedFilesUnderRoot()) {
  if (rel.endsWith(".vsix")) fail(`VSIX release artifact must not be committed: ${rel}`);
  if (rel === "node_modules" || rel.startsWith("node_modules/")) {
    fail(`node_modules must not be committed under extension tree: ${rel}`);
  }
  if (rel === "dist" || rel.startsWith("dist/")) {
    fail(`dist artifacts must not be committed under extension tree: ${rel}`);
  }
}

console.log("qstar-vscode-package-check: ok");
