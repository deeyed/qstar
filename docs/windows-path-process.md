# Windows Path And Process Prep

Round Q98 prepares the Windows port without declaring Windows official host
support. QStar can already model Windows-like toolchains in dry-run and command
planning, but source build, install layout, shell-free process execution, and
release packaging still need native Windows validation before a Windows release
artifact exists.

Round Q114 moves this from a docs-only pre-port note to a native validation
candidate path. The local gate now creates real MSVC-style response files with a
fake `clang-cl` fixture, asserts drive-letter/backslash diagnostics, and keeps a
manual Windows GitHub Actions candidate workflow. The response-file regression
is exercised through both Stella and Ninja when `ninja` is available. This still
is not official Windows support.

## Status

```txt
host support: planned validation
official release artifact: none
purpose of this document: pre-port contract
gate: make -C qstar qstar-windows-prep-tests
candidate workflow: .github/workflows/windows-validation.yml
```

Linux has validation and release-candidate packaging dry-run coverage. Windows
remains planned until QStar has a green native Windows CI lane, source build,
install smoke, response-file execution with real Windows tools, and artifact
packaging story.

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
  `drive-letter paths are not allowed`.
- `sdk\include` reports that backslashes are not allowed and `/` separators are
  required. The emitted reason text is `backslashes are not allowed`.
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
- linker paths with spaces: `"/PDB:build/qstar/pdb/windows rsp.pdb"`
- `/LIBPATH:...` paths with spaces

## Artifact Naming

Current pre-port policy:

- Executable targets may use `artifact_name = "tool.exe"` or profile
  `artifact_names = {"//:tool=tool.exe"}`. This `.exe` spelling is the current
  Windows executable naming contract.
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
  profiles, but Windows `.dll`, import library, PDB, runtime search path, and
  install layout are deferred.

Do not claim Windows packaging support until `.exe`, static `.lib`, `.dll`,
import library, debug artifact, and install layout behavior are validated on
Windows.

## Regression Gate

Run the local prep gate:

```sh
make qstar-windows-prep-tests
```

The gate checks:

- response-file corpus build through Stella
- argv-vector custom target with spaces, quotes, semicolons, and `$`
- compile database paths remain slash-normalized
- Windows/MSVC dry-run uses `response_style=msvc`
- fake Windows/MSVC build creates real compile and link `.rsp` files
- MSVC response escaping for spaces, quotes, semicolons, and trailing
  backslashes
- Windows/MSVC dry-run renders `/link`, `/LIBPATH:...`, and `kernel32.lib`
- `artifact_name = "windows_app.exe"` is reflected in the planned output path
- drive-letter and backslash package paths are rejected with specific reason text

Manual corpus commands:

```sh
./build/bin/qstar --file tests/corpus/response-files/qstar.lua build //:all
./build/bin/qstar --file tests/corpus/response-files/qstar.lua \
  --profile windows-msvc dry-run //:windows_app
./build/bin/qstar --file tests/corpus/response-files/qstar.lua \
  --profile windows-msvc-fake build //:windows_rsp
```

These commands prepare the port; they do not replace native Windows CI.

## Windows GitHub Actions Candidate

`.github/workflows/windows-validation.yml` is intentionally manual-only through
`workflow_dispatch`. It checks out submodules, uses an MSYS2 UCRT64 environment,
runs `make all`, runs `make qstar-windows-prep-tests`, and performs an install
docs/manpage smoke under `/tmp/qstar-windows-smoke`.

This workflow is a porting candidate, not a release gate. It must become green
and be promoted to regular CI before README or release notes can claim Windows
host support.
