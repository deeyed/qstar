# Windows Artifact Policy

Round Q160 freezes the pre-support artifact contract for Windows-like C and C++
projects. This is still not official Windows host support. The policy exists so
future Windows work can add native execution, packaging, and CI without changing
QStar authoring syntax again.

Round Q173 seals the implementation plan for Windows `.exe`, static `.lib`,
runtime `.dll`, import `.lib`, and PDB/debug artifacts before the shared library
implementation starts. The matching Graph IR design note is
`docs/windows-artifact-graph-ir.md`, and the forward-looking corpus lives in
`tests/corpus/windows-artifacts`.

Round Q174 promotes executable and static-library artifacts from contract-only
planning to a local regression gate. The `tests/corpus/windows-artifacts`
fixture now builds explicit `.exe` and static `.lib` outputs with a fake
Windows-like toolchain, verifies target-local and profile-level artifact names,
checks Stella/Ninja lowering, stages those artifacts, and installs them under
the Windows layout. Windows shared libraries remain deferred.

## Status

```txt
host support: manual native CI alpha
release asset: none
policy status: pre-support artifact contract
implementation plan: sealed for Q173
exe/static artifact gate: sealed for Q174
local gate: make qstar-windows-prep-tests
native alpha gate: make qstar-windows-native-alpha-tests
```

The policy intentionally separates names QStar can already plan from artifacts
that still need native Windows validation.

## Executable Artifacts

Executable targets may produce a Windows-style `.exe` name when the author names
the artifact explicitly:

```lua
qstar.executable "tool" {
  sources = {"src/main.c"},
  artifact_name = "tool.exe",
}
```

Profile-level names are also supported:

```lua
qstar.profile "windows-msvc" {
  target = "x86_64-pc-windows-msvc",
  artifact_names = {
    "//:tool=tool.exe",
  },
}
```

This is the final `.exe` naming contract for the Windows implementation plan.
QStar does not automatically add `.exe` merely because a profile target looks
Windows-like. Automatic host-specific suffix selection may be added only after
native Windows validation is stable, and it must not change existing explicit
`artifact_name` or profile `artifact_names` behavior.

Install and stage policy:

- install role: `bin/<artifact-basename>`
- stage role: explicit `qstar.stage_file(qstar.target_file("//:tool"), "...")`
- `qstar.target_file("//:tool")`: returns the primary executable artifact path

## Static Library Artifacts

The cross-host default for `qstar.staticlib` remains Unix-style `lib<name>.a`.
Windows-style static archives use explicit artifact naming until real Windows
archive tools are validated:

```lua
qstar.staticlib "windows_static" {
  sources = {"src/core.c"},
  artifact_name = "windows_static.lib",
}
```

or:

```lua
qstar.profile "windows-msvc" {
  target = "x86_64-pc-windows-msvc",
  artifact_names = {
    "//:windows_static=windows_static.lib",
  },
}
```

This fixes QStar's artifact path, planning contract, Stella action-log surface,
and Ninja lowering path. It also deliberately separates two different `.lib`
meanings:

- static archive `.lib`: primary artifact of `qstar.staticlib`
- import `.lib`: secondary artifact of Windows `qstar.sharedlib`

Those two roles must not share an output group or be inferred from the same
field. The local prep gate already builds a fake
`windows_static.lib` with a package-local fake archiver so the `.lib` artifact
path is tested beyond dry-run. That fixture does not claim that the selected
archive tool is a native `lib.exe` or `llvm-lib` compatible archiver. Native
`.lib` archive production needs a later validation pass with real Windows tools.

Install and stage policy:

- install role: `lib/<artifact-basename>`
- stage role: explicit `qstar.stage_file(qstar.target_file("//:core"), "...")`
- `qstar.target_file("//:core")`: returns the primary static library artifact

## External Windows Libraries

For MSVC-like targets, external system libraries in `libs` are rendered with the
`.lib` spelling:

```lua
qstar.executable "app" {
  sources = {"src/main.c"},
  libs = {"kernel32"},
}
```

The planned link command uses `kernel32.lib`. `lib_dirs` render as
`/LIBPATH:<path>` for MSVC-like linkers.

## Shared Library Artifacts

Windows shared libraries are not enabled yet, but Q173 fixes the implementation
contract. A Windows `qstar.sharedlib` has these artifact classes:

- runtime library: `.dll`, primary artifact
- import library: `.lib`, secondary link/interface artifact
- debug artifact: `.pdb`, optional and opt-in/deferred

The primary artifact rule is stable:

- `qstar.target_file("//:plugin")` points to the runtime `.dll`
- dependent executable/sharedlib targets link against the import `.lib`
  automatically when the producer is a Windows sharedlib
- the import `.lib` is addressable through a future selector form:
  `qstar.target_file("//:plugin", { artifact = "import_lib" })`
- PDB/debug output is opt-in/deferred and never silently installed

