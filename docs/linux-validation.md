# Linux Validation Path

QStar v0.4 beta는 macOS arm64 binary를 먼저 배포했지만, Round Q97부터 Linux host
지원은 `planned`가 아니라 `validation underway` 상태로 관리한다. 이 문서는 Linux를
release artifact 후보로 올리기 전에 필요한 source build, path/process, depfile,
install layout smoke를 고정한다.
Round Q109부터 `.github/workflows/linux-validation.yml`이 이 gate를 Ubuntu CI에서
gcc/clang matrix로 실행한다.

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
  make install PREFIX=/tmp/qstar-linux-smoke
  /tmp/qstar-linux-smoke/bin/qstar --version

ubuntu-latest / clang:
  make all
  make check
  make qstar-linux-validation-tests
  make install PREFIX=/tmp/qstar-linux-smoke
  /tmp/qstar-linux-smoke/bin/qstar --version
```

Each lane sets `QSTAR_LINUX_VALIDATION_CC` to the matrix compiler, verifies
`ninja --version`, runs the Ninja backend parity tests through `make check`, and
performs an explicit root pollution smoke: `.ninja_deps` and `.ninja_log` must
stay under the QStar build directory, not the repository root.

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
Before a `linux-*` release asset is added, all of the following must be true:

- source build passes on a clean Linux host or CI image
- `make check` passes on Linux
- `make qstar-linux-validation-tests` passes on Linux
- `.github/workflows/linux-validation.yml` is green for both gcc and clang lanes
- `make install PREFIX=/tmp/qstar-linux-smoke` installs binary, docs, and
  manpages under that prefix
- installed binary reports the tagged version
- clang and gcc depfile behavior is either both green or documented with a
  stable skip reason
- Ninja backend parity passes without root `.ninja_*` files
- release notes identify architecture and libc assumptions

Until then, README platform status remains conservative: Linux is validation
underway, Windows is still planned.
