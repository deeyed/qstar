# Windows Path And Process Prep

Round Q98 prepares the Windows port without declaring Windows official host
support. QStar can already model Windows-like toolchains in dry-run and command
planning, but source build, install layout, shell-free process execution, and
release packaging still need native Windows validation before a Windows release
artifact exists.

## Status

```txt
host support: planned validation
official release artifact: none
purpose of this document: pre-port contract
gate: make -C qstar qstar-windows-prep-tests
```

Linux is validation underway. Windows remains planned until QStar has a native
Windows CI lane, source build, install smoke, response-file execution, and
artifact packaging story.

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

Round Q98 adds a regression check that rejects drive-letter and backslash paths
in package path fields.

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

## Artifact Naming

Windows artifact naming is not official yet.

Current policy:

- Executable targets may use `artifact_name = "tool.exe"` or profile
  `artifact_names = {"//:tool=tool.exe"}`.
- External Windows libraries in `libs = {"kernel32"}` render as
  `kernel32.lib` for MSVC-like targets.
- `lib_dirs` render as `/LIBPATH:<path>` for MSVC-like targets.
- `qstar.staticlib` still uses the current static archive policy; `.lib`
  archive output is not yet a sealed Windows artifact contract.
- `qstar.sharedlib` remains plan/check-only. `.dll`, import library, PDB,
  runtime search path, and install layout are deferred.

Do not claim Windows packaging support until `.exe`, `.lib`, `.dll`, import
library, debug artifact, and install layout behavior are validated on Windows.

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
- Windows/MSVC dry-run renders `/link`, `/LIBPATH:...`, and `kernel32.lib`
- `artifact_name = "windows_app.exe"` is reflected in the planned output path
- drive-letter and backslash package paths are rejected

Manual corpus commands:

```sh
./build/bin/qstar --file tests/corpus/response-files/qstar.lua build //:all
./build/bin/qstar --file tests/corpus/response-files/qstar.lua \
  --profile windows-msvc dry-run //:windows_app
```

These commands prepare the port; they do not replace native Windows CI.
