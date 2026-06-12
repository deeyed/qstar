# QStar v0.4 Stella Workflow Seal

QStar v0.4는 standalone QStar가 Stella 기본 executor와 Ninja comparison backend를
함께 제공하는 release/install seal이다. 이 문서는 Round 90 기준으로 runtime,
installed docs/manpages, editor package, and backend workflow가 같은 surface를
가리키는지 고정한다.

```txt
status: v0.4 Stella workflow seal
runtime version: qstar 0.4.0-beta.1
extension package: qstar-vscode 0.3.0
release gate: make -C qstar qstar-v0.4-pilot-tests
```

## Stable Surface

- `qstar.lua` is the single project entrypoint.
- Mandatory TOML config paths remain removed.
- `.qst` files declare graph fragments and targets.
- `.qsm` files are side-effect-free helper modules loaded with
  `qstar.import_module("folder/path")`.
- `qstar.import_file("path.qst")` imports graph fragments explicitly.
- `qstar.group` represents deps-only aggregate targets.
- `qstar.config` plus target `configs = {...}` provides reusable mergeable
  authoring policy.
- `qstar.project.generated_dir` may place generated artifacts under
  `build/qstar/generated`.
- `-G stella|ninja|auto` selects the effective generator. `auto` resolves to
  `stella`.
- `-B path` overrides the effective build directory.
- Compact progress output is the default; detailed action logs stay behind
  `--verbose` or `--schedule-trace`.
- `qstar emit-ninja` and `qstar -G ninja build` lower supported C/C++/ASM,
  generated, executable/test, run/stage/install producer workflows into the
  Ninja comparison path.

## Install Seal

The local install gate must prove that runtime, docs, and manpages move
together:

```txt
make -C qstar install PREFIX=$HOME/.local
$HOME/.local/bin/qstar --version
$HOME/.local/bin/qstar docs --ai-index
man qstar
man qstar-lua
```

On Darwin, `make install` must ad-hoc sign the installed binary after copy:

```txt
codesign -dv --verbose=2 $HOME/.local/bin/qstar
```

The installed wiki root is `$HOME/.local/share/doc/qstar/wiki`, and the manpages
are installed under `$HOME/.local/share/man/man1/qstar.1` and
`$HOME/.local/share/man/man5/qstar-lua.5`.

## Editor Seal

The VSCode extension package is versioned independently from the runtime. For
v0.4, the package version is `0.3.0`.

```txt
cd qstar/editors/vscode/qstar
npm run check
npm run package:vsix
code --install-extension dist/qstar-vscode-0.3.0.vsix --force
```

The extension must keep syntax highlighting, snippets, file associations,
LSP client wiring, target tree/query/build/test/log/replay command palette, and
`.qsm` module association aligned with the QStar runtime.

## Performance Seal

`qstar-v0.4-pilot-tests` includes the medium project readiness corpus. The gate
compares Stella and Ninja clean/no-op build timings on a generic low-level
project shape without naming a downstream project. Stella does not need to be a
byte-for-byte Ninja clone, but no-op and clean build overhead must remain close
enough that Stella is a credible primary backend for medium projects.

## Self-host Seal

The Makefile remains QStar's canonical bootstrap and release build path. The
root `qstar.lua` is a parallel self-host validation graph that proves the
Makefile-built QStar binary can build QStar again through Stella and through the
Ninja backend.

```txt
make -C qstar qstar-self-host-tests
```

This gate must not replace `make all`, `make check`, or `make install`. Release
artifacts continue to use the Makefile-built binary until the self-host path has
additional release-cycle history.

## Deferred Surface

- Remote package resolution and lockfile management.
- Full Cale source lowering through Ninja when the Cale provider is not a
  process invocation.
- Shared library policy beyond the current check/diagnostic surface.
- C++ modules execution policy.
- Board-specific builtins. Firmware image and smoke workflows stay expressed
  through generic targets, custom actions, run targets, and stages.
