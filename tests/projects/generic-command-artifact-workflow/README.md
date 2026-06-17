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
qstar --file qstar.lua -G ninja build //:artifact_smoke
qstar --file qstar.lua -G ninja workflow --out exports/ninja
```
