# Installation

QStar는 C/C++/Cale을 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다. Public
beta에서는 macOS arm64 tarball을 먼저 배포하며, 소스에서 직접 빌드할 수도 있다.

## 최소 예제

```sh
tar -xzf qstar-v0.4.0-beta.1-macos-arm64.tar.gz -C "$HOME/.local"
export PATH="$HOME/.local/bin:$PATH"
qstar --version
```

## 전체 예제

```sh
git clone https://github.com/deeyed/qstar.git
cd qstar
make check
make qstar-self-host-tests
make install PREFIX="$HOME/.local"
qstar init c-app /tmp/qstar-install-smoke
qstar --file /tmp/qstar-install-smoke/qstar.lua build //:app
```

VSCode extension은 `qstar/editors/vscode/qstar` 아래에 있고, `.vsix`는 commit하지 않는
release artifact다.

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
qstar doctor
```

## 관련 diagnostic

- `qstar: qstar.lua not found`
- `qstar doctor`의 compiler/profile/toolchain warning
