# QStar VSCode Extension

This developer extension gives QStar authoring files a first editor surface.

Supported files:

- `qstar.lua`
- `*.qst`
- `*.qsm`

Features:

- syntax highlighting for QStar Lua-like files
- QStar extension icon, language icon, and optional QStar file icon theme
- snippets for common target and generated-action forms
- diagnostics through `qstar lsp --stdio`
- hover for QStar API names, field names, and `//pkg:target` labels
- completion for QStar API names and target fields
- definition/references for target labels
- document/workspace symbols for targets and generated actions
- document formatting through `qstar fmt --stdout`
- Explorer tree view for targets, generated actions, tests, and installable artifacts
- last build status from `build/qstar/state/last-summary.json`
- terminal commands for check, explain, list, build, action-log, and replay

The language server is intentionally read-only. It performs lint/check style
graph evaluation and never runs `qstar build` by itself.

## File Icons

VSCode installations that already have Q# support may show legacy `.qs` files
with the Microsoft Q# icon. QStar no longer owns `.qs`; canonical fragments use
`.qst`. This extension contributes the QStar logo for the `qstar` language,
defaults `*.qst`, `*.qsm`, and `qstar.lua` to the `qstar` language id, and also ships a
fallback file icon theme named `QStar File Icons`.

If `.qst`, `.qsm`, or `qstar.lua` still show the wrong icon, run:

```txt
Preferences: File Icon Theme
```

and select `QStar File Icons`. The theme maps `.qst`, `.qsm`, `qstar.lua`, and the
`qstar` language id to `media/qstar_logo.svg`.

If another extension or user setting still wins the `.qst` or `.qsm` association, add this
workspace setting:

```json
{
  "files.associations": {
    "*.qst": "qstar",
    "*.qsm": "qstar"
  }
}
```

QStar keeps `.qst` as the canonical graph fragment suffix and `.qsm` as the
helper module suffix. `.qs` is intentionally left to other ecosystems and is
reported by `qstar lint` as a removed suffix.

## Development

Build the standalone QStar binary first:

```txt
make all
```

Then open this extension directory or the sample workspace in VSCode:

```txt
code editors/vscode/qstar
code editors/vscode/qstar/samples/workspace
```

Set `qstar.server.path` to the absolute path of `build/bin/qstar`.
The sample workspace is intentionally small and can be checked without
installing the extension:

```txt
build/bin/qstar --file editors/vscode/qstar/samples/workspace/qstar.lua lint
build/bin/qstar --file editors/vscode/qstar/samples/workspace/qstar.lua list-targets --format json
```

The checked-in package surface is validated by:

```txt
make vscode-extension-tests
```

## Settings

```json
{
  "qstar.server.path": "qstar",
  "qstar.trace.server": false
}
```

For local development from the repository checkout, set `qstar.server.path` to
the absolute path of `qstar/build/bin/qstar`.

## Commands

- `QStar: Check Workspace`
- `QStar: Refresh Graph`
- `QStar: Explain Target`
- `QStar: List Targets`
- `QStar: Dry Run Target`
- `QStar: Build Target`
- `QStar: Test Target`
- `QStar: Open Action Log`
- `QStar: Replay Action`

The build/log/replay commands are explicit terminal invocations. They are not
triggered by the LSP server.

## Formatting

The formatter is intentionally conservative. It delegates to:

```txt
qstar fmt --stdout path/to/file.qst
```

v1 only canonicalizes simple QStar target/action blocks. Use VSCode's built-in
`editor.formatOnSave` setting if you want format-on-save behavior.

## Explorer View

The `QStar` Explorer view is backed by:

```txt
qstar --file qstar.lua list-targets --format json
```

It groups graph data into targets, generated actions, tests, and installable
artifacts. The view also reads `build/qstar/state/last-summary.json` when present so
the editor can show the most recent build status without executing a build.

## Packaging

The extension is packageable as a VSIX release artifact, but `.vsix` files are
not committed to the repository. Install the VSCode package tool outside this
checkout, then run:

```txt
cd editors/vscode/qstar
npm run check
npm run package:vsix
```

The packaging script writes `dist/qstar-vscode-<version>.vsix`. Install it with:

```txt
code --install-extension dist/qstar-vscode-0.3.0.vsix --force
```

`node_modules/`, `dist/`, and `*.vsix` are local artifacts only. The extension
does not vendor npm dependencies.
