# Generic Command Artifact Workflow Corpus

This corpus seals QStar's generic artifact workflow surface without naming a
specific language, operating system, emulator, board, package format, or image
format.

It verifies this flow:

```text
plain input artifact
-> qstar.transform generated artifact
-> qstar.stage copy-only layout
-> qstar.run_target with first-class inputs
-> qstar.command with typed options, bool argv helpers, and export_stage
-> project-defined install/export/package commands
```

The fixture is intentionally tool-agnostic. `tools/transform-artifact.sh` copies
one package file to one generated artifact, and `tools/check-workflow.sh`
validates that an artifact and staged layout are visible to the action that
declares them as inputs.

Useful commands:

```sh
qstar --file qstar.lua explain //:artifact_smoke
qstar --file qstar.lua build //:artifact_smoke
qstar --file qstar.lua workflow --out exports/local --mode full
qstar --file qstar.lua install --out exports/install
qstar --file qstar.lua install-local --out exports/install-local
qstar --file qstar.lua package-local --out exports/package
qstar --file qstar.lua export-local --out exports/local
qstar --file qstar.lua -G ninja build //:artifact_smoke
qstar --file qstar.lua -G ninja install --out exports/ninja-install
```

`install` is not a QStar built-in command in this fixture. It is a root
`qstar.command` that exports `//:install_layout` through
`qstar.step.export_stage`, exactly like `package-local` and `export-local`.
