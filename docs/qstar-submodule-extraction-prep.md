# QStar Submodule Extraction Prep

이 문서는 QStar를 독립 git repository로 분리하고, Cale repository의 `qstar/`를
submodule pin으로 전환하기 전 마지막 정리 기준이다. Round 66 이전에는 실제
`.gitmodules` 변경이나 path 제거를 하지 않는다.

```txt
status: ready-for-submodule-extraction-prep
current runtime: qstar 0.3.0
current extension: qstar-vscode 0.2.0
actual split: deferred to next round
```

## Current Standalone Surface

QStar 독립 repo 후보는 다음을 자기 안에 가진다.

- `Makefile`
- `include/qstar`
- `src`
- `tests`
- `tests/projects`
- `wiki`
- `docs`
- `LICENSE`
- `assets`
- `editors/vscode/qstar`
- `vendor/lua`

`docs/`는 Cale root `docs/qstar/` mirror가 아니라 QStar standalone repository의
문서 root로 취급한다. Cale repository의 `docs/qstar/`는 실제 submodule split 전까지
transition mirror로 남긴다.

## Known Path Changes For The Split Round

실제 분리 라운드에서는 다음 path rewrite가 필요하다.

- Cale `.gitmodules`: `qstar/vendor/lua` entry 제거.
- QStar standalone `.gitmodules`: `vendor/lua` entry 추가.
- Cale repository: `qstar/` directory를 QStar remote submodule로 교체.
- Cale root docs: QStar 상세 문서 대신 submodule 문서를 가리키는 얇은 handoff만 남김.
- Local install: `make -C qstar install PREFIX=$HOME/.local` 또는 standalone repo 기준
  `make install PREFIX=$HOME/.local`로 `~/.local/bin/qstar` 갱신.
- VSCode extension: standalone repo에서 `npm run package:vsix` 후
  `qstar-vscode-0.2.0.vsix` 재설치.

## Extraction Gate

분리 전후 모두 다음 gate를 통과해야 한다.

```sh
make -C qstar check
make -C qstar qstar-v0.3-rc-tests
make -C qstar vscode-extension-tests
git diff --check -- qstar docs/qstar
```

QStar standalone repository로 옮긴 뒤에는 같은 의미의 command가 다음처럼 바뀐다.

```sh
make check
make qstar-v0.3-rc-tests
make vscode-extension-tests
git diff --check
```

## Do Not Split Yet

이번 prep round에서는 다음을 하지 않는다.

- Cale repository의 `qstar/` 삭제.
- QStar remote repository 생성.
- QStar submodule 추가.
- Cale `Makefile`, frontend, backend, compiler driver integration 수정.
- `cale build` 내부에서 QStar 호출.

## Readiness Verdict

QStar는 독립 repo/submodule 전환 준비가 됐다. 남은 작업은 구조적 blocker가 아니라
repository operation이다. 다만 split 직후에는 `docs/`, `LICENSE/`, `.gitmodules`,
installed binary, VSCode extension packaging path의 drift를 반드시 다시 확인해야 한다.
