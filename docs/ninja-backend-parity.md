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
- `qstar.run_target` wrapper actions
- `qstar.group` phony aliases
- `qstar.sharedlib` for macOS and Linux platform contexts
- `compile_commands.json` according to project policy

When an executable, test, or shared library links against a QStar `sharedlib`
dependency, Ninja lowering emits the same build-tree runtime search path policy
as Stella: macOS platform contexts use `@loader_path` relative rpaths and Linux
platform contexts use `$ORIGIN` relative rpaths. This allows the freshly built artifact
to run from `build/qstar/out/...` without requiring a prefix install first.

Ninja action ids are written into `build.ninja` as `qstar_action_id` variables
and as `# qstar-action-id:` comments. QStar also writes backend action logs so
`qstar action-log <action-id>` and `qstar replay <action-id>` work for emitted
compile, archive, link, generate, and run actions.

## QStar-Owned Work

`stage` and `install` remain QStar-owned copy/manifest operations. When the
effective generator is `ninja`, QStar first builds referenced target artifacts
through Ninja and then performs copy, diff, manifest, and install layout work
itself. This keeps package/stage semantics in one implementation while still
letting Ninja produce the artifacts.

## Deferred Surface

Windows shared library policy remains deferred. QStar emits a stable diagnostic
for Windows `qstar.sharedlib` targets until `.dll`, import library, PDB,
runtime search path, and install layout behavior are validated on Windows.

Unsupported source-language lowering through Ninja is not a separate provider
contract. Use the object artifact bridge: a `qstar.custom_target` produces
`qstar.output(path, {format = "object"})`, and the consuming target lists that
generated object in `sources`.

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
`tests/projects/object-artifact-bridge`, action-log and replay compatibility,
Windows sharedlib diagnostics, explicit Windows static `.lib` lowering in the
Windows prep corpus, and that `.ninja_log` / `.ninja_deps` stay under the QStar
build directory rather than the package root.

Manual corpus commands:

```sh
./build/bin/qstar -G ninja --file tests/corpus/c-app/qstar.lua build //:app
./build/bin/qstar -G ninja --file tests/corpus/generated/qstar.lua build //:all
./build/bin/qstar -G ninja --file tests/projects/object-artifact-bridge/qstar.lua build //:all
```
