# QStar VSCode Extension

This developer extension gives QStar authoring files a first editor surface.

Supported files:

- `qstar.lua`
- `*.qs`
- `qstar.workspace`

Features:

- syntax highlighting for QStar Lua-like files
- snippets for common target and generated-action forms
- diagnostics through `qstar lsp --stdio`
- hover for QStar API names, field names, and `//pkg:target` labels
- completion for QStar API names and target fields
- Explorer tree view for targets, generated actions, tests, and installable artifacts
- last build status from `.qstar/state/last-summary.json`
- terminal commands for check, explain, list, build, action-log, and replay

The language server is intentionally read-only. It performs lint/check style
graph evaluation and never runs `qstar build` by itself.

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

## Explorer View

The `QStar` Explorer view is backed by:

```txt
qstar --file qstar.lua list-targets --format json
```

It groups graph data into targets, generated actions, tests, and installable
artifacts. The view also reads `.qstar/state/last-summary.json` when present so
the editor can show the most recent build status without executing a build.
