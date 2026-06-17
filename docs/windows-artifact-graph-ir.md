# Windows Artifact Graph IR Contract

Round Q173 fixed the Graph IR direction for Windows artifact implementation
before runtime `.dll` and import `.lib` support was added. Round Q223 promotes
that direction into the current Graph IR contract: QStar can now model and
select Windows shared-library runtime/import artifacts, while actual
Stella/Ninja Windows shared-library lowering remains deferred.

This is still not a claim of official Windows host support.

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

Each target lowers to an ordered artifact map. The first entry is the primary
artifact. Secondary artifacts are addressable by artifact id.

```txt
artifact id=runtime role=sharedlib path=build/qstar/out/___plugin/plugin.dll install_dir=bin primary=true installable=true
artifact id=import_lib role=import_lib path=build/qstar/out/___plugin/plugin.lib install_dir=lib primary=false installable=true
```

Role meanings:

| Artifact id | Role metadata | Producer | Primary | Install default | Notes |
| --- | --- | --- | --- | --- | --- |
| `runtime` | `exe` | `qstar.executable` / `qstar.test` | yes | `bin/` for installable executable targets | `.exe` when explicitly named or future Windows default suffix |
| `archive` | `staticlib` | `qstar.staticlib` | yes | `lib/` | Unix `.a` and Windows static `.lib` are the same static role with different names |
| `runtime` | `sharedlib` | `qstar.sharedlib` | yes | `bin/` on Windows, `lib/` on Darwin/Linux | Runtime `.dll` on Windows; `qstar.target_file(label)` resolves here |
| `import_lib` | `import_lib` | Windows `qstar.sharedlib` | no | `lib/` | Link interface consumed by dependents when lowering lands |
| `debug_symbols` | `debug_symbols` | opt-in linker/debug policy | no | none | PDB/debug artifacts are deferred and never implicit |

The existing `artifact_name` field remains the primary artifact basename. It
must not be overloaded to name both `.dll` and import `.lib`.

## Public Resolution Rules

Existing projects keep the current rule:

```lua
qstar.target_file("//:plugin")
```

This resolves to the primary artifact. For Windows shared libraries, the primary
artifact is the runtime `.dll`.

Q223 implements an optional selector form for secondary artifacts:

```lua
qstar.target_file("//:plugin", { artifact = "import_lib" })
```

Selector names are artifact ids, not filenames. Unknown selectors fail with a
diagnostic that lists known artifacts for the target. `debug_symbols` stays
unavailable until the target has explicitly opted into QStar-owned debug
artifacts.

## Link Semantics

Link consumers should not choose the import library manually once Windows
shared-library lowering lands. When a Windows
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

The Windows `link-shared` action is not executable yet. Q223 deliberately stops
at Graph IR and selector resolution. The eventual Windows `link-shared` action
should have a multi-output record:

```txt
action id=//:plugin:link-shared:0
output role=runtime path=build/qstar/out/__plugin/plugin.dll primary=1
output role=import_lib path=build/qstar/out/__plugin/plugin.lib primary=0
```

If PDB support is later owned by QStar:

```txt
output role=debug_symbols path=build/qstar/out/__plugin/plugin.pdb primary=0 optional=1
```

Stella dirty-check state and Ninja lowering must later track all outputs of the
same final action. A missing import `.lib` should dirty the sharedlib action
even if the runtime `.dll` exists.

## JSON And Explain Compatibility

Existing JSON fields should remain compatible:

- `artifact_name`: primary artifact basename
- `installable`: whether the target has installable artifacts

Target-list JSON includes an `artifacts` array:

```json
{
  "label": "//:plugin",
  "kind": "sharedlib",
  "artifact_name": "plugin.dll",
  "artifacts": [
    {
      "id": "runtime",
      "role": "sharedlib",
      "path": "build/qstar/out/___plugin/plugin.dll",
      "install_dir": "bin",
      "primary": true,
      "installable": true
    },
    {
      "id": "import_lib",
      "role": "import_lib",
      "path": "build/qstar/out/___plugin/plugin.lib",
      "install_dir": "lib",
      "primary": false,
      "installable": true
    }
  ]
}
```

`explain`, `dry-run`, `query`, `list-targets`, and `list-targets --format json`
display the artifact map. Windows `sharedlib` dry-run/explain also prints:

```txt
plan_diagnostic kind=windows-sharedlib-lowering status=deferred artifacts=modeled doc=docs/windows-artifact-graph-ir.md
```

This makes the `.dll`/import `.lib` split visible without pretending that the
backend can already link the shared library.

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

Install dry-run can also show the eventual layout:

```txt
install_file src=build/qstar/out/___plugin/plugin.dll ... role=sharedlib artifact=runtime
install_file src=build/qstar/out/___plugin/plugin.lib ... role=import_lib artifact=import_lib
```

Non-dry-run Windows sharedlib install remains deferred because no backend yet
produces the two files.

## Stella And Ninja Parity

Stella and Ninja must agree on:

- artifact role names
- primary artifact path
- secondary artifact paths
- consumer link inputs
- install/stage producer resolution
- unsupported selector diagnostics
- Windows sharedlib deferred-lowering diagnostics until implementation lands

Ninja lowering should emit the runtime `.dll` and import `.lib` as outputs of
the same link edge when the native linker produces both. Stella should store the
same multi-output relationship in its action state.

## Deferred

Not included in Q223:

- official Windows host support
- automatic `.exe` or `.lib` suffix selection
- real `link.exe`, `lld-link`, `lib.exe`, or `llvm-lib` validation
- PDB/debug ownership syntax
- Windows sharedlib Stella/Ninja lowering
- automatic dependent linking against `import_lib`
- Windows release asset packaging
- named pipe Stella daemon

These are future implementation rounds and must not be smuggled into the Graph
IR contract as implicit behavior.
