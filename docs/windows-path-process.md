# Windows Path And Process Prep

Round Q98 prepares the Windows port without declaring Windows official host
support. QStar can already model Windows-like toolchains in dry-run and command
planning, but source build, install layout, shell-free process execution, and
release packaging still need native Windows validation before a Windows release
artifact exists.

Round Q114 moves this from a docs-only pre-port note to a native validation
candidate path. Round Q158 hardens the highest-risk pre-port layer: path
diagnostics, MSVC/clang-cl response-file escaping, `.exe` artifact naming, and
the shell-free argv-vector contract. The local gate creates real MSVC-style
response files with a fake `clang-cl` fixture, asserts drive-letter/backslash
diagnostics, and keeps a manual Windows GitHub Actions candidate workflow. The
response-file regression is exercised through both Stella and Ninja when `ninja`
is available. Round Q159 turns that workflow into a manual native CI alpha with
MSYS2 UCRT64 bootstrap, `qstar --version`, a limited smoke subset, install
docs/man smoke, and uploaded failure logs. This still is not official Windows
support. Round Q160 moves the artifact naming/install policy into
`docs/windows-artifact-policy.md`. Round Q164 isolates the Unix socket Stella
daemon code behind a Windows stub so MSYS2 `make all CC=gcc` can progress past
the previously observed `<sys/socket.h>` failure. Round Q165 seals the artifact
policy with explicit `.exe` introspection, fake static `.lib` Stella/Ninja
builds, and Windows sharedlib diagnostic parity. The daemon remains disabled on
Windows until a named-pipe transport and ACL policy are implemented. Round Q174
adds a dedicated Windows artifact corpus that builds target-local and
profile-mapped `.exe`/static `.lib` artifacts through Stella and Ninja, then
checks stage/install layout while keeping Windows sharedlib deferred.
Round Q178 adds `tests/corpus/windows-execution` and the
`qstar-windows-execution-corpus-tests` target. This is the first Windows alpha
gate that builds and runs real executables in the MSYS2 UCRT64 GCC lane instead
of only proving Windows-like graph contracts.
Round Q179 splits POSIX process execution from the Windows compile boundary:
`src/executor.c` and `src/ninja.c` now compile as `_WIN32` stubs without POSIX
`<poll.h>`, `fork`, `waitpid`, pipe, or Unix launcher headers. Native Stella and
Ninja-backed execution still need a future CreateProcess runner. Q179 also
introduces `qstar_platform_mkdir` and `qstar_platform_lstat` so the baseline
Windows build does not depend on POSIX `mkdir(path, mode)` or `lstat`.

## Status

```txt
host support: manual native CI alpha
official release artifact: none
purpose of this document: pre-port contract
gate: make -C qstar qstar-windows-prep-tests
alpha smoke: make -C qstar qstar-windows-native-alpha-tests
execution corpus: make -C qstar qstar-windows-execution-corpus-tests
alpha workflow: .github/workflows/windows-validation.yml
```

Linux has validation and release-candidate packaging dry-run coverage. Windows
has a manual alpha lane, but remains unofficial until QStar has a green regular
Windows CI lane, source build, install smoke, response-file execution with real
Windows tools, and artifact packaging story. Q178 starts that execution path for
MSYS2 UCRT64 GCC; Q179 moves the process-runner failure from POSIX headers to an
explicit unsupported CreateProcess boundary and starts closing filesystem helper
signature differences; MSVC/clang-cl execution is still deferred.

## Path Normalization Rule

QStar DSL path fields use package-relative, slash-normalized paths on every
host:

```lua
sources = {"src/main.c"}
include_dirs = {"sdk/include"}
outputs = {qstar.output("build/qstar/generated/value.c")}
```

Do not write Windows separators or drive letters in source/header/output/stage
paths:

```lua
sources = {"src\\main.c"}       -- invalid
include_dirs = {"C:\\SDK\\inc"} -- invalid
include_dirs = {"C:/SDK/inc"}   -- invalid
```

This keeps labels, dependency keys, compile database paths, generated output
ownership, LSP navigation, and Ninja lowering stable across hosts. Windows
absolute tool paths belong in toolchain/profile resolution, not package file
paths.

## Drive Letters And Escaping

