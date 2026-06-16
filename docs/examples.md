# QStar Examples

This page keeps compact examples for the current generic QStar DSL. For full
explanations, read `../wiki/AI_INDEX.md` and `../wiki/reference/qstar-lua.md`.

## Toolset And Config

```lua
qstar.project {
  name = "demo",
  version = "0.1.0",
  root = ".",
  generated_dir = "build/qstar/generated",
}

qstar.toolset "host" {
  tools = {
    c = { compiler = qstar.cli {"cc"} },
    cxx = { compiler = qstar.cli {"c++"} },
    asm = { compiler = qstar.cli {"cc"} },
    archive = qstar.cli {"ar"},
    link = qstar.cli {"cc"},
  },
  response_files = "auto",
}

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

## Library And App

```lua
qstar.staticlib "core" {
  configs = {"//:common_c"},
  sources = {"src/core.c"},
  lang = {
    c = {
      public_headers = {"include/core.h"},
      public_include_dirs = {"include"},
    },
  },
}

qstar.executable "app" {
  configs = {"//:common_c"},
  sources = {"src/main.c"},
  deps = {"//:core"},
}
```

## Package Artifact

```lua
qstar.custom_target "package_blob" {
  inputs = {qstar.target_file("//:app")},
  outputs = {qstar.output("build/qstar/generated/app.bin", {group = "packages"})},
  command = qstar.cli {"tools/package-object", qstar.input(0), qstar.output(0)},
  description = qstar.status("Packaging app.bin"),
}

qstar.stage "bundle" {
  root = "stage/bundle",
  description = qstar.status("Staging release bundle"),
  files = {
    qstar.stage_file(qstar.target_file("//:app"), "bin/app"),
    qstar.stage_file(qstar.target_file("//:package_blob"), "share/app.bin"),
  },
}
```

## External Object Bridge

```lua
qstar.custom_target "foreign_object" {
  inputs = {"src/foreign_source.ext"},
  outputs = {qstar.output("build/qstar/generated/foreign.o", {format = "object"})},
  command = qstar.cli {"tools/compile-foreign.sh", qstar.input(0), qstar.output(0)},
  description = qstar.status("Building foreign object"),
}

qstar.executable "mixed_app" {
  configs = {"//:common_c"},
  sources = {
    "src/main.c",
    qstar.output("build/qstar/generated/foreign.o"),
  },
}
```
