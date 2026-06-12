# QStar v0.4 Stella Workflow Seal

QStar v0.4는 standalone QStar가 Stella 기본 executor와 Ninja backend 후보를
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
  generated, staticlib, executable/test, run target, and group workflows into
  Ninja.
- `stage` and `install` remain QStar-owned copy/manifest operations, while
  referenced artifacts are produced by the effective generator.

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

## Linux Validation Seal

Round Q97 moves Linux from planned support to validation underway. The canonical
source-build and install smoke is documented in `docs/linux-validation.md`.

```txt
make -C qstar qstar-linux-validation-tests
make -C qstar install PREFIX=/tmp/qstar-linux-smoke
/tmp/qstar-linux-smoke/bin/qstar --version
```

On macOS this smoke only proves portable path/process behavior. A Linux release
asset requires the same gate on a clean Linux host or CI image, plus clang/gcc
depfile coverage and installed docs/manpage verification.

## Windows Prep Seal

Windows remains planned validation, not official support. Round Q98 records the
pre-port path/process/response-file contract in `docs/windows-path-process.md`.

```txt
make -C qstar qstar-windows-prep-tests
./build/bin/qstar --file tests/corpus/response-files/qstar.lua build //:all
```

QStar DSL package paths are slash-normalized and package-relative on every host.
Backslash paths and drive-letter package paths are rejected before the native
Windows port so that Graph IR, compile database, response files, and Ninja
lowering do not grow incompatible path dialects.

## Public Beta Packaging Seal

Round Q99 adds a public beta release gate around the Makefile-built runtime.
The package script keeps the release asset reproducible enough for local smoke
and GitHub prerelease publication.

```txt
make -C qstar qstar-public-beta-release-tests
```

This gate runs `tools/package-public-beta.sh`, installs QStar into a temporary
release root, verifies `qstar --version`, installed wiki/manpages, Darwin
codesign, macOS arm64 binary identity, prefix-style tarball layout, and
`SHA256SUMS`. The expected macOS beta asset is:

```txt
dist/release/qstar-v0.4.0-beta.1-macos-arm64.tar.gz
dist/release/SHA256SUMS
```

The runtime tarball contains `bin/qstar`, installed wiki, manpages, README files,
Apache-2.0 license, and Lua vendor license notice. VSCode `.vsix` packages are
not included in the runtime tarball; they remain separately versioned editor
artifacts.

Release procedure, wiki sync checklist, and GitHub publish commands live in
`docs/public-beta-release.md`. New release notes should start from
`docs/releases/TEMPLATE.md`.

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
compares Stella and Ninja clean/no-op/incremental build timings on a generic
low-level project shape without naming a downstream project. Stella does not
need to be a byte-for-byte Ninja clone, but no-op, incremental, and clean build
overhead must remain close enough that Stella is a credible primary backend for
medium projects. Round Q92 keeps timing thresholds report-only by default and
uses `QSTAR_MEDIUM_PERF_REPORT_ONLY=0` to promote them to a hard gate.

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
