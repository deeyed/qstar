# Windows Artifact Graph IR Plan

Round Q173 fixes the Graph IR direction for Windows artifact implementation
before runtime `.dll` and import `.lib` support is added. This is a design note,
not a claim of official Windows host support.

## Goal

Windows support needs more than a suffix table. A single `qstar.sharedlib`
target can produce multiple artifacts:

- runtime `.dll`
- import `.lib`
- optional debug `.pdb`

QStar must model these without changing the meaning of existing targets,
`qstar.target_file("//:label")`, install, stage, Stella action logs, or Ninja
lowering.

## Artifact Roles

Each target should lower to an ordered artifact map. The first entry is the
primary artifact. Secondary artifacts are addressable by role.

```txt
artifact role=runtime primary=1 group=bin path=build/qstar/out/__plugin/plugin.dll
artifact role=import_lib primary=0 group=lib path=build/qstar/out/__plugin/plugin.lib
artifact role=debug_symbols primary=0 group=symbols path=build/qstar/out/__plugin/plugin.pdb
```

Role meanings:

| Role | Producer | Primary | Install default | Notes |
| --- | --- | --- | --- | --- |
| `executable` | `qstar.executable` | yes | `bin/` | `.exe` when explicitly named or future Windows default suffix |
| `static_lib` | `qstar.staticlib` | yes | `lib/` | Unix `.a` and Windows static `.lib` are the same static role with different names |
| `runtime` | `qstar.sharedlib` | yes | `bin/` on Windows | Runtime `.dll`; `qstar.target_file(label)` resolves here |
| `import_lib` | Windows `qstar.sharedlib` | no | `lib/` | Link interface consumed by dependents |
| `debug_symbols` | opt-in linker/debug policy | no | none | PDB/debug artifacts are deferred and never implicit |

The existing `artifact_name` field remains the primary artifact basename. It
must not be overloaded to name both `.dll` and import `.lib`.

## Public Resolution Rules

Existing projects keep the current rule:

```lua
qstar.target_file("//:plugin")
```

This resolves to the primary artifact. For Windows shared libraries, the primary
artifact is the runtime `.dll`.

Q173 reserves an optional selector form for secondary artifacts:

```lua
qstar.target_file("//:plugin", { artifact = "import_lib" })
qstar.target_file("//:plugin", { artifact = "debug_symbols" })
```

Selector names are artifact roles, not filenames. Unknown selectors should fail
with a diagnostic that lists known roles for the target. `debug_symbols` should
be unavailable unless the target has explicitly opted into QStar-owned debug
artifacts.

## Link Semantics

Link consumers should not choose the import library manually. When a Windows
`sharedlib` target appears in `deps`, `private_deps`, or `public_deps`, QStar
should use the producer's `import_lib` artifact as the link input and keep the
runtime `.dll` as an order/runtime artifact.

```txt
//:app link inputs:
  objects...
  artifact //:plugin role=import_lib path=.../plugin.lib

//:app runtime order inputs:
  artifact //:plugin role=runtime path=.../plugin.dll
```

On Darwin/Linux, shared library dependencies continue to link against the
primary shared object artifact. Windows is special because the runtime artifact
and link interface artifact are different files.

## Build Action Outputs

The Windows `link-shared` action should have a multi-output record:

```txt
action id=//:plugin:link-shared:0
output role=runtime path=build/qstar/out/__plugin/plugin.dll primary=1
output role=import_lib path=build/qstar/out/__plugin/plugin.lib primary=0
```

If PDB support is later owned by QStar:

```txt
output role=debug_symbols path=build/qstar/out/__plugin/plugin.pdb primary=0 optional=1
```

Stella dirty-check state and Ninja lowering must track all outputs of the same
final action. A missing import `.lib` should dirty the sharedlib action even if
the runtime `.dll` exists.

## JSON And Explain Compatibility

Existing JSON fields should remain compatible:

- `artifact_name`: primary artifact basename
- `installable`: whether the target has installable artifacts

New JSON can add an `artifacts` array:

```json
{
  "label": "//:plugin",
  "kind": "sharedlib",
  "artifact_name": "plugin.dll",
  "artifacts": [
    {"role": "runtime", "primary": true, "path": "build/qstar/out/__plugin/plugin.dll"},
    {"role": "import_lib", "primary": false, "path": "build/qstar/out/__plugin/plugin.lib"}
  ]
}
```

`explain` and `dry-run` should display the artifact roles before command argv so
diagnostics and AI tools can tell whether a `.lib` is static or import.

## Install And Stage

Install policy:

- executable `.exe`: `bin/`
- runtime `.dll`: `bin/`
- static `.lib`: `lib/`
- import `.lib`: `lib/`
- PDB/debug: no implicit install

Stage policy remains explicit. Users choose destination paths with
`qstar.stage_file(...)`. The selector form is needed for import libraries:

```lua
qstar.stage_file(qstar.target_file("//:plugin"), "bin/plugin.dll")
qstar.stage_file(qstar.target_file("//:plugin", { artifact = "import_lib" }),
  "lib/plugin.lib")
```

## Stella And Ninja Parity

Stella and Ninja must agree on:

- artifact role names
- primary artifact path
- secondary artifact paths
- consumer link inputs
- install/stage producer resolution
- unsupported selector diagnostics
- Windows sharedlib unsupported diagnostics until implementation lands

Ninja lowering should emit the runtime `.dll` and import `.lib` as outputs of
the same link edge when the native linker produces both. Stella should store the
same multi-output relationship in its action state.

## Deferred

Not included in Q173:

- official Windows host support
- automatic `.exe` or `.lib` suffix selection
- real `link.exe`, `lld-link`, `lib.exe`, or `llvm-lib` validation
- PDB/debug ownership syntax
- Windows release asset packaging
- named pipe Stella daemon

These are future implementation rounds and must not be smuggled into the Graph
IR contract as implicit behavior.
