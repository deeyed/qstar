# Linux Validation Path

QStar v0.4 beta는 macOS arm64 binary를 먼저 배포했지만, Round Q97부터 Linux host
지원은 `planned`가 아니라 `validation underway` 상태로 관리했다. Round Q157부터 Linux
x86_64는 public beta release artifact 대상이다. 이 문서는 Linux asset을 publish하기 전에
필요한 source build, path/process, depfile, install layout smoke, release tarball dry-run,
GitHub release upload 조건을 고정한다.
Round Q109부터 `.github/workflows/linux-validation.yml`이 이 gate를 Ubuntu CI에서
gcc/clang matrix로 실행한다. Round Q113부터 gcc lane은
`QSTAR_RELEASE_PLATFORM=linux-x86_64` packaging dry-run까지 수행한다.
Round Q142부터 같은 Ubuntu gcc/clang lane은 Stella/Ninja medium performance line
protocol도 수집하고 CI artifact로 업로드한다. Large synthetic corpus는 시간이 더 오래
걸리므로 우선 `workflow_dispatch` 전용 report-only lane으로 분리한다.
Round Q156부터 release-candidate tarball을 다시 extract해서 실행, docs/wiki/manpage
lookup, `file(1)`, `ldd(1)` sanity를 반복 검증하고, `workflow_dispatch`의
`daemon_socket_smoke` input으로 Linux daemon socket smoke를 opt-in lane에서 수행한다.
Round Q157부터 `publish_linux_asset=true` workflow_dispatch lane은 clean Ubuntu host에서
tag를 checkout하고 Linux x86_64 tarball을 GitHub release에 업로드한다.
Round Q163부터 publish lane은 업로드 직후 같은 release asset을 다시 다운로드해
`SHA256SUMS`, extracted prefix, docs/wiki lookup, manpage source/render reports,
`file(1)`, `ldd(1)`를 반복 검증한다.
Round Q167부터 daemon opt-in lane은 medium performance뿐 아니라 Linux-only daemon
validation script를 먼저 실행해 Unix socket, `inotify` watcher trace, memory-state trace,
skip/fail reason artifact를 남긴다.
Round Q171부터 hosted release verification은 matrix install smoke reports,
downloaded-release tarball contents, download-smoke stdout, and a
`linux-hosted-release-decision.txt` artifact를 남긴다. 이 파일이 `status=published`와
`download_smoke=ok`를 기록하기 전에는 Linux asset을 release-backed로 보지 않는다.

## Scope

Linux validation은 다음을 확인한다.

- package-relative path가 `/` separator를 기준으로 안정적으로 유지되는지
- `qstar.import_module`, `qstar.join`, `generated_dir`가 Linux filesystem layout에서
  같은 결과를 내는지
- external tool override가 package root `cwd`와 environment를 보존하는지
- Linux `cc`, clang, gcc가 `-MMD -MF` depfile을 생성하고 incremental rebuild reason에
  반영되는지
- `make install PREFIX=...`가 `bin/qstar`, installed wiki, `qstar(1)`,
  `qstar-lua(5)` manpage를 같은 prefix 아래에 배치하는지
- `QSTAR_DOC_DIR=<prefix>/share/doc/qstar qstar docs --path`가 installed docs를
  가리키는지
- Linux release candidate tarball이 ELF x86-64 binary, `ldd` sanity, installed
  docs/wiki/manpages, `SHA256SUMS`, prefix layout을 만족하는지
- release-candidate tarball을 임시 prefix에 다시 extract했을 때 `qstar --version`,
  `qstar docs --path`, `qstar docs --show`, manpage file, `file(1)`, `ldd(1)`가
  다시 통과하는지
- GitHub release에 업로드된 Linux asset을 다시 다운로드했을 때 checksum,
  extracted `qstar --version`, docs/wiki home, Lua reference, manpage source,
  `file(1)`, `ldd(1)` smoke가 다시 통과하는지
- Stella와 Ninja medium corpus timing이 `medium_project_gate ...` line protocol로
  수집되는지
- Linux에서 Stella process runner trace가 `runner=posix_spawn`과 `event_wait=poll`을
  보고하는지
- medium perf raw output, text summary, markdown summary가 CI artifact로 보존되는지
- Linux daemon socket smoke는 기본 push/PR gate가 아니라 `workflow_dispatch`의
  `daemon_socket_smoke` input으로 켜는 opt-in lane에서 수행되는지
