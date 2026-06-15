# Windows Artifact Policy

Round Q173 seals the implementation plan for Windows `.exe`, static `.lib`,
runtime `.dll`, import `.lib`, and PDB/debug artifacts before full Windows
support. Round Q174 promotes executable and static-library artifacts from
contract-only planning to a local regression gate.

```txt
host support: manual native CI alpha
release asset: none
policy status: pre-support artifact contract
implementation plan: sealed for Q173
exe/static artifact gate: sealed for Q174
local gate: make qstar-windows-prep-tests
native alpha gate: make qstar-windows-native-alpha-tests
```

## Executable Artifacts

Executable targets use explicit target-local naming when a Windows-style suffix
is required:

```lua
qstar.executable "tool" {
  sources = {"src/main.c"},
  artifact_name = "tool.exe",
}
```

Install and stage policy:

- install role: `bin/<artifact-basename>`
- stage role: explicit `qstar.stage_file(qstar.target_file("//:tool"), "...")`
- `qstar.target_file("//:tool")`: primary executable artifact path

## Static Library Artifacts

static archive `.lib` outputs are explicit target-local names until real Windows
archive tools are validated:

```lua
qstar.staticlib "windows_static" {
  sources = {"src/core.c"},
  artifact_name = "windows_static.lib",
}
```

The local gate builds a fake static `.lib` with a package-local fake archiver so
the `windows_static.lib` artifact path is tested beyond dry-run. This does not
claim native `lib.exe` or `llvm-lib` support.

Install and stage policy:

- install role: `lib/<artifact-basename>`
- stage role: explicit `qstar.stage_file(qstar.target_file("//:core"), "...")`
- `qstar.target_file("//:core")`: primary static library artifact path

## Shared Library Artifacts

Windows shared libraries remain deferred. The planned artifact classes are:

- runtime .dll: primary artifact
- import .lib: secondary link/interface artifact
- PDB/debug: optional and opt-in/deferred

The primary artifact rule is:

- `qstar.target_file("//:plugin")` points to the runtime .dll
- dependent targets link against the import .lib when Windows sharedlib support lands
- the import `.lib` is addressable through a future selector:
  `qstar.target_file("//:plugin", { artifact = "import_lib" })`
- PDB/debug output is opt-in/deferred and never silently installed

Final install/stage direction:

- runtime .dll: `bin/`
- import .lib: `lib/`
- static .lib: `lib/`
- executable .exe: `bin/`
- PDB/debug artifact: no implicit install

## Stella and Ninja parity

When Windows sharedlib support is implemented, Stella and Ninja parity means:

- both backends compute the same artifact map
- both emit the same primary runtime .dll path
- both track the import .lib as an output of the same link-shared action
- both link consumers through the import .lib, not the runtime .dll
- both keep PDB/debug artifacts out of install/stage unless explicitly owned
- both preserve `qstar.target_file(label)` as the primary artifact path
- both reject unsupported selector names with the same diagnostic

## Diagnostics

Windows sharedlib diagnostic parity stays part of the gate while runtime .dll
plus import .lib implementation is deferred:

```txt
qstar: sharedlib target '//:plugin' is not supported for Windows-like platform
contexts yet; Windows shared libraries require a runtime .dll, import .lib, and optional
PDB/debug artifact policy. Use custom_target/object bridge for now or see
docs/windows-artifact-policy.md
```

## Regression Gate

Run:

```sh
make qstar-windows-prep-tests
```

The gate verifies explicit `.exe` naming, external `.lib` spelling, `/LIBPATH`,
MSVC response-file escaping, slash-normalized package paths, explicit static
`.lib` artifact planning, fake static `.lib` build output through Stella and
Ninja, `.exe` and static `.lib` install/stage layout through the Windows
artifacts corpus, and Windows sharedlib diagnostic parity. It does not claim
official Windows support.
