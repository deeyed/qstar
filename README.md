# QStar

English | [한국어](README.ko.md)

QStar is a standalone build system with a Lua-based project DSL, a fast native
executor named Stella, and an optional Ninja backend. It focuses on deterministic
build graphs, explicit command vectors, reusable target configuration, and
tooling-friendly diagnostics for C, C++, assembly, generated files, and
language-provider driven projects.

QStar is currently in beta. The current public prerelease line is
`v0.5.0-beta.1`, with macOS arm64 binaries published first. Linux has a
validation-backed source build path and a `linux-x86_64` release-candidate
tarball dry-run through Ubuntu gcc/clang CI. Windows has a manual native
validation candidate, but no public asset or official host support yet. QStar
1.0 is reserved for a release that is validated across macOS, Linux, and Windows.

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
- Ninja backend with `-G ninja`
- `compile_commands.json` generation for editor tooling
- LSP and VSCode extension support for `qstar.lua`, `.qst`, and `.qsm`
- Docs, manpages, action logs, replay, and AI-oriented documentation index

## Install

Download the macOS arm64 tarball from the
[GitHub Releases](https://github.com/deeyed/qstar/releases) page:

```sh
tar -xzf qstar-v0.5.0-beta.1-macos-arm64.tar.gz -C "$HOME/.local"
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
| macOS arm64 | Beta release artifact |
| Linux x86_64 | Candidate tarball dry-run through Ubuntu gcc/clang CI; no public asset yet |
| Windows | Manual native validation candidate; no public asset yet |

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