- Linux daemon opt-in lane이 `daemon_watcher status=active backend=inotify`와
  `daemon_watcher status=event backend=inotify` trace를 artifact로 보존하는지

macOS local run에서는 Linux kernel, glibc, distro package layout, Linux `cc` 구현을
직접 검증할 수 없다. 따라서 macOS에서는 portable path/process smoke만 수행하고,
Linux host 또는 CI에서 같은 script를 full validation으로 실행한다.
Script는 `QSTAR_LINUX_VALIDATION_CC`를 QStar profile의 C compiler로 넣어 depfile
생성을 확인한다. Makefile target은 기본값으로 `$(CC)`를 전달하므로 `CC=clang make ...`
또는 explicit `QSTAR_LINUX_VALIDATION_CC=clang make ...` 둘 다 사용할 수 있다.

## Local Smoke

QStar tree에서 다음을 실행한다.

```sh
make all
make check
make qstar-linux-validation-tests
QSTAR_TEST_QSTAR=build/bin/qstar sh tests/medium-project-performance.sh
git diff --check
```

`tests/linux-validation.sh`는 임시 project를 만들고 다음 경로를 한 번에 확인한다.

- `.qsm` module import
- `qstar.project.generated_dir = "build/qstar/generated"`
- custom generated header
- executable build
- compiler depfile 존재
- header 변경 후 `reason=depfile-changed`
- prefix install layout
- installed docs/manpage lookup

macOS에서 실행하면 output에 다음처럼 제한 모드가 표시된다.

```txt
qstar-linux-validation: host=Darwin mode=limited-linux-path
```

Linux에서 실행하면 full validation으로 표시된다.

```txt
qstar-linux-validation: host=linux mode=full
```

## Linux Host Gate

Fresh Linux host 또는 CI에서는 최소 다음 명령이 통과해야 한다.

```sh
make all
make check
make qstar-linux-validation-tests
make install PREFIX=/tmp/qstar-linux-smoke
/tmp/qstar-linux-smoke/bin/qstar --version
QSTAR_DOC_DIR=/tmp/qstar-linux-smoke/share/doc/qstar \
  /tmp/qstar-linux-smoke/bin/qstar docs --path
test -f /tmp/qstar-linux-smoke/share/man/man1/qstar.1
test -f /tmp/qstar-linux-smoke/share/man/man5/qstar-lua.5
```

Compiler-specific depfile checks should run with both common Linux compilers
when they are available:

```sh
QSTAR_LINUX_VALIDATION_CC=clang make qstar-linux-validation-tests
QSTAR_LINUX_VALIDATION_CC=gcc make qstar-linux-validation-tests
CC=clang make qstar-linux-validation-tests
CC=gcc make qstar-linux-validation-tests
```

If one compiler is not installed, CI may report that lane as skipped, but a
Linux release artifact needs at least one green distro image with clang and one
green distro image with gcc before promotion.

## GitHub Actions CI

The Linux validation workflow lives at `.github/workflows/linux-validation.yml`.
It uses `ubuntu-latest` and installs only the small tooling QStar actually
needs:

- C compiler: distro `cc`, plus explicit clang/gcc lanes when available
- `make`
- `ar`
- `ninja` for backend parity tests
- `mandoc` or `man-db` only if the CI verifies rendered manpages

Current job shape:

```txt
ubuntu-latest / gcc:
  make all
  make check
  make qstar-linux-validation-tests
  make qstar-ninja-backend-parity-tests
  tests/medium-project-performance.sh
  upload dist/perf/linux-gcc-medium-*.txt/md
  make install PREFIX=/tmp/qstar-linux-smoke
  /tmp/qstar-linux-smoke/bin/qstar --version
  QSTAR_RELEASE_PLATFORM=linux-x86_64 tools/package-public-beta.sh
  extract qstar-v<version>-linux-x86_64.tar.gz and run qstar/docs/man/file/ldd smoke
  upload dist/release/qstar-v*-linux-x86_64.tar.gz and sanity reports

ubuntu-latest / clang:
  make all
  make check
  make qstar-linux-validation-tests
  make qstar-ninja-backend-parity-tests
  tests/medium-project-performance.sh
  upload dist/perf/linux-clang-medium-*.txt/md
  make install PREFIX=/tmp/qstar-linux-smoke
  /tmp/qstar-linux-smoke/bin/qstar --version
```

