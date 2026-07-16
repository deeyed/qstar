# QStar Syntax Cheat Sheet

`wiki/reference/qstar-lua.md` is the canonical user-facing syntax reference.
This file is a short developer-side drift guard for the current generic DSL.
The current configurable build surface reference lives in
`docs/configurable-build-surface.md` and its wiki mirror at
`wiki/reference/configurable-build-surface.md`; it defines `qstar.option`,
`qstar.variant`, `qstar.objectlib`, CLI `-D` option overrides, nested
`qstar.subdir`, and explicit fragment-relative `./` path resolution.

## Strict Declaration Tables

Every public declaration table has an exact field and Lua type schema. Unknown
fields, wrong types, and sparse or named-key lists fail graph evaluation; the
diagnostic includes the Lua source location, API name, and declaration label.
QStar does not silently ignore misspelled fields.

```text
qstar: src/core/core.qst:12: qstar.objectlib declaration '//src/core:objects': unknown field 'soruces'
```

`qstar.variant.values` remains user-defined metadata, ordinary tables returned
by `qstar.import_module()` are not declarations, and `lang.<provider>` options
use the activated provider's dynamic option schema. The complete field/type
contract is in `docs/public-declaration-schemas.md` and its wiki mirror at
`wiki/reference/declaration-schemas.md`.

## Entrypoints

```lua
qstar.project {
  name = "demo",
  version = "0.1.0",
  root = ".",
  build_dir = "build/qstar",
  generated_dir = "build/qstar/generated",
  action_cache = "local", -- optional; default "off"
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

Provider `finals` schemas declare provider-owned final artifact hooks. The example
above is `qstar.lang/1`: when all compile sources come from one v1 provider and
there are no native link deps/inputs, QStar calls the matching final with
`ctx.input("sources")`, `ctx.output("artifact")`, and `ctx.kind()`.

`qstar.lang/2` adds explicit final input ownership and artifact descriptors:

```lua
finals = {
  executable = {
    lower = "link_executable",
    inputs = {"sources", "objects", "link_interfaces", "link_inputs", "link_options"},
    artifacts = {
      runtime = {type = "file", roles = {"primary", "runtime"}},
      metadata = {type = "file", roles = {"secondary"}, suffix = ".metadata"},
      resources = {type = "tree", roles = {"secondary", "runtime"}, suffix = ".resources"},
      link = {type = "file", roles = {"secondary", "link-interface"}, suffix = ".link"},
    },
  },
}
```

V2 final lowering reads only manifest-declared `ctx.input(...)` classes and uses
descriptor ids with `ctx.output("runtime")`, `ctx.output("metadata")`, and so on.
`sources` are final-owner source paths; `objects` can combine built-in and foreign
provider objects; `link_interfaces` are dependency artifacts; `link_inputs` and
`link_options` preserve explicit target declarations. Every declared output must
appear in the lowering result and undeclared outputs are rejected. Each final has
exactly one primary artifact, may have file/tree secondary and runtime artifacts,
and at most one file `link-interface`. Descriptor ids are named
`qstar.target_file(..., {artifact = "id"})` selectors.

QStar accepts `qstar.lang/1` and `qstar.lang/2` simultaneously. Other versions
fail with a supported-version diagnostic. V2 does not infer platform, target
triple, runtime, linker, or library flags. The full Korean contract is in
`docs/language-provider-backend-contract.md` and
`wiki/reference/language-providers.md`.

The GLP backend calls the unit `lower` function from `provider.lua` during graph
evaluation, and calls final lowering after the complete target graph is available.
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
link options belong in `qstar.config` or target-local fields. Generated actions
can also select a toolset for command tool policy. If a generated action uses a
bare PATH command tool and sets `toolset`, that tool must be listed in the
selected toolset's `path_tools`.

## Opt-In C++ Build Strategies

```lua
qstar.executable "app" {
  sources = {
    "src/math.cppm",
    "src/math_use.cpp",
    "src/main.cpp",
  },
  lang = {
    cxx = {
      standard = "c++20",
      precompiled_header = "include/pch.hpp",
      unity = { enabled = true, batch_size = 8 },
      modules = { enabled = true },
    },
  },
}
```

All three strategies default to off. PCH and unity are available for recognized
Clang/GCC-family C++ compiler roles. Module lowering requires upstream Clang and
C++20 or newer. `.cppm`/`.ixx` sources are module interfaces; ordinary built-in
C++ sources are implementation units. Interface actions emit object plus `.pcm`
BMI outputs, later interfaces depend on earlier interfaces, and implementations
depend on every interface BMI. The interface basename is the BMI lookup name, so
`src/math.cppm` must declare `export module math;`; basenames must be unique and
interfaces that import another interface must follow it in the `sources` list.
This release deliberately uses declaration order as a safe dependency superset
instead of embedding a compiler-specific import scanner. Each dependency is
passed as an explicit `-fmodule-file=<name>=<BMI>` mapping, so lowering does not
depend on compiler directory discovery. A configured PCH is deliberately omitted
from module-interface actions, while ordinary implementation and unity actions
continue to consume it; this keeps compiler-version-sensitive PCH state out of the
BMI. Unity batches only target-local ordinary C++ implementation sources and never
merge C, ASM, module interfaces, GLP sources,
or source ownership across targets. Strategy action ids and the actual PCH,
module, and unity commands are recorded in `compile_commands.json` for both
Stella and Ninja.

## Generic Artifact Workflow

```lua
qstar.toolset "artifact_tools" {
  tools = {
    archive = qstar.cli {"ar"},
    link = qstar.cli {"cc"},
  },
  path_tools = {"transform-artifact"},
}

