# Windows Artifact Policy

Round Q160 freezes the pre-support artifact contract for Windows-like C and C++
projects. This is still not official Windows host support. The policy exists so
future Windows work can add native execution, packaging, and CI without changing
QStar authoring syntax again.

## Status

```txt
host support: manual native CI alpha
release asset: none
policy status: pre-support artifact contract
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

This is the current `.exe` contract. QStar does not automatically add `.exe`
just because a profile target looks Windows-like. Automatic host-specific suffix
selection is deferred until native Windows validation is stable.

Install and stage policy:

- install role: `bin/<artifact-name>`
- stage role: explicit `qstar.stage_file(qstar.target_file("//:tool"), "...")`
- `qstar.target_file("//:tool")`: returns the primary executable artifact path

## Static Library Artifacts

The cross-host default for `qstar.staticlib` remains `lib<name>.a`. A
Windows-style static library name is available only through explicit artifact
naming:

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

This only fixes QStar's artifact path and planning contract. It does not claim
that the selected archive tool is a native `lib.exe` or `llvm-lib` compatible
archiver. Native `.lib` archive production needs a later validation pass with
real Windows tools.

Install and stage policy:

- install role: `lib/<artifact-name>`
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

Windows shared libraries are not enabled yet. The desired future contract has at
least three artifact classes:

- runtime library: `.dll`
- import library: `.lib`
- debug artifact: `.pdb`, optional and opt-in

QStar's current artifact target model exposes one primary `target_file`. Windows
shared libraries need a multi-artifact policy before QStar can support them
without ambiguity. The expected future rule is:

- `qstar.target_file("//:plugin")` points to the runtime `.dll`
- import `.lib` is exposed through a separate future API or explicit generated
  output, not by overloading the runtime artifact
- PDB/debug output is opt-in and never silently installed

Until that multi-artifact policy exists, Stella and Ninja reject Windows-like
`qstar.sharedlib` targets with a diagnostic that names the missing runtime
`.dll`, import `.lib`, and PDB/debug artifact policy.

## PDB And Debug Artifacts

PDB/debug artifacts are deferred. They should be opt-in metadata or an explicit
debug artifact in a future release. They are not part of `qstar.target_file` and
must not be installed or staged implicitly.

Tentative future install/stage policy:

- runtime `.dll`: `bin/`
- import `.lib`: `lib/`
- PDB/debug artifact: opt-in role such as `symbols/` or explicit stage entry
- public headers: `include/`

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
package paths, and explicit static `.lib` artifact planning. It does not claim
official Windows support.
