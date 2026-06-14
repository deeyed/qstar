# QStar

English | [한국어](README.ko.md)

QStar is a standalone build system with a Lua-based project DSL, a fast native
executor named Stella, and an optional Ninja backend. It focuses on deterministic
build graphs, explicit command vectors, reusable target configuration, and
tooling-friendly diagnostics for C, C++, assembly, generated files, and
language-provider driven projects.

QStar is currently in beta. The current release-prep line is `v0.7.0-beta`,
with macOS arm64 and Linux x86_64 runtime tarballs. Linux assets
are produced only from the Ubuntu release workflow or a clean Linux x86_64 host,
after source validation, Ninja backend parity, extracted tarball smoke, and
Stella/Ninja medium performance artifact collection. Windows has a manual native
validation candidate, but no public asset or official host support yet. QStar
1.0 is reserved for a release that is validated across macOS, Linux, and
Windows. The `0.6.x-beta` line is reserved for release/package/documentation
hotfixes.

## Highlights

- One entrypoint: `qstar.lua`
- Lua DSL with safe imports: `qstar.import_file(...)` and
  `qstar.import_module(...)`
- Build rules for executables, static libraries, shared libraries, tests,
  custom targets, configure files, stage/install flows, and dependency-only
  groups
- Reusable `qstar.config` bundles for large projects with repeated compiler
  options
- Stella native executor with compact progress output
- Stella daemon beta opt-in workflow for repeated local builds and IDE read APIs
- Ninja backend with `-G ninja` for C/C++/ASM and generated/custom graph actions
- Cale source support through Stella language-provider process actions
- `compile_commands.json` generation for editor tooling
- LSP and VSCode extension support for `qstar.lua`, `.qst`, and `.qsm`
- Docs, manpages, action logs, replay, and AI-oriented documentation index

## Install

Download the runtime tarball for your host from the
[GitHub Releases](https://github.com/deeyed/qstar/releases) page:

```sh
# macOS arm64
tar -xzf qstar-v0.7.0-beta-macos-arm64.tar.gz -C "$HOME/.local"
export PATH="$HOME/.local/bin:$PATH"
qstar --version

# Linux x86_64
tar -xzf qstar-v0.7.0-beta-linux-x86_64.tar.gz -C "$HOME/.local"
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

Maintainers can verify the uploaded GitHub release asset, not just the local
source-tree tarball, with:

```sh
make qstar-public-beta-download-smoke
```

## Quick Start

Create and build a small C application:

```sh
qstar init c-app hello
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
`qstar build` path still uses normal Stella; daemon residency is explicit.

```sh
qstar daemon --socket build/qstar/stella/daemon/qstar-daemon.sock --start
qstar --file qstar.lua -B build/qstar build //:app --use-daemon=auto --daemon-socket build/qstar/stella/daemon/qstar-daemon.sock
qstar --file qstar.lua -B build/qstar daemon --socket build/qstar/stella/daemon/qstar-daemon.sock --query targets.list
qstar daemon --socket build/qstar/stella/daemon/qstar-daemon.sock --stop
```

The read API is intended for IDE/AI tooling and currently exposes `hello`,
`workspace.info`, `targets.list`, `diagnostics.list`, `compile_commands.path`,
and `build.summary`. Socket, pid, and lock files are local-only and owner-only;
Windows named pipe support remains deferred.

## Authoring Surface

QStar projects use `qstar.lua` at the package root. Subdirectory fragments use
`.qst`, and helper modules use `.qsm`.

```lua
qstar.import_file("qstar/policies/freestanding.qst")
local paths = qstar.import_module("qstar/modules/paths")

qstar.config "common_c" {
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
qstar.custom_target "image" {
  inputs = {qstar.target_file("//:kernel")},
  outputs = {qstar.output("generated/kernel.img")},
  command = qstar.cli {
    "llvm-objcopy",
    "-O", "binary",
    qstar.input(0),
    qstar.output(0),
  },
}
```

## Common Commands

```sh
qstar --version
qstar docs
qstar docs --path
qstar docs --ai-index
qstar docs --show reference/qstar-lua.md

qstar init c-app hello
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
qstar install //:app --prefix /tmp/qstar-install
qstar stage //:image --dry-run
qstar why-rebuild //:app
qstar clean --target //:app
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
| Windows | Manual native CI alpha through MSYS2 UCRT64; no public asset yet |

QStar can model cross-compilation targets today, including freestanding and
firmware-style projects, but official host support is intentionally conservative.
The 1.0 milestone requires validated release artifacts and CI coverage for
macOS, Linux, and Windows.

## Documentation

- [GitHub Wiki](https://github.com/deeyed/qstar/wiki)
- [AI Index](wiki/AI_INDEX.md)
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
