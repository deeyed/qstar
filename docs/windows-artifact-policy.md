# Windows Artifact Policy

Round Q173 seals the implementation plan for Windows `.exe`, static `.lib`,
runtime `.dll`, import `.lib`, and PDB/debug artifacts before full Windows
support. Round Q174 promotes executable and static-library artifacts from
contract-only planning to a local regression gate. Round Q223 sealed the
Windows shared-library artifact map, Round Q224 lowers that map through
Stella and Ninja, Round Q225 promotes the shared-library checks into a
named beta-candidate release gate, and Round Q246 adds the Windows public beta
package skeleton without publishing a Windows asset.

```txt
host support: validation-backed beta candidate
release asset: planned qstar-v<version>-windows-x86_64.zip, not published
policy status: beta-candidate artifact contract
implementation plan: sealed for Q173
exe/static artifact gate: sealed for Q174
sharedlib Graph IR gate: sealed for Q223
sharedlib lowering gate: sealed for Q224
sharedlib parity gate: sealed for Q225
package skeleton gate: sealed for Q246
local prep gate: make qstar-windows-prep-tests
native smoke gate: make qstar-windows-native-alpha-tests
named sharedlib gate: make qstar-windows-sharedlib-artifact-parity-tests
package contract gate: make qstar-windows-release-package-tests
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

Windows shared-library targets model and lower multiple artifacts:

- runtime .dll: primary artifact
- import .lib: secondary link/interface artifact
- PDB/debug: optional and opt-in/deferred

The primary artifact rule is:

- `qstar.target_file("//:plugin")` points to the runtime .dll
- dependent targets link against the import .lib on Windows
- the import `.lib` is addressable through the selector:
  `qstar.target_file("//:plugin", { artifact = "import_lib" })`
- PDB/debug output is opt-in/deferred and never silently installed

Final install/stage direction:

- runtime .dll: `bin/`
- import .lib: `lib/`
- static .lib: `lib/`
- executable .exe: `bin/`
- PDB/debug artifact: no implicit install

## Stella and Ninja parity

Q224 seals the Windows sharedlib lowering side. Stella and Ninja parity means:

- both backends compute the same artifact map
- both emit the same primary runtime .dll path
- both track the import .lib as an output of the same link-shared action
- both link consumers through the import .lib, not the runtime .dll
- both keep PDB/debug artifacts out of install/stage unless explicitly owned
- both preserve `qstar.target_file(label)` as the primary artifact path
- both reject unsupported selector names with the same diagnostic

## Diagnostics

Windows sharedlib diagnostics now focus on ordinary invalid states:

- unknown artifact selectors list the known artifacts for the target
- unsupported non-Darwin/non-Linux/non-Windows platform contexts reject
  `qstar.sharedlib`
- PDB/debug selectors remain unavailable until QStar owns such artifacts

## Regression Gate

Run:

```sh
make qstar-windows-prep-tests
make qstar-windows-sharedlib-artifact-parity-tests
```

`qstar-windows-prep-tests` verifies explicit `.exe` naming, external `.lib`
spelling, `/LIBPATH`, MSVC response-file escaping, slash-normalized package
paths, explicit static `.lib` artifact planning, fake static `.lib` build
output through Stella and Ninja, `.exe` and static `.lib` install/stage layout
through the Windows artifacts corpus, slash-normalized install/stage manifests,
Windows sharedlib runtime/import-lib Graph IR, selector resolution,
Stella/Ninja multi-output lowering, dependent executable linkage through the import `.lib`,
install/stage layout for runtime/import artifacts, and unknown selector
diagnostics.

`qstar-windows-sharedlib-artifact-parity-tests` is the named Q225 release gate
for the shared-library subset. It focuses on runtime `.dll` plus import `.lib`
artifact maps, Stella/Ninja multi-output lowering, action-log output counts,
consumer import-library links, stage/install layout, manifest normalization,
and root `.ninja_*` pollution guards.

The Windows execution corpus adds real MSYS2 GCC coverage for `.exe -> bin`,
static archive -> `lib`, generated object bridge staging, and Windows sharedlib
runtime/import-lib build artifacts through both Stella and Ninja. These gates
make Windows a validation-backed beta candidate; they still do not claim
official Windows support or publish a Windows release asset. The Q246 package
skeleton fixes the future zip name and runtime layout, but upload and
download-smoke remain future release gates.
