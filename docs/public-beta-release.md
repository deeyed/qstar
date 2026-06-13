# Public Beta Release Gate

이 문서는 QStar public beta release를 다시 만들 때의 수동 실수를 줄이기 위한
canonical checklist다. 현재 release asset은 macOS arm64 runtime tarball만 대상으로
한다. VSCode extension은 별도 검증/패키징 대상이며 runtime tarball에 포함하지 않는다.
VSCode extension is not included in the public beta runtime tarball.

## Release Target

현재 beta package 이름은 runtime version에서 파생한다.

```txt
runtime version: qstar 0.4.0-beta.1
release tag: v0.4.0-beta.1
macOS asset: qstar-v0.4.0-beta.1-macos-arm64.tar.gz
checksum file: SHA256SUMS
```

`QSTAR_VERSION`은 `include/qstar/qstar.h`의 string을 기준으로 읽는다. Release tag가
찍힌 commit에서 package script를 실행하면 현재 tag가 `v$QSTAR_VERSION`과 같은지
검증한다. Tag를 아직 만들지 않은 main branch에서는 package smoke만 수행하고
`tag=not-on-tag`를 출력한다.

## Local Gate

Release 전에 다음 gate를 순서대로 통과시킨다.

```sh
make all
make check
make qstar-public-beta-release-tests
```

`make qstar-public-beta-release-tests`는 `tools/package-public-beta.sh`를 실행한다.
이 script는 다음을 확인한다.

- `make install PREFIX=<release-root>`가 성공한다.
- installed binary의 `qstar --version`이 `QSTAR_VERSION`과 일치한다.
- Darwin host에서는 installed binary가 ad-hoc codesign되어 있고 `codesign --verify`가
  통과한다.
- installed wiki, `qstar(1)`, `qstar-lua(5)` manpage가 존재한다.
- `QSTAR_DOC_DIR=<release-root>/share/doc/qstar qstar docs --path`가 installed wiki root를
  가리킨다.
- macOS arm64 package에서는 `file bin/qstar`가 arm64 binary로 보고한다.
- tarball layout이 prefix install 구조다.
- `SHA256SUMS`가 release tarball을 포함한다.
- VSCode `.vsix`는 runtime tarball에 포함되지 않는다.

## Install Smoke

Release tarball 생성과 별개로 installed tree smoke를 한 번 더 수행한다.

```sh
make install PREFIX=/tmp/qstar-release-smoke
/tmp/qstar-release-smoke/bin/qstar --version
QSTAR_DOC_DIR=/tmp/qstar-release-smoke/share/doc/qstar \
  /tmp/qstar-release-smoke/bin/qstar docs --path
test -f /tmp/qstar-release-smoke/share/man/man1/qstar.1
test -f /tmp/qstar-release-smoke/share/man/man5/qstar-lua.5
codesign -dv --verbose=2 /tmp/qstar-release-smoke/bin/qstar
```

`codesign` smoke는 Darwin/macOS에서만 의미가 있다. Linux validation host에서는
docs/manpage install과 runtime version만 확인한다.

Linux runtime tarball은 아직 public beta asset이 아니다. Linux asset을 추가하려면
`.github/workflows/linux-validation.yml`의 Ubuntu gcc/clang lane이 green이어야 하고,
각 lane에서 `make check`, `make qstar-linux-validation-tests`, Ninja backend parity,
install docs/man smoke가 통과해야 한다.

## Tarball Layout

Runtime tarball은 prefix에 바로 풀 수 있어야 한다.

```txt
bin/qstar
share/doc/qstar/wiki/AI_INDEX.md
share/man/man1/qstar.1
share/man/man5/qstar-lua.5
README.md
README.ko.md
LICENSE.md
LICENSE/lua.txt
LICENSE/README.md
```

예상 설치 명령:

```sh
tar -xzf qstar-v0.4.0-beta.1-macos-arm64.tar.gz -C "$HOME/.local"
export PATH="$HOME/.local/bin:$PATH"
qstar --version
```

## Wiki Sync Checklist

Repo 내부 `wiki/`가 source-of-truth다. GitHub Wiki는 release 직전에 mirror한다.

```sh
tools/sync-github-wiki.sh
```

확인 항목:

- GitHub repo wiki가 enabled 상태다.
- `wiki/README.md`가 GitHub Wiki `Home.md`로 복사된다.
- `_Sidebar.md`가 주요 guide/reference 문서를 가리킨다.
- Wiki에는 특정 downstream project 이름을 남기지 않는다.
- `wiki/AI_INDEX.md`가 최신 CLI, backend, release gate를 포함한다.

Network push가 필요한 작업이므로 local package smoke와 문서 drift gate가 통과한 뒤
실행한다.

## VSCode Extension Policy

VSCode extension은 runtime과 별도 version을 가진다. 현재 package version은 `0.3.0`이다.

```sh
make vscode-extension-tests
cd editors/vscode/qstar
npm run package:vsix
```

이번 public beta runtime tarball에는 `.vsix`를 포함하지 않는다. Extension release가
필요하면 GitHub release asset을 별도로 추가할지 별도 release로 배포할지 release note에서
명시한다. `.vsix`, `node_modules`, extension `dist/` 산출물은 source tree에 commit하지
않는다.

## Publish Checklist

Local smoke가 끝난 뒤 tag와 GitHub release를 만든다.

```sh
git tag -a v0.4.0-beta.1 -m "QStar v0.4.0 beta 1"
git push origin v0.4.0-beta.1

gh release create v0.4.0-beta.1 \
  dist/release/qstar-v0.4.0-beta.1-macos-arm64.tar.gz \
  dist/release/SHA256SUMS \
  --repo deeyed/qstar \
  --title "QStar v0.4.0 Beta 1" \
  --notes-file docs/releases/v0.4.0-beta.1.md \
  --prerelease \
  --latest=false
```

GitHub release 생성은 QStar commit/push, wiki sync, release smoke가 끝난 뒤에만 수행한다.
