# QStar Rule Model

QStar target rules describe build graph edges, not domain-specific artifact
semantics.

Supported artifact rules:

- `qstar.executable`
- `qstar.staticlib`
- `qstar.sharedlib`
- `qstar.test`

Utility rules:

- `qstar.custom_target`
- `qstar.configure_file`
- `qstar.run_target`
- `qstar.group`
- `qstar.stage`

Link policy:

- `link_options` are appended to the link argv exactly as authored.
- `link_inputs` are tracked as rebuild inputs without being appended to argv.
- macOS frameworks are written under `link.frameworks` in a host branch.
- Shared library consumers receive build-tree runtime search paths on macOS and
  Linux host policies.

Generated object bridge:

```lua
qstar.custom_target "foreign_object" {
  inputs = {"src/foreign_source.ext"},
  outputs = {qstar.output("build/qstar/generated/foreign.o", {format = "object"})},
  command = qstar.cli {"tools/compile-foreign.sh", qstar.input(0), qstar.output(0)},
}
```

The consuming target lists the generated object in `sources`. QStar does not
parse or own the external source language.
