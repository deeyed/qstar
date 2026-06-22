# QStar

English | [한국어](README.ko.md)

QStar is a standalone build system with a Lua-based project DSL, a fast native
executor named Stella, and an optional Ninja backend. It focuses on deterministic
build graphs, explicit command vectors, reusable target configuration, and
tooling-friendly diagnostics for C, C++, assembly, generated files, and
external object artifact flows. Language providers can be activated with
`qstar.use_language(...)`; QStar ships standard Zig, Rust, and CUDA providers
and also accepts project-local provider packages. Provider source units lower through the same
Stella/Ninja backend action contract.

QStar is currently in beta. The current release-prep line is `v0.7.19-beta`,
with macOS arm64 and Linux x86_64 runtime tarballs. Linux assets
are produced only from the Ubuntu release workflow or a clean Linux x86_64 host,
after source validation, Ninja backend parity, extracted tarball smoke, and
Stella/Ninja medium performance artifact collection. Performance numbers are
report-only release inputs, not stable guarantees. Windows has a manual native
validation candidate that builds, extracts, and smoke-tests a `windows-x86_64`
public beta candidate zip asset in GitHub Actions. The same manual workflow has
an opt-in `publish_windows_asset=true` job that publishes and download-smokes the
Windows zip for a release tag. Windows is still beta until that hosted evidence
is green for the selected release.
QStar 1.0 is reserved for a release that is validated across macOS, Linux, and
Windows. The `0.7.x-beta` line carries release/package/documentation patch
updates while the next feature line is prepared.
The v1 gap checklist is tracked in [docs/qstar-v1-readiness.md](docs/qstar-v1-readiness.md).

## Highlights

- One entrypoint: `qstar.lua`
- Lua DSL with safe imports: `qstar.import_file(...)` and
  `qstar.import_module(...)`
- Build rules for executables, static libraries, shared libraries, tests,
  custom targets, configure files, stage/install flows, and dependency-only
  groups
- `qstar.toolset` declarations for compiler, archive, link, response-file, and
  external tool policy
- `qstar.use_language(...)` activation for bundled or project-local language
  provider namespaces
- Reusable `qstar.config` bundles for large projects with repeated compiler
  options
- Stella native executor with compact progress output
- Stella daemon beta opt-in workflow for repeated local builds and IDE read APIs
- Ninja backend with `-G ninja` for C/C++/ASM and generated/custom graph actions
- `compile_commands.json` generation for editor tooling
- LSP and VSCode extension support for `qstar.lua`, `.qst`, and `.qsm`
- Docs, manpages, action logs, replay, and AI-oriented documentation index

## Install