Lua string escaping makes backslash-heavy Windows paths especially easy to get
wrong. QStar avoids that by keeping project files canonical:

- package paths: always slash-normalized and package-relative
- tool names: bare names such as `clang-cl`, `lld-link`, `link.exe`
- absolute tool paths: future Windows support may allow them through explicit
  `tool_overrides` plus `allow_absolute_tools`, but not as source paths
- user shell paths on the CLI: handled by the host shell before QStar sees them

Round Q114 diagnostics distinguish common mistakes:

- `C:\project\src\main.c` and `C:/project/src/main.c` report that drive-letter
  paths are not allowed. The emitted reason text is
  `drive-letter paths are not allowed in package paths`.
- `sdk\include` reports that backslashes are not allowed and `/` separators are
  required. The emitted reason text is `backslash paths are not normalized`.
- `../escape`, `./local`, duplicate separators, and colon-containing package
  paths remain invalid package paths.

## Process Model

QStar commands remain shell-free argv vectors:

```lua
qstar.custom_target "probe" {
  outputs = {qstar.output("build/qstar/generated/argv.txt")},
  command = qstar.cli {
    "tool-name",
    qstar.output(0),
    "argument with spaces",
    "semi;colon",
  },
}
```

QStar must pass each list item as one argv element. It must not expand `$VAR`,
split on spaces, interpret semicolons, or run commands through a shell. The
Windows port should use the platform process API with the same argv-vector
semantics, not a shell command string.

Until that runner lands, `_WIN32` Stella build/test execution and QStar's Ninja
launcher fail with explicit diagnostics instead of accidentally depending on
POSIX process APIs. This is intentional: `qstar check`, `qstar dry-run`, and
`qstar emit-ninja` can continue validating graph contracts while real Windows
execution remains a separate implementation step.

Local non-Windows tests use `tests/corpus/response-files/tools/fake-clang-cl` to
prove that QStar itself preserves argv structure and response-file escaping. That
fixture is not a compiler and must not be used as evidence of native Windows
toolchain support.

## Response Files

Response-file policy belongs to `qstar.profile`:

```lua
qstar.profile "windows-msvc" {
  target = "x86_64-pc-windows-msvc",
  cc = "clang-cl",
  linker = "clang-cl",
  response_files = "on",
  response_style = "msvc",
}
```

Current styles:

- `posix`: shell-style quoting for POSIX compiler response files
- `windows`: Windows command-line double-quote/backslash quoting
- `msvc`: MSVC-compatible double-quote/backslash quoting

For `x86_64-pc-windows-msvc`, `response_style` should be `msvc`. For MinGW-like
targets, `windows` may be appropriate, but native validation is still pending.

The prep gate verifies MSVC response-file escaping for:

- arguments with spaces: `"/DNAME=alpha beta"`
- embedded double quotes: `"/DQUOTE=\"value\""`
- trailing backslashes: `"/DTRAIL=tail\\"`
- semicolons preserved without shell splitting: `/DSEMICOLON=a;b`
- Windows-like argv option paths with spaces and backslashes:
  `"/DWINPATH=C:\Program Files\QStar\Include"`
- JSON-like values with quotes and backslashes:
  `"/DJSON={\"path\":\"C:\qstar\include\"}"`
- an argument ending with a literal space:
  `"/DSPACE_TRAIL=value with trailing space "`
- a backslash immediately before an embedded quote:
  `"/DSLASHQUOTE=C:\qstar\\\"quoted\""`
- linker paths with spaces: `"/PDB:build/qstar/pdb/windows rsp.pdb"`
- `/LIBPATH:...` paths with spaces

## Artifact Naming

The detailed artifact contract is `docs/windows-artifact-policy.md`. Summary:

- Executable targets may use `artifact_name = "tool.exe"` or profile
  `artifact_names = {"//:tool=tool.exe"}`. This `.exe` spelling is the current
  Windows executable naming contract. The local prep gate verifies both target
  local `artifact_name` mapping.
- External Windows libraries in `libs = {"kernel32"}` render as `kernel32.lib`
  for Windows/MSVC-like targets. This external library spelling is sealed for
  the pre-port contract.
