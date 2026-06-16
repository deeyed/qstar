# QStar Syntax Cheat Sheet

`wiki/reference/qstar-lua.md` is the canonical user-facing syntax reference.
This file is a short developer-side drift guard for the current generic DSL.

## Entrypoints

```lua
qstar.project {
  name = "demo",
  version = "0.1.0",
  root = ".",
  build_dir = "build/qstar",
  generated_dir = "build/qstar/generated",
}
```

```lua
qstar.import_file("qstar/policies/common.qst")
local paths = qstar.import_module("qstar/modules/paths")
qstar.subdir("src")
```

## Toolsets

```lua
qstar.toolset "host" {
  tools = {
    c = { compiler = qstar.cli {"cc"} },
    cxx = { compiler = qstar.cli {"c++"} },
    asm = { compiler = qstar.cli {"cc"} },
    archive = qstar.cli {"ar"},
    link = qstar.cli {"cc"},
  },
  response_files = "auto",
  response_style = "posix",
  path_tools = {"python3", "sh"},
}
```

Toolsets only select tool roles and command materialization policy. Compile and
link options belong in `qstar.config` or target-local fields.

## Configs

```lua
qstar.config "common_c" {
  toolset = "//:host",
  lang = {
    c = {
      public_include_dirs = {"include"},
      compile_options = {"-std=c23", "-Wall", "-Wextra"},
    },
  },
}
```

Config list fields append in `configs` order. Target-local list fields append
last. Target-local scalar fields override config scalars.

## Targets

```lua
qstar.staticlib "core" {
  configs = {"//:common_c"},
  sources = {"src/core.c"},
}

qstar.executable "app" {
  configs = {"//:common_c"},
  sources = {"src/main.c"},
  deps = {"//:core"},
}

qstar.group "all" {
  deps = {"//:app"},
}
```

`qstar.group` has no artifact and cannot be used with `qstar.target_file`.

## Generated Artifacts

```lua
qstar.custom_target "package_blob" {
  inputs = {qstar.target_file("//:app")},
  outputs = {qstar.output("build/qstar/generated/app.bin", {group = "packages"})},
  command = qstar.cli {"tools/package-object", qstar.input(0), qstar.output(0)},
  description = qstar.status("Packaging app.bin"),
}
```

Generated object outputs use the same surface:

```lua
qstar.custom_target "foreign_object" {
  inputs = {"src/foreign_source.ext"},
  outputs = {qstar.output("build/qstar/generated/foreign.o", {format = "object"})},
  command = qstar.cli {"tools/compile-foreign.sh", qstar.input(0), qstar.output(0)},
}
```

## Run And Stage

```lua
qstar.run_target "smoke" {
  deps = {"//:app"},
  command = qstar.cli {qstar.target_file("//:app")},
  expect = {contains = "OK"},
  description = qstar.status("Running smoke test"),
}

qstar.stage "bundle" {
  root = "stage/bundle",
  files = {
    qstar.stage_file(qstar.target_file("//:app"), "bin/app"),
  },
}
```

## Host Constants

Allowed constants:

- `QSTAR_VERSION`
- `QSTAR_VERSION_MAJOR`
- `QSTAR_VERSION_MINOR`
- `QSTAR_VERSION_PATCH`
- `QSTAR_HOST_OS`
- `QSTAR_HOST_ARCH`
- `QSTAR_PACKAGE_ROOT`
- `QSTAR_PROJECT_ROOT`
- `qstar.version`
- `qstar.host.os`
- `qstar.host.arch`
- `qstar.project.root`

Host-specific authoring uses ordinary Lua `if` statements over `qstar.host`.

## CLI

```sh
qstar --file qstar.lua check //...
qstar --file qstar.lua explain //:app
qstar --file qstar.lua -B build/stella -G stella build //:app
qstar --file qstar.lua -B build/ninja -G ninja build //:app
```