Download the runtime tarball for your host from the
[GitHub Releases](https://github.com/deeyed/qstar/releases) page:

```sh
# macOS arm64
tar -xzf qstar-v0.7.19-beta-macos-arm64.tar.gz -C "$HOME/.local"
export PATH="$HOME/.local/bin:$PATH"
qstar --version

# Linux x86_64
tar -xzf qstar-v0.7.19-beta-linux-x86_64.tar.gz -C "$HOME/.local"
export PATH="$HOME/.local/bin:$PATH"
qstar --version
```

To build from source:

```sh
git clone --recurse-submodules https://github.com/deeyed/qstar.git
cd qstar
make all
build/bin/qstar --version
```

If the repository was cloned without submodules, initialize the vendored Lua
runtime before building:

```sh
git submodule update --init --recursive
```

The Makefile remains the canonical bootstrap and release build path. QStar also
ships a self-host graph for backend parity validation:

```sh
make qstar-self-host-tests
build/bin/qstar --file qstar.lua -B build/qstar-self build //:qstar
build/bin/qstar --file qstar.lua -B build/qstar-self-ninja -G ninja build //:qstar
```

When real Rust and Zig compilers are available, maintainers can also run the
optional GLP real-compiler gates:

```sh
make qstar-real-glp-compiler-corpus-tests
make qstar-real-language-init-scaffold-tests
```

The hosted Linux/macOS version of the real compiler corpus is an optional manual
GitHub Actions lane:

```sh
gh workflow run "Real GLP Compiler Validation"
```

Maintainers can verify the uploaded GitHub release asset, not just the local
source-tree tarball, with:

```sh
make qstar-public-beta-download-smoke
```

## Quick Start

Create and build a small C application:

```sh
qstar init app hello
cd hello
qstar build //:app --progress plain
./build/qstar/out/___app/app
```

A minimal `qstar.lua` looks like this:

```lua
qstar.project {
  name = "hello",
  version = "0.1.0",
  root = ".",
}

qstar.executable "app" {
  sources = {
    "src/main.c",
  },
}
```

Use `-B` to choose a build directory and `-G` to choose a backend:

```sh
qstar -B build/stella -G stella build //:app
qstar -B build/ninja -G ninja build //:app
```

## Stella Daemon

QStar also has a documented beta opt-in daemon workflow. The default
`qstar build` path still uses normal Stella; daemon residency is explicit and
is not a v1 stable/default-on surface.

```sh
qstar daemon --socket build/qstar/stella/daemon/qstar-daemon.sock --start
qstar --file qstar.lua -B build/qstar build //:app --use-daemon=auto --daemon-socket build/qstar/stella/daemon/qstar-daemon.sock
qstar --file qstar.lua -B build/qstar daemon --socket build/qstar/stella/daemon/qstar-daemon.sock --query targets.list
qstar daemon --socket build/qstar/stella/daemon/qstar-daemon.sock --stop
```

The read API is intended for IDE/AI tooling and currently exposes `hello`,
`workspace.info`, `targets.list`, `diagnostics.list`, `compile_commands.path`,
and `build.summary`. Socket, pid, and lock files are local-only and owner-only;
package/build identity mismatches are rejected instead of hidden by fallback.
Windows named pipe support remains deferred.

## Authoring Surface

QStar projects use `qstar.lua` at the package root. Subdirectory fragments use
`.qst`, and helper modules use `.qsm`.

```lua
qstar.toolset "host" {
  tools = {
    c = { compiler = qstar.cli {"cc"} },
    cxx = { compiler = qstar.cli {"c++"} },
    asm = { compiler = qstar.cli {"cc"} },
    archive = qstar.cli {"ar"},
    link = qstar.cli {"cc"},
  },
}

qstar.import_file("qstar/policies/common.qst")
local paths = qstar.import_module("qstar/modules/paths")

qstar.config "common_c" {
  toolset = "//:host",
  lang = {
    c = {
      public_include_dirs = {"include"},
      compile_options = {"-std=c23", "-Wall"},
    },
  },
}

qstar.staticlib "core" {
  configs = {"//qstar/policies:common_c"},
  sources = {"src/core.c"},
}

qstar.group "all_libs" {
  deps = {
    "//:core",
  },
}
```

Commands are argv vectors, not shell strings:

```lua
qstar.transform "image" {
  input = qstar.target_file("//:app"),
  output = qstar.output("generated/app.bin"),
  command = qstar.cli {
    "tools/package-object",
    qstar.input(0),
    qstar.output(0),
  },
  description = qstar.status("Packaging app.bin"),
}
```

Use `qstar.custom_target` when a generated action needs multiple inputs or outputs;
`qstar.transform` is single-input/single-output sugar over the same generated
artifact contract.

## Common Commands

```sh
qstar --version
qstar docs
qstar docs --path
qstar docs --ai-index
qstar docs --show reference/qstar-lua.md
qstar docs --show reference/generic-workflows.md

qstar init app hello
qstar check //...
qstar lint //...
qstar fmt --check qstar.lua
qstar list-targets --format json
qstar query //:app
qstar doctor
qstar explain //:app
qstar dry-run //:app
qstar emit-ninja //:app
qstar build //:app
qstar test //...
qstar commands
qstar install --out exports/install  # project command declared by qstar.lua
qstar package-local --out exports/package  # project command declared by qstar.lua
qstar stage //:image --dry-run
qstar why-rebuild //:app
qstar clean //:app
qstar log //:app
qstar last-failure
qstar action-log <action-id>
qstar replay <action-id>
```

## Platform Status

| Host platform | Status |
| --- | --- |
| macOS arm64 | 0.7 beta release-prep artifact |
| Linux x86_64 | 0.7 beta release-prep artifact from Ubuntu release workflow or clean Linux host |
| Windows | Validation-backed beta candidate through MSYS2 UCRT64; Actions builds/extracts the `windows-x86_64` public beta candidate zip; optional GitHub Release publish/download smoke gate exists |

QStar can model custom toolchains and cross-compilation targets through explicit
toolsets, configs, argv-vector commands, language provider source units, and
object artifact bridges. Official
host support is intentionally conservative: the Windows beta candidate lane
validates source build, execution corpus, install/stage layout, and sharedlib
runtime/import artifacts plus a generated/extracted public beta candidate zip,
and the Q253 manual publish job can verify the uploaded zip from GitHub Release.
Windows is not official support until that release-backed evidence is green.
The 1.0 milestone requires
validated release artifacts and CI coverage for macOS, Linux, and Windows.
The exact remaining blockers and stable-surface policy are tracked in
[docs/qstar-v1-readiness.md](docs/qstar-v1-readiness.md).

## Documentation

- [GitHub Wiki](https://github.com/deeyed/qstar/wiki)
- [AI Index](wiki/AI_INDEX.md)
- [v1 readiness gap report](docs/qstar-v1-readiness.md)
- [Getting Started](wiki/getting-started.md)
- [QStar Lua Reference](wiki/reference/qstar-lua.md)
- [Modules and Imports](wiki/reference/modules.md)
- [Reusable Configs](wiki/reference/configs.md)
- [CMake migration notes](wiki/migration/from-cmake.md)

## License

QStar is licensed under the Apache License, Version 2.0. See
[LICENSE.md](LICENSE.md).

QStar vendors Lua as a submodule. The Lua license notice is preserved in
[LICENSE/lua.txt](LICENSE/lua.txt).
