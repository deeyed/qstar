# Installation

QStar는 C/C++/Cale을 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다. Public
beta에서는 macOS arm64 tarball을 먼저 배포한다. Linux host 지원은 Ubuntu gcc/clang CI
기반 source build 검증, `linux-x86_64` release-candidate tarball dry-run, Stella/Ninja
medium performance artifact collection을 갖췄지만 아직 public release artifact는 없다.
Windows host 지원은 native validation candidate 준비 단계다. Windows는 아직 공식 지원이
아니지만 path/process/response-file 준비 규칙과 manual Windows workflow 후보를 QStar tree
안에서 검증한다. 모든 platform에서 소스에서 직접 빌드할 수 있도록 검증 경로를 늘려간다.

## 최소 예제

```sh
tar -xzf qstar-v0.6.0-beta-macos-arm64.tar.gz -C "$HOME/.local"
export PATH="$HOME/.local/bin:$PATH"
qstar --version
```

## 전체 예제

```sh
git clone --recurse-submodules https://github.com/deeyed/qstar.git
cd qstar
make check
make qstar-self-host-tests
make qstar-public-beta-release-tests
make qstar-linux-validation-tests
make qstar-windows-prep-tests
make install PREFIX="$HOME/.local"
qstar init c-app /tmp/qstar-install-smoke
qstar --file /tmp/qstar-install-smoke/qstar.lua build //:app
```

이미 repository를 clone한 뒤 `vendor/lua/lua.h`가 없다면 다음을 먼저 실행한다.

```sh
git submodule update --init --recursive
```

VSCode extension은 `qstar/editors/vscode/qstar` 아래에 있고, `.vsix`는 commit하지 않는
release artifact다.

## Public beta package 검증

Public beta tarball은 Makefile-built binary를 기준으로 만든다.

```sh
make qstar-public-beta-release-tests
tar -tzf dist/release/qstar-v0.6.0-beta-macos-arm64.tar.gz
cat dist/release/SHA256SUMS
```

이 gate는 installed binary version, macOS codesign, installed wiki, manpages,
`qstar docs --path`, `qstar docs --ai-index`, `qstar docs --show`, prefix-style
tarball layout, `SHA256SUMS`, VSCode `.vsix` 미포함 정책을 확인한다.
GitHub Wiki mirror는 source tree의 `wiki/`가 최신인 것을 확인한 뒤
`tools/sync-github-wiki.sh`로 수행한다.

## 실패 예제

```sh
qstar --file missing/qstar.lua list-targets
```

`qstar.lua`가 없으면 package root를 결정할 수 없다.

## 관련 CLI

```sh
make all
make check
make qstar-self-host-tests
make qstar-public-beta-release-tests
make qstar-linux-validation-tests
make qstar-windows-prep-tests
qstar doctor
```

## Linux 검증 경로

Linux release asset은 아직 배포하지 않는다. Linux host 또는 CI에서는 다음 source build
smoke가 통과해야 validation-backed source build 상태를 유지한다.

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

GitHub Actions 후보는 `.github/workflows/linux-validation.yml`에 있다. 이 workflow는
`ubuntu-latest`에서 gcc/clang matrix를 돌리고, 각 lane에서 Ninja를 설치한 뒤
`make all`, `make check`, `make qstar-linux-validation-tests`, install docs/man smoke를
수행한다. Depfile compiler lane은 `QSTAR_LINUX_VALIDATION_CC=gcc|clang`으로 고정한다.
gcc lane은 추가로 다음 release-candidate package dry-run을 수행한다.

```sh
QSTAR_RELEASE_PLATFORM=linux-x86_64 tools/package-public-beta.sh
test -f dist/release/qstar-v0.6.0-beta-linux-x86_64.tar.gz
test -s dist/release/file-linux-x86_64.txt
test -s dist/release/ldd-linux-x86_64.txt
```

이 dry-run은 Linux binary가 ELF x86-64인지, `ldd` sanity가 가능한지, installed wiki와
manpage가 tarball에 들어가는지, `SHA256SUMS`가 생성되는지를 확인한다. GitHub release에
Linux asset을 실제로 붙이는 결정은 별도 release 라운드에서 한다.

## Windows 준비 경로

Windows release asset은 아직 없다. 현재 QStar는 Windows 이식 전에 다음 규칙을 먼저
고정한다.

- QStar DSL path는 Windows에서도 `/` 기반 package-relative path다.
- `src\\main.c`, `C:\\SDK\\include` 같은 path는 source/include/output/stage field에
  쓰지 않는다.
- process 실행은 shell string이 아니라 `qstar.cli { ... }` argv-vector다.
- MSVC 계열 response file은 `response_style = "msvc"`로 dry-run과 log에서 확인한다.
- `.exe`는 `artifact_name = "tool.exe"` 또는 profile `artifact_names`로 명시한다.
- 외부 system library `libs = {"kernel32"}`는 MSVC-like target에서 `kernel32.lib`로
  렌더링한다.
- QStar가 직접 만드는 static `.lib`, `.dll`, import library, PDB, Windows install
  layout은 아직 official contract가 아니다.

```sh
make qstar-windows-prep-tests
./build/bin/qstar --file tests/corpus/response-files/qstar.lua build //:all
./build/bin/qstar --file tests/corpus/response-files/qstar.lua \
  --profile windows-msvc dry-run //:windows_app
./build/bin/qstar --file tests/corpus/response-files/qstar.lua \
  --profile windows-msvc-fake build //:windows_rsp
```

`.github/workflows/windows-validation.yml`은 `workflow_dispatch` 전용 후보 workflow다.
MSYS2 UCRT64 환경에서 `make all`, `make qstar-windows-prep-tests`, install docs/man smoke를
실행하도록 설계했지만, regular CI나 release gate가 되기 전까지 Windows official support로
표기하지 않는다.

## 관련 diagnostic

- `qstar: qstar.lua not found`
- `qstar doctor`의 compiler/profile/toolchain warning