- `lib_dirs` render as `/LIBPATH:<path>` for MSVC-like targets.
- `qstar.staticlib` still uses the current cross-host static archive default
  `lib<name>.a`. If a Windows archive name is required before native archive
  support lands, use `artifact_name = "name.lib"` or profile
  `artifact_names = {"//:name=name.lib"}` explicitly. Automatic `.lib` output is
  deferred until native `lib.exe`/`llvm-lib` validation.
- `qstar.sharedlib` is supported for Darwin-like `.dylib` and Linux-like `.so`
  profiles, but Windows runtime `.dll`, import `.lib`, PDB/debug artifact,
  runtime search path, and install layout are deferred.

Do not claim Windows packaging support until `.exe`, static `.lib`, runtime
`.dll`, import `.lib`, debug artifact, and install layout behavior are
validated on Windows.

## Regression Gate

Run the local prep gate:

```sh
make qstar-windows-prep-tests
```

Run the native execution corpus:

```sh
make qstar-windows-execution-corpus-tests
```

On Windows this corpus is run by the manual `windows-latest` workflow under
MSYS2 UCRT64 GCC. On non-Windows hosts it still provides a contract-execution
smoke against the same QStar graph.

The gate checks:

- response-file corpus build through Stella
- argv-vector custom target with spaces, quotes, semicolons, and `$`
- compile database paths remain slash-normalized
- Windows/MSVC dry-run uses `response_style=msvc`
- fake Windows/MSVC build creates real compile and link `.rsp` files
- MSVC response escaping for spaces, quotes, semicolons, and trailing
  backslashes
- MSVC response escaping for Windows-like define/path options, JSON-like values,
  trailing-space arguments, and backslash-before-quote arguments
- Windows/MSVC dry-run renders `/link`, `/LIBPATH:...`, and `kernel32.lib`
- `artifact_name = "windows_app.exe"` is reflected in the planned output path
  and target metadata.
- target-local `artifact_name = "mapped_named.exe"` is reflected in the planned
  output path
- target-local `artifact_name = "windows_static.lib"` is reflected in explicit
  static archive planning
- fake static `.lib` artifacts are built through Stella and Ninja on non-Windows
  hosts without claiming native `lib.exe`/`llvm-lib` support
- the Windows artifact corpus stages and installs explicit `.exe` files under
  `bin/` and explicit static `.lib` files under `lib/`
- Windows-like `qstar.sharedlib` targets fail with the same deferred
  runtime `.dll`, import `.lib`, and PDB/debug diagnostic for Stella and Ninja
- drive-letter and backslash package paths are rejected with specific reason text

The execution corpus checks:

- C executable build and run
- static archive plus executable link and run
- forced response-file compile/link and executable run
- generated object artifact bridge dry-run, build, and run
- install prefix smoke for executable and static archive artifacts

Manual corpus commands:

```sh
./build/bin/qstar --file tests/corpus/response-files/qstar.lua build //:all
./build/bin/qstar --file tests/corpus/response-files/qstar.lua \
  --target x86_64-pc-windows-msvc --toolchain clang dry-run //:windows_app
./build/bin/qstar --file tests/corpus/response-files/qstar.lua \
  --target x86_64-pc-windows-msvc --toolchain clang build //:windows_rsp
```

These commands prepare the port; they do not replace the manual native Windows
alpha workflow.

## Windows GitHub Actions Alpha

`.github/workflows/windows-validation.yml` is intentionally manual-only through
`workflow_dispatch`. It checks out submodules, uses an MSYS2 UCRT64 environment,
runs `make all CC=gcc`, records `qstar --version`, runs
`make qstar-windows-native-alpha-tests CC=gcc`, runs
`make qstar-windows-prep-tests CC=gcc`, and performs an install docs/manpage
smoke under `/tmp/qstar-windows-smoke`. When the `run_ninja_parity=true` input
is enabled it also runs `make qstar-ninja-backend-parity-tests CC=gcc`.

The workflow uploads a `qstar-windows-native-alpha` artifact with environment,
build, smoke, and install logs. This workflow is an alpha porting lane, not a
release gate. It must become repeatedly green and be promoted to regular CI
before README or release notes can claim Windows host support.

The native alpha tracking document is `docs/windows-native-alpha.md`.
