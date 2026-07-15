# Ninja Backend Parity

Round Q93 treats the Ninja backend as a practical backend candidate for ordinary
C/C++/ASM projects while keeping unsupported surface explicit.

## Supported Lowering

The Ninja backend lowers:

- C/C++/ASM compile actions
- generated object artifacts from `qstar.output(..., {format = "object"})`
- `qstar.staticlib`
- `qstar.executable`
- `qstar.test` build plus `qstar test -G ninja`
- `qstar.configure_file`
- `qstar.custom_target`
- `qstar.transform`
- `qstar.run_target` wrapper actions
- `qstar.group` phony aliases
- `qstar.interface`, `qstar.imported`, and `qstar.tool` dependency-only aliases
- transitive `compile_usage` and `link_usage` options/inputs
- `qstar.tool_file` executable producer edges for generated actions
- `qstar.sharedlib` for macOS, Linux, and Windows platform contexts
- `compile_commands.json` according to project policy

When an executable, test, or shared library links against a QStar `sharedlib`
dependency, Ninja lowering emits the same build-tree runtime search path policy
as Stella: macOS platform contexts use `@loader_path` relative rpaths and Linux
platform contexts use `$ORIGIN` relative rpaths. This allows the freshly built artifact
to run from `build/qstar/out/...` without requiring a prefix install first.
Windows shared-library dependencies link against the producer's import `.lib`;
the runtime `.dll` remains the primary artifact for `qstar.target_file(label)`,
stage, and project-defined export layouts.

Ninja action ids are written into `build.ninja` as `qstar_action_id` variables
and as `# qstar-action-id:` comments. QStar also writes backend action logs so
`qstar action-log <action-id>` and `qstar replay <action-id>` work for emitted
compile, archive, link, generate, and run actions.

## QStar-Owned Work

`stage` remains a QStar-owned copy/manifest operation. When the
effective generator is `ninja`, QStar first builds referenced target artifacts
through Ninja and then performs copy, diff, and manifest work itself. Projects
define install/package/deploy names through `qstar.command` and export layouts
through `qstar.step.export_stage`. This keeps stage semantics in one implementation while still
letting Ninja produce the artifacts.

Root project commands also use the effective generator for build-producing
steps. A command can build a generated artifact, materialize a stage, run a
generic check with `qstar.stage_dir(...)` inputs, and export the layout with
`qstar.step.export_stage` without falling back to shell scripts.

## Deferred Surface

Windows PDB/debug artifact ownership, MSVC `link.exe`/`lld-link` validation,
and general Windows runtime search path policy remain deferred. The runtime
`.dll` plus import `.lib` artifact model and consumer import-library link are
now part of the Ninja backend parity contract.

Provider source-language lowering uses the same provider action template as
Stella. The object artifact bridge remains available: a `qstar.custom_target`
produces `qstar.output(path, {format = "object"})`, and the consuming target
lists that generated object in `sources`.

## Regression Gate

Run the dedicated gate with:

```sh
make qstar-ninja-backend-parity-tests
```

Generic DSL release candidates also run the consolidated Q193 seal:

```sh
make qstar-generic-dsl-backend-parity-tests
```

The gate checks staticlib, sharedlib, sharedlib-linked executable/test,
generated actions, configure file, run target expect handling, sharedlib
stage/install producer integration, object artifact bridge parity through
`tests/projects/object-artifact-bridge`, generic command/artifact workflow
parity through `tests/projects/generic-command-artifact-workflow`, action-log
and replay compatibility,
Windows sharedlib multi-output lowering and import-library consumer links,
explicit Windows static `.lib` lowering in the Windows prep corpus, and that
`.ninja_log` / `.ninja_deps` stay under the QStar build directory rather than
the package root.

Manual corpus commands:

```sh
./build/bin/qstar -G ninja --file tests/corpus/c-app/qstar.lua build //:app
./build/bin/qstar -G ninja --file tests/corpus/generated/qstar.lua build //:all
./build/bin/qstar -G ninja --file tests/projects/object-artifact-bridge/qstar.lua build //:all
./build/bin/qstar -G ninja --file tests/projects/generic-command-artifact-workflow/qstar.lua workflow --out exports/ninja
```