Each lane sets `QSTAR_LINUX_VALIDATION_CC` to the matrix compiler, verifies
`ninja --version`, checks out `vendor/lua` with `submodules: recursive`, runs the
Ninja backend parity tests through `make check`, and performs an explicit root
pollution smoke: `.ninja_deps` and `.ninja_log` must stay under the QStar build
directory, not the repository root.

Round Q156 also runs `make qstar-ninja-backend-parity-tests` as an explicit
named step so Ninja backend regressions are visible even though `make check`
already covers the same corpus.

Round Q142 adds a medium performance collection step to both compiler lanes. It
does not make timing a hard CI threshold yet. The hard checks are that Stella and
Ninja both run, the line protocol includes Ninja clean timing, and the Linux
Stella trace reports the POSIX fast path:

```txt
medium_project_gate scheduler runner=posix_spawn event_wait=poll
medium_project_gate backend=ninja phase=clean elapsed_ms=...
```

The uploaded medium artifacts are:

```txt
dist/perf/linux-<compiler>-medium-perf.txt
dist/perf/linux-<compiler>-medium-summary.txt
dist/perf/linux-<compiler>-medium-summary.md
```

Round Q171 requires the text summary to include `perf_summary status=ok` and
also preserves install-prefix smoke evidence for each compiler lane:

```txt
dist/release/linux-<compiler>-install-smoke/version.txt
dist/release/linux-<compiler>-install-smoke/docs-path.txt
dist/release/linux-<compiler>-install-smoke/docs-ai-index.txt
dist/release/linux-<compiler>-install-smoke/docs-show-qstar-lua.txt
dist/release/linux-<compiler>-install-smoke/man-qstar.1.txt
dist/release/linux-<compiler>-install-smoke/man-qstar-lua.5.txt
dist/release/linux-<compiler>-install-smoke/file.txt
dist/release/linux-<compiler>-install-smoke/ldd.txt
```

The workflow also has a manual large performance job, intended for nightly or
on-demand report-only runs:

```txt
workflow_dispatch / large-performance-report:
  tests/large-project-performance.sh
  upload dist/perf/linux-gcc-large-*.txt/md
```

Round Q156 adds a second manual job for daemon socket validation. Round Q167
strengthens it into a Linux daemon validation lane. It is not a regular push/PR
gate because socket behavior can vary across runners and sandboxed CI, but
maintainers should run it before promoting Linux daemon support from
validation-backed to release-backed:

```txt
workflow_dispatch / daemon_socket_smoke=true:
  make qstar-linux-daemon-validation-tests
  require linux_daemon_validation status=ok watcher_backend=inotify
  require daemon_watcher status=active backend=inotify
  require daemon_watcher status=event backend=inotify
  tests/medium-project-performance.sh
  require backend=stella-daemon clean/noop/incremental lines
  fail if socket-bind-not-permitted appears
  upload dist/perf/linux-daemon-validation-*.txt and linux-daemon-medium-*.txt/md

workflow_dispatch / publish_linux_asset=true:
  checkout selected release tag
  rerun Linux release gates
  publish qstar-v<version>-linux-x86_64.tar.gz
  download the uploaded asset again
  verify SHA256SUMS, qstar --version, docs/wiki, manpage, file, ldd smoke
  upload dist/release/download-smoke-linux-x86_64/download/*.txt
  upload dist/release/download-smoke-linux-x86_64/download-smoke.log
  upload dist/release/linux-hosted-release-decision.txt
```

The gcc lane also performs a release-candidate packaging dry-run. On regular
push/PR runs it does not publish the artifact:

```sh
QSTAR_RELEASE_PLATFORM=linux-x86_64 tools/package-public-beta.sh
test -f dist/release/qstar-v<version>-linux-x86_64.tar.gz
test -s dist/release/file-linux-x86_64.txt
test -s dist/release/ldd-linux-x86_64.txt
test -s dist/release/extract-file-linux-x86_64.txt
test -s dist/release/extract-ldd-linux-x86_64.txt
test -s dist/release/extract-docs-show-qstar-lua.txt
tar -tzf dist/release/qstar-v<version>-linux-x86_64.tar.gz
```

