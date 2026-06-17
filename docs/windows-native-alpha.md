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
baseline lane: msys2-ucrt64-gcc
primary compiler: mingw-w64-ucrt-x86_64-gcc
release asset: none
official support: no
```

The lane exists to turn Windows portability work into a real failure list. If it
fails, the failure class should be copied into this document's Known Issues
section before the next Windows round.

Round Q172 fixes the alpha lane identity as `msys2-ucrt64-gcc`. The workflow
now writes step status files and a generated `KNOWN_ISSUES.md` into the
`qstar-windows-native-alpha` artifact so a failed run leaves a structured
failure list instead of only a long console log.

Round Q178 adds a native execution corpus to that same alpha lane. The new
`tests/corpus/windows-execution` project is intentionally separate from the
Windows artifact contract corpus: it uses the MSYS2 UCRT64 GCC lane to build and
run real executable artifacts, while the existing fake MSVC/clang-cl fixtures
continue to cover `.lib`, response escaping, and deferred `.dll` policy.

Round Q179 splits the POSIX Stella/Ninja process runner boundary. `src/executor.c`
and `src/ninja.c` no longer expose `<poll.h>`, POSIX pipe/wait, or Unix process
launch headers to `_WIN32` compilation. Windows builds can compile the runner
boundary as a clear unsupported/fallback path. The same round also adds
small `qstar_platform_mkdir` and `qstar_platform_lstat` helpers so the Windows
bootstrap does not stop at POSIX `mkdir(path, mode)` and `lstat` signatures.

Round Q218 refreshes the manual alpha artifact contract before the
CreateProcess rounds. The workflow still records top-level step logs and
`status/*.status`, but `tests/windows-native-alpha.sh` and
`tests/windows-execution-corpus.sh` now also copy their inner temporary
`.out`/`.err` files and corpus build trees into script-specific detail
directories when they fail. This is intentionally failure-only so successful
runs stay compact while failed runs preserve the exact QStar diagnostics,
generated build files, response files, action logs, replay files, Ninja files,
and corpus outputs needed by the next Windows implementation round.

Round Q219 introduces a shared platform process layer in
`src/platform_process.c`. Stella actions, Stella/Ninja test artifact runners,
and QStar's Ninja launcher now call the same start/wait/terminate/status
contract instead of embedding their own POSIX `fork`/`exec`/`waitpid` logic.
The Windows side of that layer prepares the CreateProcess boundary: argv vector
to Windows command-line quoting, current environment block serialization,
cwd-aware process start parameters, stdout/stderr pipe setup, and timeout/kill
semantics are all represented behind the common contract.

Round Q220 fills the Stella side of that boundary with a real
`CreateProcessA` runner. Windows Stella actions now start through the platform
layer, propagate exit codes, capture stdout/stderr through the same observed
stream path as POSIX, enforce QStar timeouts with `TerminateProcess`, and record
the effective Windows command line in action logs and replay details. QStar's
DSL-facing argv remains shell-free; the MSYS2 alpha lane adds only an
argv-preserving `sh <script>.sh ...` adapter for package-local `.sh` fixtures so
the existing execution corpus can exercise real compile, link, custom, and
`run_target` actions under CreateProcess. This is still manual alpha validation,
not official Windows host support.

Round Q221 extends that execution corpus through the Ninja backend. QStar's
Ninja launcher already uses the shared platform process layer; Q221 makes the
Windows corpus build the same hello/staticlib/response/custom-object/run-target
graph with `-G ninja`, keeps `.ninja_log` and `.ninja_deps` out of the package
root, and verifies Ninja action-log/replay metadata for generated object bridge
actions. Package-local `.sh` fixture commands are adapted in emitted Ninja
commands as `sh <script>.sh ...` on Windows while preserving the original
logged argv vector.

The first Q172 hosted run was
`https://github.com/deeyed/qstar/actions/runs/27508325529`. It reached the
baseline Makefile bootstrap and failed in `src/executor.c` because MSYS2 UCRT64
gcc does not provide POSIX `<poll.h>`. The artifact correctly recorded
`status=fail step=make-all cc=gcc rc=2 log=make-all.log`, so the next Windows
round had a concrete executor portability boundary to fix. Q179 addresses that
boundary locally.

The Q179 hosted run was
`https://github.com/deeyed/qstar/actions/runs/27527243941`. It passed the
Makefile bootstrap, `qstar --version`, and `make qstar-windows-native-alpha-tests
CC=gcc`. It then failed at `make qstar-windows-execution-corpus-tests CC=gcc`,
which became the expected next alpha boundary before Q220. Q218 addresses the
artifact visibility gap for that failure class: a future execution-corpus
failure should include
`windows-execution-detail/` with the inner temp outputs and the
`tests/corpus/windows-execution/build/qstar` tree.

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

Do not add a second Windows lane until the baseline lane has a stable failure
history. `clang-cl`, MSVC, Visual Studio project generation, and `nmake` remain
separate future lanes rather than hidden requirements of this alpha gate.

## Workflow Shape

`.github/workflows/windows-validation.yml` is manual-only:

```txt
workflow_dispatch
  inputs:
    run_ninja_parity: false by default
  job:
    windows alpha / msys2-ucrt64-gcc baseline
```

The default alpha job runs:

```sh
make all CC=gcc
build/bin/qstar --version
make qstar-windows-native-alpha-tests CC=gcc
make qstar-windows-execution-corpus-tests CC=gcc
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

Round Q179 adds the same kind of boundary to process execution. `_WIN32` builds
compile `src/executor.c` and `src/ninja.c` without POSIX `<poll.h>`, `fork`,
`waitpid`, or Unix socket assumptions. Q179 also moves directory creation and
symlink-aware stat calls behind `qstar_platform_mkdir` and
`qstar_platform_lstat` to keep the baseline Makefile bootstrap moving across the
next Windows C library differences.

Round Q219 moves the process boundary itself into `src/platform_process.c`.
`src/executor.c` and `src/ninja.c` still own QStar-specific scheduling, replay,
action-log, and test-result formatting, but platform process start, output
pipe setup, wait, terminate, and exit-status normalization are no longer
duplicated there. The local `_WIN32` compile smoke now compiles
`src/platform_process.c` directly alongside `src/executor.c` and `src/ninja.c`.

Round Q220 changes that platform layer from a compile-only Windows boundary into
the Stella CreateProcess runner. The execution corpus now includes
`//:hello_smoke`, a `run_target` with `expect.contains`, so the Windows lane
checks not only compile/link actions but also captured program output and expect
matching through the Stella runner.

Round Q221 makes the same execution corpus run with `-G ninja` whenever Ninja is
available. That keeps the alpha lane from becoming Stella-only: the hosted
Windows workflow now checks QStar-launched Ninja builds, Ninja-generated shell
script paths, response files, generated object bridge actions, install output,
and root `.ninja_*` pollution guards in the same corpus.

The optional `run_ninja_parity=true` input also runs the broader cross-corpus
Ninja backend parity gate:

```sh
make qstar-ninja-backend-parity-tests
```

All alpha logs are uploaded as the `qstar-windows-native-alpha` artifact. This
is required because early native Windows failures are expected to be
environment-sensitive.

Round Q172 makes the artifact layout explicit:

```txt
environment.txt
make-all.log
version.log
native-alpha.log
windows-prep.log
install.log
native-alpha-detail/failure.status
native-alpha-detail/tmp/*.out
native-alpha-detail/tmp/*.err
native-alpha-detail/corpus/build/...
windows-execution-detail/failure.status
windows-execution-detail/tmp/*.out
windows-execution-detail/tmp/*.err
windows-execution-detail/corpus/build/qstar/...
windows-execution-detail/corpus/stage/...
SUMMARY.md
KNOWN_ISSUES.md
windows-alpha-status.txt
status/environment.status
status/make-all.status
status/version.status
status/native-alpha.status
status/windows-execution.status
status/windows-prep.status
status/ninja-backend-parity.status
status/install.status
```

If a step fails, its status file is written before the step exits non-zero. The
final `SUMMARY.md` and `KNOWN_ISSUES.md` steps run with `if: always()` so the
artifact should still contain a machine-readable failure class. Missing status
files mean the corresponding step was not reached, usually because an earlier
baseline step failed. The `native-alpha-detail/` and
`windows-execution-detail/` directories are created only when the corresponding
script fails. The `tmp/` subdirectory preserves the script's captured
`.out`/`.err` files. The `corpus/` subdirectory preserves generated build
artifacts such as response files, `compile_commands.json`, `ninja/build.ninja`,
`logs/last-failure.replay`, action logs, generated object bridge outputs, and
any stage tree that existed before the failure.

## Local Contract Smoke

Non-Windows hosts can run the contract-only subset:

```sh
make qstar-windows-native-alpha-tests
make qstar-windows-prep-tests
```

On non-Windows hosts, `tests/windows-native-alpha.sh` reports
`mode=contract-only`. On MSYS2/Cygwin/MinGW-like hosts it reports
`mode=native-windows-alpha`.

The smoke also prints:

```txt
qstar-windows-native-alpha: baseline=msys2-ucrt64-gcc
qstar-windows-native-alpha: daemon_named_pipe=deferred
```

That output is intentional. Windows native validation is currently anchored to
the MSYS2 UCRT64 gcc lane, and daemon named pipe work is not part of this alpha
round.

## Native Execution Corpus

`tests/windows-execution-corpus.sh` drives
`tests/corpus/windows-execution/qstar.lua`. It is the first Windows lane that is
meant to prove actual build/run behavior, not only graph lowering contracts.

The corpus covers:

- `//:hello`: C executable build and run
- `//:core` + `//:app`: static library plus executable link and run
- `//:response_probe`: forced response-file compile/link with a real executable
  run
- `//:bridge_object` + `//:bridge_app`: generated object artifact bridge dry-run,
  build, and run
- `qstar install //:app` and `qstar install //:core` into a temporary prefix

The status file is:

```txt
status/windows-execution.status
```

and the raw log is:

```txt
windows-execution.log
```

When the corpus fails under the hosted Windows workflow, the detail bundle is:

```txt
windows-execution-detail/
  failure.status
  tmp/
    check.out
    check.err
    bridge-dry.out
    bridge-dry.err
    ...
  corpus/
    build/qstar/
      compile_commands.json
      generated/
      logs/
      ninja/
      out/
      rsp/
    stage/
```

If a later hosted Windows alpha run fails, start from this detail bundle rather
than from the full Actions console log.

The corpus deliberately uses `.exe` executable artifact names and a GCC-friendly
static archive name, `libwinexec_core.a`. Windows `.lib`, runtime `.dll`, import
library, and PDB/debug behavior remain in the artifact policy track until the
Windows shared-library rounds take ownership of them.

## Known Issues

Current known gaps:

- Q159's first observed `src/daemon.c` `<sys/socket.h>` failure is addressed by
  Q164's Windows daemon stub. The manual alpha lane still needs to be rerun to
  discover the next real native failure class.
- Q172's hosted `msys2-ucrt64-gcc` run failed at `src/executor.c` because the
  Stella process/event runner included POSIX `<poll.h>`. Q179 splits that
  process runner boundary and the hosted Q179 run passed `make all`,
  `qstar --version`, and the native alpha smoke.
- Q220 implements the Stella CreateProcess runner in `src/platform_process.c`.
  The hosted Q220 lane reached real compile/link/custom/run execution.
- Q221 extends the Windows execution corpus to `-G ninja`, so QStar-launched
  Ninja builds now share the same native Windows alpha evidence as Stella. The
  optional `run_ninja_parity=true` lane remains the broader cross-corpus Ninja
  backend gate.
- Q222 strengthens the same execution lane from build/run evidence to
  install/stage layout evidence. The corpus now checks `.exe` installation under
  `bin/`, static archive installation under `lib/`, generated object bridge
  staging, and slash-normalized install/stage manifests.
- Windows filesystem helpers are now split enough for local `_WIN32` object
  compile checks and the Q179 hosted run reached past the previous
  `mkdir`/`lstat` compile failures.
- Q218 adds failure-only detail directories for `tests/windows-native-alpha.sh`
  and `tests/windows-execution-corpus.sh`. If a future Windows alpha failure does
  not include `native-alpha-detail/` or `windows-execution-detail/` for the
  failing script, treat that as an artifact-contract regression before deeper
  Windows execution work.
- Q172 has not promoted Windows to official support. It only makes
  `msys2-ucrt64-gcc` the baseline lane and ensures failed runs leave structured
  status and known-issue artifacts.
- Q178 adds real build/run/install corpus coverage, Q220 adds Stella
  CreateProcess execution for it, Q221 adds Ninja execution coverage, and Q222
  adds install/stage layout validation for the same corpus. The lane is still
  manual alpha validation: a green execution corpus does not by itself create a
  Windows public release asset or official support claim.
- Stella daemon on Windows is disabled/deferred. The future supported transport
  is a named pipe with Windows ACL rules, not Unix sockets.
- No Windows public release asset.
- The `.exe`/static archive/object bridge install-stage subset is alpha
  validated. Runtime `.dll`, import `.lib`, PDB/debug, and full Windows
  packaging layout are still deferred.
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

Use the generated artifact in this order:

1. Read `windows-alpha-status.txt` for the first `status=fail` line.
2. Open the corresponding `*.log` file named in that status line.
3. Copy the failure class, not the entire raw log, into this Known Issues list.
4. Keep daemon named pipe, MSVC bootstrap, public Windows asset, `.dll`/import
   `.lib`, and PDB/debug policy deferred unless a later round explicitly takes
   ownership of them.

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
