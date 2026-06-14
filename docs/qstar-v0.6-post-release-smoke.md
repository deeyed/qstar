# QStar 0.6 Post-Release Smoke

이 문서는 `v0.6.1-beta` GitHub release가 실제 사용자 다운로드 경로에서 정상 동작하는지
확인하기 위한 post-release smoke 기록이다. Source tree에서 tarball을 만드는
`make qstar-public-beta-release-tests`와 다르게, 이 gate는 GitHub release에 올라간 asset을
다시 다운로드해서 검증한다.

```txt
release tag: v0.6.1-beta
release url: https://github.com/deeyed/qstar/releases/tag/v0.6.1-beta
macOS asset: qstar-v0.6.1-beta-macos-arm64.tar.gz
Linux asset: qstar-v0.6.1-beta-linux-x86_64.tar.gz
checksum file: SHA256SUMS
status: post-release download smoke required for release seal
```

## Gate

```sh
make qstar-public-beta-download-smoke
```

이 target은 `tools/smoke-github-release.sh`를 실행한다. 기본값은 현재
`include/qstar/qstar.h`의 `QSTAR_VERSION`을 읽어 `v$QSTAR_VERSION` tag와
현재 host에 맞는 `qstar-v$QSTAR_VERSION-<platform>.tar.gz` asset을 검증한다.

## What It Checks

- GitHub release asset tarball download.
- GitHub release `SHA256SUMS` download.
- `SHA256SUMS` 안의 asset checksum과 실제 tarball checksum 일치.
- Prefix tarball layout:
  - `bin/qstar`
  - `share/doc/qstar/wiki/AI_INDEX.md`
  - `share/doc/qstar/wiki/reference/qstar-lua.md`
  - `share/man/man1/qstar.1`
  - `share/man/man5/qstar-lua.5`
  - `README.md`, `README.ko.md`, `LICENSE.md`, `LICENSE/lua.txt`
- VSCode `.vsix`가 runtime tarball에 포함되지 않음.
- `/tmp` 아래 임시 prefix에 extract한 뒤 `bin/qstar --version`이 기대 version과 일치.
- `QSTAR_DOC_DIR=<prefix>/share/doc/qstar qstar docs --path`가 installed wiki root를 가리킴.
- `qstar docs --ai-index`와 `qstar docs --show reference/qstar-lua.md`가 installed docs 기준으로 동작.
- manpage file이 설치되어 있음.
- macOS host에서는 downloaded binary의 ad-hoc codesign detail과 `codesign --verify` 통과.
- `file bin/qstar`가 macOS arm64 또는 Linux x86-64 binary sanity를 만족.

## Manual Equivalent

```sh
tmp=$(mktemp -d)
cd "$tmp"
curl -fsSLO https://github.com/deeyed/qstar/releases/download/v0.6.1-beta/qstar-v0.6.1-beta-macos-arm64.tar.gz
curl -fsSLO https://github.com/deeyed/qstar/releases/download/v0.6.1-beta/SHA256SUMS
shasum -a 256 qstar-v0.6.1-beta-macos-arm64.tar.gz
cat SHA256SUMS
mkdir root
tar -xzf qstar-v0.6.1-beta-macos-arm64.tar.gz -C root
root/bin/qstar --version
QSTAR_DOC_DIR="$tmp/root/share/doc/qstar" root/bin/qstar docs --path
test -f root/share/man/man1/qstar.1
test -f root/share/man/man5/qstar-lua.5
codesign -dv --verbose=2 root/bin/qstar
codesign --verify root/bin/qstar
```

## Known Issues After 0.6

- macOS arm64 and Linux x86_64 are public beta binary assets.
- Linux x86_64 asset smoke must run on Linux; macOS hosts intentionally refuse
  to execute the Linux binary.
- Windows is a manual native validation candidate and not official host support.
- Stella daemon is beta opt-in and default-off.
- Stable daemon API version promise, Linux daemon CI lane, and Windows named pipe support remain deferred.

## Overrides

```sh
QSTAR_RELEASE_TAG=v0.6.1-beta make qstar-public-beta-download-smoke
QSTAR_RELEASE_PLATFORM=macos-arm64 make qstar-public-beta-download-smoke
QSTAR_RELEASE_PLATFORM=linux-x86_64 make qstar-public-beta-download-smoke
QSTAR_RELEASE_SMOKE_DIR=/tmp/qstar-release-smoke tools/smoke-github-release.sh
```

`QSTAR_RELEASE_SMOKE_DIR` must be absolute. If it is not provided, the script uses
a temporary directory under `${TMPDIR:-/tmp}` and removes it on exit.