`tools/package-public-beta.sh` refuses to build `linux-x86_64` packages on a
non-Linux host, verifies `file(1)` reports an ELF x86-64 binary, records
`ldd(1)` output, checks `qstar docs --path`, `qstar docs --ai-index`, and
`qstar docs --show reference/qstar-lua.md` against the installed doc tree, and
requires the tarball to contain the installed wiki home, AI index, Lua reference,
and manpages.

The same script then extracts the tarball into
`dist/release/qstar-v<version>-linux-x86_64-extract-smoke` and repeats
`qstar --version`, docs lookup, manpage presence, `file(1)`, and `ldd(1)` checks
against the extracted tree. The workflow uploads the dry-run tarball and sanity
reports as `qstar-linux-x86_64-release-candidate-dry-run`.

The publish job then runs the same release from the GitHub download URL using
`make qstar-public-beta-download-smoke`. The smoke output is preserved under
`dist/release/download-smoke-linux-x86_64/download/` and includes:

```txt
SHA256SUMS
contents.txt
file-linux-x86_64.txt
ldd-linux-x86_64.txt
docs-show-home.txt
docs-show-qstar-lua.txt
man-qstar.1.txt
man-qstar-lua.5.txt
```

Q171 also preserves:

```txt
dist/release/download-smoke-linux-x86_64/download-smoke.log
dist/release/linux-hosted-release-decision.txt
```

The decision file must contain:

```txt
linux_release_asset status=published
download_smoke=ok
```

The install prefix smoke checks:

```sh
make install PREFIX="$RUNNER_TEMP/qstar-linux-smoke-<compiler>"
"$prefix/bin/qstar" --version
QSTAR_DOC_DIR="$prefix/share/doc/qstar" "$prefix/bin/qstar" docs --path
test -f "$prefix/share/doc/qstar/wiki/AI_INDEX.md"
test -f "$prefix/share/man/man1/qstar.1"
test -f "$prefix/share/man/man5/qstar-lua.5"
```

## Release Asset Conditions

A Linux tarball must not be published merely because the source compiles once.
Before a `linux-*` release asset is added or refreshed, all of the following
must be true:

- source build passes on a clean Linux host or CI image
- `make check` passes on Linux
- `make qstar-linux-validation-tests` passes on Linux
- `.github/workflows/linux-validation.yml` is green for both gcc and clang lanes
- medium Stella/Ninja performance line protocol is uploaded for both gcc and
  clang lanes
- medium summary artifacts contain `perf_summary status=ok`
- install smoke artifacts are uploaded for both gcc and clang lanes
- Linux Stella trace reports `runner=posix_spawn` and `event_wait=poll`
- `QSTAR_RELEASE_PLATFORM=linux-x86_64 tools/package-public-beta.sh` passes on a
  Linux host or Ubuntu CI packaging lane
- the release-candidate tarball is extracted again and the extracted tree passes
  `qstar --version`, `qstar docs --path`, `qstar docs --show`, manpage file,
  `file(1)`, and `ldd(1)` smoke
- the dry-run tarball and reports are uploaded as
  `qstar-linux-x86_64-release-candidate-dry-run`
- uploaded GitHub release asset passes `make qstar-public-beta-download-smoke`
  on a Linux host and preserves `download-smoke-linux-x86_64` reports
- `linux-hosted-release-decision.txt` records `status=published` and
  `download_smoke=ok`
- `make install PREFIX=/tmp/qstar-linux-smoke` installs binary, docs, and
  manpages under that prefix
- installed binary reports the tagged version
- `file(1)` reports ELF x86-64 and `ldd(1)` output is recorded, or a static
  binary exception is explicitly documented
- clang and gcc depfile behavior is either both green or documented with a
  stable skip reason
- Ninja backend parity passes without root `.ninja_*` files
- optional `daemon_socket_smoke` is green before Linux daemon behavior is
  described as release-backed rather than validation-backed
- daemon opt-in artifact includes `linux-daemon-validation-status.txt`,
  `linux-daemon-validation-reason.txt`, server logs, schedule traces, and
  medium performance summaries
- release notes identify architecture and libc assumptions

Release publication uses the manual workflow path:

```txt
workflow: Linux Validation
release_tag: v0.7.0-beta
publish_linux_asset: true
```

The publish job checks out the selected tag, reruns the Linux release gates,
invokes `tools/publish-github-release-asset.sh`, uploads
`qstar-v<version>-linux-x86_64.tar.gz`, and merges the Linux checksum into the
release `SHA256SUMS`. Windows is still planned.
