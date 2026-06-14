# Windows Native CI Alpha

Round Q159 moves Windows from a documentation-only preparation path to a manual
native CI alpha. This is still not official Windows host support and does not
produce a public Windows release asset.

## Support Level

```txt
status: manual native CI alpha
workflow: .github/workflows/windows-validation.yml
trigger: workflow_dispatch
host: windows-latest
bootstrap shell: MSYS2 UCRT64
primary compiler: mingw-w64-ucrt-x86_64-gcc
release asset: none
official support: no
```

The lane exists to turn Windows portability work into a real failure list. If it
fails, the failure class should be copied into this document's Known Issues
section before the next Windows round.

## Toolchain Choice

The alpha lane uses MSYS2 UCRT64 first because QStar's current bootstrap build is
a POSIX Makefile over C99 sources plus vendored Lua. MSYS2 gives the smallest
first native Windows surface:

- `make all`
- `build/bin/qstar --version`
- QStar-authored Windows path/process smoke
- install docs/man smoke under `/tmp/qstar-windows-smoke`

This is intentionally not a Visual Studio solution, `nmake`, or MSVC release
lane yet. MSVC/clang-cl behavior is covered at the QStar graph/response-file
contract level through the response-file corpus and fake `clang-cl` fixture.
Real MSVC process execution remains a later validation step.

## Workflow Shape

`.github/workflows/windows-validation.yml` is manual-only:

```txt
workflow_dispatch
  inputs:
    run_ninja_parity: false by default
```

The default alpha job runs:

```sh
make all CC=gcc
build/bin/qstar --version
make qstar-windows-native-alpha-tests CC=gcc
make qstar-windows-prep-tests CC=gcc
make install CC=gcc PREFIX=/tmp/qstar-windows-smoke
```

Round Q164 isolates the Unix socket Stella daemon implementation from Windows
builds. `src/daemon.c` now provides a Windows stub that compiles without
`<sys/socket.h>` and returns a clear deferred diagnostic for `qstar daemon` or
`--use-daemon=always`. `--use-daemon=auto` can still fall back to the normal
Stella build path. This should let `make all CC=gcc` progress past the first
observed daemon source failure; any new native failure class belongs in the
Known Issues section below.

The optional `run_ninja_parity=true` input also runs:

```sh
make qstar-ninja-backend-parity-tests
```

All alpha logs are uploaded as the `qstar-windows-native-alpha` artifact. This
is required because early native Windows failures are expected to be
environment-sensitive.

## Local Contract Smoke

Non-Windows hosts can run the contract-only subset:

```sh
make qstar-windows-native-alpha-tests
make qstar-windows-prep-tests
```

On non-Windows hosts, `tests/windows-native-alpha.sh` reports
`mode=contract-only`. On MSYS2/Cygwin/MinGW-like hosts it reports
`mode=native-windows-alpha`.

## Known Issues

Current known gaps:

- Q159's first observed `src/daemon.c` `<sys/socket.h>` failure is addressed by
  Q164's Windows daemon stub. The manual alpha lane still needs to be rerun to
  discover the next real native failure class.
- Stella daemon on Windows is disabled/deferred. The future supported transport
  is a named pipe with Windows ACL rules, not Unix sockets.
- No Windows public release asset.
- No stable Windows install layout contract.
- Windows artifact policy is currently a pre-support contract in
  `docs/windows-artifact-policy.md`; native validation still has to prove real
  `.exe`, explicit static `.lib`, runtime `.dll`, import `.lib`, and PDB/debug
  behavior.
- No Visual Studio, `nmake`, or direct MSVC bootstrap lane.
- The MSYS2 alpha lane pins `CC=gcc`; the hosted runner may provide a `CC=c99`
  environment value that is not an executable tool.
- Real MSVC/clang-cl compiler execution is not yet a release gate.
- Windows `.dll`, import `.lib`, PDB/debug, and shared library install policy
  are deferred.
- Persistent Stella daemon uses Unix socket paths today; Windows named pipe
  support is deferred.
- QStar DSL package paths still intentionally reject drive letters and
  backslashes. Windows-like paths are allowed only as compiler/linker argv
  options, escaped through `response_style = "msvc"`.

When the manual workflow fails, append new failure classes here instead of
loosening the path/process contract silently.

## Promotion Criteria

Windows can move from alpha to validation-backed beta only after:

- the manual alpha workflow is repeatedly green;
- a regular Windows CI lane is enabled for push/PR or scheduled validation;
- native Windows source build and install smoke pass without MSYS2-only
  assumptions leaking into QStar authoring;
- process spawn, response files, path normalization, and install layout are
  tested on Windows with real tools;
- release packaging rules for `.exe`, `.lib`, `.dll`, import library, PDB, docs,
  and manpage-equivalent artifacts are decided.