qstar.transform "payload_artifact" {
  toolset = "//:artifact_tools",
  input = "fixtures/payload.artifact",
  output = qstar.output("generated/artifacts/payload.artifact", {
    group = "artifacts",
  }),
  command = qstar.cli {
    "transform-artifact",
    qstar.input(0),
    qstar.output(0),
  },
}

qstar.stage "workflow_layout" {
  root = "stage/workflow",
  files = {
    qstar.stage_file(qstar.target_file("//:payload_artifact"),
      "artifacts/payload.artifact"),
  },
}

qstar.command "workflow" {
  options = {
    out = qstar.param.path { default = "exports/workflow" },
    verify = qstar.param.bool { default = true },
  },
  steps = {
    qstar.step.build("//:payload_artifact"),
    qstar.step.stage("//:workflow_layout"),
    qstar.step.run {
      when = qstar.param("verify"),
      inputs = {qstar.stage_dir("//:workflow_layout")},
      command = qstar.cli {
        "tools/check-workflow.sh",
        "--layout",
        qstar.input(0),
        qstar.arg_if(qstar.param("verify"), "--verify"),
      },
    },
    qstar.step.export_stage("//:workflow_layout", {
      to = qstar.param("out"),
    }),
  },
}
```

`qstar.transform` is single-input/single-output sugar over `qstar.custom_target`.
`qstar.command` is root-only and exposes Makefile-like project commands without
shell strings or domain-specific target kinds.

## Configs

```lua
qstar.config "common_c" {
  toolset = "//:host",
  cacheable = true,
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

Typed dependency target은 artifact graph와 usage requirement를 명시적으로 표현한다.

```lua
qstar.interface "api" {
  compile_usage = {
    options = {"-DAPI_LEVEL=3"},
    inputs = {"contracts/api.txt"},
  },
}

qstar.imported "codec" {
  artifact_kind = "prebuilt_codec",
  artifacts = {
    default = {
      {id = "archive", role = "link", path = "vendor/libcodec.a", primary = true},
    },
    windows = {
      {id = "runtime", role = "runtime", path = "vendor/codec.dll", primary = true},
      {id = "import_lib", role = "link", path = "vendor/codec.lib", primary = false},
    },
  },
  deps = {"//:api"},
}

qstar.tool "generator" { path = "tools/generator" }
```

`compile_usage`/`link_usage`는 `options`와 `inputs`만 받는다. Options는 consumer
argv에 그대로 추가되고 inputs는 argv에 들어가지 않는 rebuild/producer input이다.
`qstar.tool_file(label)`은 package-local tool, imported `role = "tool"`, QStar-built
executable/test를 generated command argv[0]으로 사용하며 producer edge를 만든다.
Imported suffix, `artifact_kind`, id에서 flag를 추론하지 않는다. 상세 계약은
`docs/typed-dependency-targets.md`와 `wiki/reference/typed-dependencies.md`에 있다.

```lua
qstar.staticlib "core" {
  configs = {"//:common_c"},
  sources = {"src/core.c"},
}

qstar.objectlib "core_objects" {
  configs = {"//:common_c"},
  sources = {"src/core_extra.c"},
  compile_context = "own",
  cacheable = true,
}

qstar.executable "app" {
  configs = {"//:common_c"},
  sources = {"src/main.c"},
  deps = {"//:core"},
  objects = {"//:core_objects"},
}

qstar.group "all" {
  deps = {"//:app"},
}
```

`qstar.objectlib` compiles or collects object files but has no final
archive/link artifact. `compile_context = "own"` compiles once under the
objectlib context. `compile_context = "consumer"` keeps source ownership in the
objectlib but compiles each source under every consuming artifact target's
effective configs/lang/toolset, with per-consumer object identities. Artifact
targets consume object libraries through `objects = {...}`.
`qstar.target_file("//:objects")` is an error for objectlib targets.
Objectlib sources can be built-in C/C++/ASM sources, activated GLP raw source
strings, explicit provider source tokens in `"own"` context, or generated
object outputs. Explicit provider source tokens are rejected in
`compile_context = "consumer"` because they already carry a concrete
action/output.

`qstar.group` has no artifact and cannot be used with `qstar.target_file`.
`qstar.target_file("//:label")` resolves to the target's primary artifact. A
secondary artifact can be selected when the target exposes one:

```lua
qstar.target_file("//:plugin", { artifact = "import_lib" })
```

This is used by the Windows sharedlib artifact contract and by `qstar.lang/2`
provider artifacts. On Windows the primary artifact is the runtime `.dll`, while
`artifact = "import_lib"` selects the import `.lib`. A v2 provider uses its
manifest descriptor ids for the same selector surface, including file or tree
secondary/runtime artifacts.

## Generated Artifacts

```lua
qstar.toolset "artifact_tools" {
  tools = {
    archive = qstar.cli {"ar"},
    link = qstar.cli {"cc"},
  },
  path_tools = {"package-object", "compile-foreign"},
}

qstar.custom_target "package_blob" {
  toolset = "//:artifact_tools",
  inputs = {qstar.target_file("//:app")},
  outputs = {qstar.output("build/qstar/generated/app.bin", {group = "packages"})},
  command = qstar.cli {"package-object", qstar.input(0), qstar.output(0)},
  description = qstar.status("Packaging app.bin"),
  cacheable = true,
}
```

Single-input/single-output transforms can use the readable sugar form. It lowers
to the same generated action contract as `qstar.custom_target`.

```lua
qstar.transform "package_blob" {
  toolset = "//:artifact_tools",
  input = qstar.target_file("//:app"),
  output = qstar.output("build/qstar/generated/app.bin", {group = "packages"}),
  command = qstar.cli {"package-object", qstar.input(0), qstar.output(0)},
  description = qstar.status("Packaging app.bin"),
  cacheable = true,
}
```

`cacheable` is a strict boolean candidate policy for artifact/test/objectlib,
config, and generated declarations. It defaults to `true`, but the local CAS is
off unless `qstar.project.action_cache = "local"` or build option
`--action-cache local` is selected. The current CAS stores only regular-file
compile/generated outputs. External runtime/device actions and unsupported
action kinds remain non-cacheable. The audit is report-only; see
`docs/local-action-cache.md`.

Generated object outputs use the same surface:

```lua
qstar.custom_target "foreign_object" {
  toolset = "//:artifact_tools",
  inputs = {"src/foreign_source.ext"},
  outputs = {qstar.output("build/qstar/generated/foreign.o", {format = "object"})},
  command = qstar.cli {"compile-foreign", qstar.input(0), qstar.output(0)},
}
```

Generated action fields:

| Field | Meaning |
| --- | --- |
| `inputs` | `custom_target` input list. Items can be package files or artifact tokens such as `qstar.target_file(...)`. |
| `input` | `transform` single input. This is sugar for one `custom_target.inputs` item. |
| `outputs` | `custom_target` output list. Each item is `qstar.output(...)`. |
| `output` | `transform` single output. This is sugar for one `custom_target.outputs` item. |
| `command` | Required `qstar.cli { ... }` argv vector. |
| `toolset` | Optional `qstar.toolset` label used to resolve and authorize the first argv item. |
| `description` | Optional `qstar.status("...")` progress/action-log description. |
| `cacheable` | Optional boolean local-CAS candidate policy; default `true`. |

If `toolset` is omitted, generated actions use the path-tool policy declared by
the graph's toolsets. Package-relative command paths such as
`tools/generate.sh` remain package-local files and do not need `path_tools`.

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
qstar.transform "image" {
  input = qstar.target_file("//:app"),
  output = qstar.output("build/qstar/generated/app.img"),
  command = qstar.cli {"tools/transform", qstar.input(0), qstar.output(0)},
}

qstar.run_target "check_image" {
  inputs = {qstar.target_file("//:image"), "fixtures/expected.txt"},
  command = qstar.cli {"tools/check-image", qstar.input(0), qstar.input(1)},
}
```

## Project Commands

Root `qstar.lua` may expose user-facing project commands. A project command is a
Makefile-phony-style CLI surface made of generic QStar steps, not a shell
script. It is root-only: `.qst` fragments, `.qsm` modules, and provider files
cannot declare project commands.

```lua
qstar.command "make-bundle" {
  description = qstar.status("Build the local bundle"),
  aliases = {"bundle"},
  is_default = true,
  options = {
    mode = qstar.param.enum {
      choices = {"debug", "release"},
      default = "debug",
    },
    verbose = qstar.param.bool {
      default = false,
    },
  },
  steps = {
    qstar.step.build("//:image"),
    qstar.step.stage("//:bundle", {root = "stage/bundle"}),
    qstar.step.run {
      when = qstar.param("verbose"),
      inputs = {qstar.stage_dir("//:bundle")},
      timeout = 30,
      expect = {
        contains = "bundle ok",
      },
      command = qstar.cli {
        "tools/inspect-bundle",
        qstar.input(0),
        qstar.param("mode"),
        qstar.arg_if(qstar.param("verbose"), "--verbose"),
      },
    },
    qstar.step.export_stage("//:bundle", {
      to = "exports/bundle",
    }),
  },
}
```

Invocation:

```sh
qstar commands
qstar commands --format json
qstar bundle --mode release
qstar        # runs the single is_default command, when declared
```

Allowed `qstar.command` fields:

| Field | Type | Meaning |
| --- | --- | --- |
| `description` | `qstar.status("...")` | Command list/help text. |
| `options` | table | Typed CLI option schema. |
| `env` | list string | Default `NAME=value` environment overlay for `qstar.step.run`; action-log/replay redact values. |
| `working_dir` | string | Package-relative working directory used by `qstar.step.run` unless the step overrides it. |
| `steps` | list | Ordered `qstar.step.*` values. |
| `is_default` | bool | Marks the default project command; only one may be default. |
| `hidden` | bool | Hides from `qstar commands` while staying callable. |
| `aliases` | list string | Alternative CLI names. |

Supported option schemas are `qstar.param.string`, `qstar.param.path`,
`qstar.param.bool`, `qstar.param.int`, `qstar.param.enum`, and
`qstar.param.list`. Common option fields are `description`, `required`,
`default`, and `choices` for enum. Command options are validated at CLI
dispatch. `required = true` cannot be combined with `default`, bool options
cannot be required, and list defaults are not supported. Runtime values are
referenced with `qstar.param("name")`.

Bool options support these CLI forms:

| Declared option | Accepted CLI form | Runtime value |
| --- | --- | --- |
| `qstar.param.bool { default = false }` | omitted | `false` |
| `qstar.param.bool { default = false }` | `--flag`, `--flag=true` | `true` |
| `qstar.param.bool { default = true }` | omitted | `true` |
| `qstar.param.bool { default = true }` | `--no-flag`, `--flag=false` | `false` |

Runtime option helpers:

| Helper | Where | Meaning |
| --- | --- | --- |
| `qstar.param("name")` | inside `qstar.cli { ... }` | Expands the option value into argv. A list option expands to all provided values. |
| `when = qstar.param("flag")` | step option/field | Skips the step unless the referenced bool option is true. |
| `qstar.arg_if(qstar.param("flag"), "--arg")` | inside `qstar.cli { ... }` | Appends one argv atom only when the referenced bool option is true. |
| `qstar.args_if(qstar.param("flag"), {"--a", "b"})` | inside `qstar.cli { ... }` | Appends multiple argv atoms only when the referenced bool option is true. |

Supported steps in the core surface are:

| Step | Meaning |
| --- | --- |
| `qstar.step.build(label)` | Build a target, generated action, stage, run target, or group label. |
| `qstar.step.test(label)` | Run a `qstar.test` target selection. |
| `qstar.step.stage(label, opts)` | Materialize a `qstar.stage`; `opts.root` and `opts.dry_run` mirror the stage CLI. |
| `qstar.step.check(label)` | Run graph/input validation. Use `"//..."` for the whole graph. |
| `qstar.step.lint(label)` | Run authoring lint. Use `"//..."` for the whole graph. |
| `qstar.step.run { command = qstar.cli { ... } }` | Execute a command-local argv-vector action. Supports `inputs`, `env`, `working_dir`, `description`, `timeout`, `expect`, and `when`. |
| `qstar.step.call(name)` | Call another project command or alias; cycles are rejected. |
| `qstar.step.export_stage(label, opts)` | Materialize a `qstar.stage` and copy its layout to `opts.to`; `opts.to` may be a literal package path or `qstar.param("path_option")`. |

`qstar.step.run` is phony in the Makefile sense: it runs every command invocation.
It still uses the backend execution contract, so Stella and Ninja producer edges
are built first, stdout/stderr are captured under `build/qstar/logs`, timeouts and
expect checks produce failure replay files, and `qstar action-log
qstar-command:<name>:run:<index>` can inspect the resolved argv/env metadata.

`qstar.step.run.inputs` accepts the same item kinds as `run_target.inputs`:
package-relative files, `qstar.target_file(...)`, and `qstar.stage_dir(...)`.
`qstar.input(N)` in the step command resolves to the Nth declared input. Step
`env` entries override command-level env entries with the same variable name.
`expect = { contains = "...", file = "..." }` matches captured stdout/stderr and
the optional package-relative file.

The command name and aliases cannot collide with built-in QStar CLI commands
such as `build`, `test`, `stage`, `commands`, `init`, `docs`, `daemon`, or
diagnostic commands.

### Reusable command specifications

Large roots may move pure command data into a cached QSM without giving that
module graph-declaration authority:

```lua
-- qstar/modules/commands/commands.qsm
return {
  qstar.command_spec "verify" {
    aliases = {"v"},
    steps = {qstar.step.check("//...")},
  },
}
```

```lua
-- root qstar.lua
local commands = qstar.import_module("qstar/modules/commands")
qstar.command_set(commands)
```

`qstar.command_spec` accepts exactly the same fields, option schemas, and steps
as `qstar.command`, but only returns a deeply read-only deterministic value. It
does not mutate the graph and is therefore allowed in an ordinary `.qsm`.
`qstar.command_set` accepts one non-empty contiguous list of immutable specs and
is root-only. Plain tables are rejected. Materialized commands use the existing
name/alias/default/call-cycle validation and appear identically in
`qstar commands --format json` under `qstar-commands-v1`.

See [Reusable Project Command Sets](reusable-project-command-sets.md) for the
complete module layout, immutability, collision, and authority contract.

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

## Composable Test Suites

```lua
qstar.test_suite "verification" {
  tests = {"//:unit", "//:runtime_smoke"},
  tags = {"fast", "simulator"},
  description = qstar.status("Project verification suite"),
  manual = false,
}
```

`tests` accepts existing `qstar.test`, `qstar.run_target`, or nested suite
labels. `tags` are exact free-form user metadata with no builtin platform,
runner, or evidence meaning. `manual` only excludes a suite from implicit tag
root discovery; explicit or nested selection still includes it.

```sh
qstar test --suite //:verification
qstar test --tag fast --exclude-tag slow
qstar query //:verification --format json
```

The canonical contract is `docs/composable-test-suites.md`.

## Test Resources And Results

```lua
qstar.test_resource "shared.slot" {
  capacity = 1,
  description = qstar.status("Exclusive test slot"),
}

qstar.test "integration" {
  sources = {"tests/integration.c"},
  resources = { ["shared.slot"] = 1 },
  retry = { count = 2, on = {"fail", "error", "timeout"} },
  setup = qstar.cli {"tools/setup-fixture"},
  cleanup = qstar.cli {"tools/cleanup-fixture"},
  timeout = 30,
  manual = false,
}
```

Resource ids are exact user-defined identifiers. QStar assigns no device,
platform, runner, or evidence meaning to names such as `board`, `gpu`, or
`serial-port`. Builtin result states are `pass`, `fail`, `skip`, `error`, and
`timeout`. Resources remain held across setup, test body, and cleanup, and are
released before a retry.

```sh
qstar test --jobs 8 --report-json build/results/tests.json
qstar test --include-manual --output-junit build/results/junit.xml
qstar action-log '//:integration:test:test:2'
```

JSON defaults to `<build_dir>/test-results.json` and uses
`qstar-test-results-v1`. Stella and Ninja share the scheduler and result
protocol. The canonical Korean contract is
`docs/test-resources-and-results.md`.

## CLI

```sh
qstar --file qstar.lua check //...
qstar --file qstar.lua explain //:app
qstar --file qstar.lua -B build/stella -G stella build //:app
qstar --file qstar.lua -B build/ninja -G ninja build //:app
```
