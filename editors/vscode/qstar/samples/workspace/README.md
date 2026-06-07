# QStar VSCode Sample Workspace

This workspace is a tiny authoring corpus for the QStar VSCode extension.
Open this directory in VSCode after building QStar, then set
`qstar.server.path` to the absolute path of `qstar/build/bin/qstar`.

Useful commands:

```txt
qstar --file qstar.lua lint
qstar --file qstar.lua list-targets --format json
qstar --file qstar.lua explain //app:app
qstar --file qstar.lua dry-run //app:app
qstar --file qstar.lua build //app:app
```

The sample intentionally uses the canonical fragment names:

- root: `qstar.lua`
- app fragment: `app/app.qs`
- library fragment: `lib/lib.qs`