The selector form extends the existing helper instead of introducing a second
artifact helper. It keeps QStar authoring centered around `qstar.target_file`,
while preserving the current one-argument meaning for all existing projects.

Until that multi-artifact implementation lands, Stella and Ninja reject Windows-like
`qstar.sharedlib` targets with a diagnostic that names the missing runtime
`.dll`, import `.lib`, and PDB/debug artifact policy.

## PDB And Debug Artifacts

PDB/debug artifacts are deferred. They should be opt-in metadata or an explicit
debug artifact in a future release. They are not part of `qstar.target_file` and
must not be installed or staged implicitly.

Q173 does not introduce a stable PDB syntax. Linker options may still mention
`/PDB:<path>` manually, but that path is not a QStar-owned debug artifact until
the Graph IR has an explicit `debug_symbols` role. A later implementation may
make it addressable as:

```lua
qstar.target_file("//:plugin", { artifact = "debug_symbols" })
```

only when the target explicitly opts into debug artifact ownership.

Final install/stage direction:

- runtime `.dll`: `bin/`
- import `.lib`: `lib/`
- static `.lib`: `lib/`
- executable `.exe`: `bin/`
- PDB/debug artifact: no implicit install; opt-in role such as `symbols/` or
  explicit stage entry after debug artifact ownership exists
- public headers: `include/`

Stage follows the same roles, but stage destinations are explicit:

```lua
qstar.stage "sdk" {
  root = "build/qstar/stage/sdk",
  files = {
    qstar.stage_file(qstar.target_file("//:tool"), "bin/tool.exe"),
    qstar.stage_file(qstar.target_file("//:core"), "lib/core.lib"),
    qstar.stage_file(qstar.target_file("//:plugin"), "bin/plugin.dll"),
    qstar.stage_file(qstar.target_file("//:plugin", { artifact = "import_lib" }),
      "lib/plugin.lib"),
  },
}
```

The last selector is reserved for the Windows sharedlib implementation and is
not part of the current stable runtime surface yet.

## Ninja Lowering Scope

Supported pre-port planning:

- executable `.exe` names through target-local `artifact_name`
- executable `.exe` names through profile-level `artifact_names`
- static `.lib` names through explicit `artifact_name` or `artifact_names`
- external MSVC-like libraries rendered as `<name>.lib`
- `/LIBPATH:<path>` for MSVC-like `lib_dirs`
- MSVC response file escaping through `response_style = "msvc"`

Unsupported until later Windows validation:

- automatic `.exe` or `.lib` suffix selection
- native `lib.exe`/`llvm-lib` archive semantics
- `qstar.sharedlib` Windows `.dll`/import `.lib` lowering
- PDB/debug artifact modeling
- Windows release asset packaging

Ninja and Stella should reject the same unsupported Windows sharedlib graph. The
backend choice must not change the Windows artifact contract.

When Windows sharedlib support is implemented, Stella and Ninja parity means:

- both backends compute the same artifact map
- both emit the same primary runtime `.dll` path
- both track the import `.lib` as an output of the same link-shared action
- both link consumers through the import `.lib`, not the runtime `.dll`
- both keep PDB/debug artifacts out of install/stage unless explicitly owned
- both preserve `qstar.target_file(label)` as the primary artifact path
- both reject unsupported selector names with the same diagnostic

## Diagnostics

Good Windows sharedlib diagnostics should explain that QStar is missing a
multi-artifact policy, not that the target rule is generally invalid:

```txt
qstar: sharedlib target '//:plugin' is not supported for Windows-like profiles
yet; Windows shared libraries require a runtime .dll, import .lib, and optional
PDB/debug artifact policy. Use custom_target/object bridge for now or see
docs/windows-artifact-policy.md
```

For unsupported source languages that can still produce objects, use the object
artifact bridge documented in `wiki/reference/object-artifacts.md` instead of
adding language-specific target rules.

## Regression Gate

Run:

```sh
make qstar-windows-prep-tests
```

The gate verifies explicit `.exe` naming, profile `artifact_names`, external
`.lib` spelling, `/LIBPATH`, MSVC response-file escaping, slash-normalized
package paths, explicit static `.lib` artifact planning, fake static `.lib`
build output through Stella and Ninja, `.exe` and static `.lib` install/stage
layout through the Windows artifacts corpus, and Windows sharedlib diagnostic
parity. It does not claim official Windows support.
Windows sharedlib diagnostic parity stays part of the gate while runtime `.dll`
plus import `.lib` implementation is deferred.

`tests/corpus/windows-artifacts` is now part of the prep gate. It keeps the
expected `.exe`, static `.lib`, runtime `.dll`, import `.lib`, install/stage,
and Stella/Ninja parity surface in one Windows-focused project shape. The
runtime `.dll` plus import `.lib` part remains a future implementation target,
while executable and static library artifacts are regression-tested today.
