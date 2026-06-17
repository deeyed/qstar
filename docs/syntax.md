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

```lua
local zig = qstar.use_language("zig")
-- Resolves project-local qstar/languages/zig/zig.qsm first.
-- If absent, falls back to the bundled standard provider.
-- qstar.use_language("qstar/languages/zig") is the explicit project folder form.
```

`qstar.use_language` activates a provider namespace before `lang.<namespace>`
is accepted. Built-in `lang.c`, `lang.cxx`, and `lang.asm` are preloaded.
QStar ships standard `zig`, `rust`, and `cuda` providers.
The Zig provider's real `zig build-obj` path is documented in `docs/zig-provider.md`;
it also supports `zig build-exe`/`zig build-lib` provider final actions, exposes
`target`, `optimize`, `macos_min_version`, and `compile_options`, and uses provider
action env for project-local Zig caches.
The Rust provider's real `rustc` path is documented in `docs/rust-provider.md`;
it exposes `crate_type` for object units and supports provider final actions for
`bin`, `staticlib`, and `cdylib` artifacts.

Provider packages use a manifest plus implementation split:

```lua
-- qstar/languages/zig/zig.qsm
return qstar.language_provider {
  api = "qstar.lang/1",
  id = "zig",
  version = "0.1",
  namespace = "zig",
  implementation = "provider.lua",
  tools = {
    compiler = {role = "zig.compiler", required = true},
  },
  units = {
    object = {
      suffixes = {".zig"},
      emits = "object",
      lower = "compile_object",
      deps = "none",
    },
  },
  finals = {
    executable = {lower = "link_executable"},
    staticlib = {lower = "archive_staticlib"},
    sharedlib = {lower = "link_sharedlib"},
  },
  options = {
    optimize = {
      type = "enum",
      values = {"Debug", "ReleaseSafe", "ReleaseFast", "ReleaseSmall"},
      default = "Debug",
    },
    target = {
      type = "string",
      default = "native",
    },
    macos_min_version = {
      type = "string",
      default = "",
    },
    compile_options = {
      type = "list",
      default = {},
    },
  },
  exports = {
    tools = "tools",
    options = "options",
    object = "object",
  },
  scaffold = {
    api = "qstar.scaffold/1",
    tools = {
      compiler = {"zig"},
    },
    options = {
      optimize = "Debug",
      target = "native",
      macos_min_version = "11.0",
    },
    shapes = {
      app = {
        files = {
          {
            path = "src/main.zig",
            body = "export fn main() c_int { return 0; }\n",
          },
        },
        targets = {
          {
            kind = "executable",
            name = "app",
            sources = {
              "src/main.zig",
            },
          },
        },
      },
    },
  },
}
```

```lua
-- qstar/languages/zig/provider.lua
local P = {}

function P.tools(t)
  return qstar.provider_tools("zig", {
    compiler = t.compiler,
  })
end

function P.options(t)
  return qstar.language_options("zig", t or {})
end

function P.object(path, opts)
  return qstar.source(path, qstar.merge({
    language = "zig",
    unit = "object",
  }, opts or {}))
end

function P.link_executable(ctx)
  local argv = qstar.argv()
  argv:add(ctx.tool("compiler"))
  argv:add("build-exe")
  argv:add_all(ctx.input("sources"))
  argv:add("-femit-bin=" .. ctx.output("artifact"))
  return {command = argv, inputs = ctx.input("sources"), outputs = {ctx.output("artifact")}}
end

return P
```

`provider.lua` runs in a restricted provider sandbox. It cannot declare targets
or call graph entrypoints; it can read read-only `qstar.host.os` and
`qstar.host.arch` for host-specific argv choices. Only the manifest `exports`
are returned to user code.
Provider `scaffold` metadata is optional validated init data. `qstar init`
materializes the primary provider shape into `qstar.lua`, source files, and
workspace fragments when that shape is available. It can name default
tools/options and shape-local files/targets/fragments, but all path fields must
be package-relative and the schema has no shell command, script, fetch, or
network fields.
Provider `options` schemas validate `lang.<namespace>` keys and values when
user code writes tables such as:

```lua
local zig = qstar.use_language("zig")

qstar.config "debug_zig" {
  lang = {
    zig = zig.options {
      optimize = "Debug",
      target = "native",
      macos_min_version = "",
      compile_options = {"-Ddemo"},
    },
  },
}

qstar.staticlib "core" {
  configs = {"//:debug_zig"},
  sources = {
    "src/main.zig",
  },
}
```

Provider `units` schemas register source suffixes with the graph-level source
registry. Once `qstar.use_language("zig")` activates the provider, a raw source
string such as `"src/main.zig"` lowers to the provider's object unit just like
built-in C/C++/ASM sources do. Exported helpers such as
`zig.object("src/main.zig", {optimize = "ReleaseFast"})` remain available for
source-local options or for disambiguating suffix collisions. If a raw string
matches more than one provider unit, or both a built-in suffix and a provider
unit, QStar asks the user to use an explicit provider helper.

Provider `finals` schemas declare provider-owned final artifact hooks. If a target's
compile sources all come from the same external provider and the target has no
native link deps/inputs, QStar calls the matching final lowering function and uses
`ctx.input("sources")`, `ctx.output("artifact")`, and `ctx.kind()` instead of
creating object compile edges plus a native C-style final linker/archive edge.

The GLP backend calls the unit `lower` function from `provider.lua` during graph
evaluation, and calls final lowering functions for pure provider final artifacts.
It stores the resulting action template in Graph IR. Stella and Ninja then
consume the same `command`, `env`, `inputs`, `outputs`, and `depfile` contract.
Env entries use `NAME=value`; process runners receive the values, while
action-log/replay show only `NAME=<redacted>`.

Standard external providers follow the same shape:

```lua
local cuda = qstar.use_language("cuda")

qstar.config "debug_cuda" {
  lang = {
    cuda = cuda.options {
      arch = "native",
      compile_options = {},
    },
  },
}
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
`qstar.target_file("//:label")` resolves to the target's primary artifact. A
secondary artifact can be selected when the target exposes one:

```lua
qstar.target_file("//:plugin", { artifact = "import_lib" })
```

Today this is used by the Windows sharedlib artifact contract: the primary
artifact is the runtime `.dll`, while `artifact = "import_lib"` selects the
import `.lib` that dependent targets link against on Windows.

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
  inputs = {qstar.target_file("//:app")},
  command = qstar.cli {"tools/check-artifact.sh", qstar.input(0)},
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

`run_target.inputs` declares first-class inputs independently from the command
argv. Items may be package-relative files, `qstar.target_file(...)` artifacts, or
`qstar.stage_dir(...)` layout roots. `qstar.input(N)` inside the command resolves
to the Nth run input, so the producer edge, input tracking, and argv path stay in
sync. A stage input is a consumable layout artifact: QStar builds the files that
feed the stage, materializes the layout, writes its manifest, and only then runs
the consuming action on both Stella and Ninja backends:

```lua
qstar.custom_target "image" {
  inputs = {qstar.target_file("//:app")},
  outputs = {qstar.output("build/qstar/generated/app.img")},
  command = qstar.cli {"tools/transform", qstar.input(0), qstar.output(0)},
}

qstar.run_target "check_image" {
  inputs = {qstar.target_file("//:image"), "fixtures/expected.txt"},
  command = qstar.cli {"tools/check-image", qstar.input(0), qstar.input(1)},
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
