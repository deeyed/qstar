# Ninja Backend Parity

Round Q93 treats the Ninja backend as a practical backend candidate for ordinary
C/C++/ASM projects while keeping unsupported surface explicit.

## Supported Lowering

The Ninja backend lowers:

- C/C++/ASM compile actions
- `qstar.staticlib`
- `qstar.executable`
- `qstar.test` build plus `qstar test -G ninja`
- `qstar.configure_file`
- `qstar.custom_target`
- `qstar.run_target` wrapper actions
- `qstar.group` phony aliases
- `qstar.sharedlib` for Darwin-like and Linux-like profiles
- `compile_commands.json` according to project policy

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
for Windows-like `qstar.sharedlib` profiles until `.dll`, import library, PDB,
runtime search path, and install layout behavior are validated on Windows.

Cale source lowering through Ninja is also deferred. Use `-G stella` for Cale
process compilation.

## Regression Gate

Run the dedicated gate with:

```sh
make qstar-ninja-backend-parity-tests
```

The gate checks staticlib, sharedlib, executable, test, generated actions,
configure file, run target marker handling, stage/install producer integration,
action-log and replay compatibility, Windows sharedlib diagnostics, and that `.ninja_log` /
`.ninja_deps` stay under the QStar build directory rather than the package root.

Manual corpus commands:

```sh
./build/bin/qstar -G ninja --file tests/corpus/c-app/qstar.lua build //:app
./build/bin/qstar -G ninja --file tests/corpus/generated/qstar.lua build //:all
```
