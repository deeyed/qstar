# Installation

QStar는 C/C++/Cale을 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다. 현재는
Cale repo 안의 독립 binary로 빌드하고, 필요하면 사용자 PATH에 직접 올려 쓴다.

## 최소 예제

```sh
make -C qstar
qstar/build/bin/qstar --version
```

## 전체 예제

```sh
make -C qstar check
make -C qstar vscode-extension-tests
install -d "$HOME/.local/bin"
install qstar/build/bin/qstar "$HOME/.local/bin/qstar"
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
make -C qstar
make -C qstar check
make -C qstar vscode-extension-tests
qstar doctor
```

## 관련 diagnostic

- `qstar: qstar.lua not found`
- `qstar doctor`의 compiler/profile/toolchain warning
