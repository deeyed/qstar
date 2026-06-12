# QStar Standalone Extraction Status

이 문서는 QStar가 독립 git repository로 분리된 뒤 유지해야 하는 standalone 경계를
기록한다. Cale repository는 `qstar/`를 이 repository의 submodule pin으로 소비한다.

```txt
status: standalone-repository-extracted
current runtime: qstar 0.4.0
current extension: qstar-vscode 0.3.0
actual split: completed
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

`docs/`는 QStar standalone repository의 문서 root로 취급한다. Cale repository의
`docs/qstar/`는 상세 mirror가 아니라 이 submodule 문서를 가리키는 얇은 handoff만
유지한다.

## Cale Side Conversion Contract

Cale repository에서 QStar를 소비할 때는 다음 경계를 유지한다.

- Cale `.gitmodules`: `qstar/` entry만 둔다.
- QStar standalone `.gitmodules`: `vendor/lua` entry를 소유한다.
- Cale repository: `qstar/` directory를 QStar remote submodule로 둔다.
- Cale root docs: QStar 상세 문서 대신 submodule 문서를 가리키는 얇은 handoff만 남김.
- Local install: standalone repo 기준 `make install PREFIX=$HOME/.local` 또는 Cale
  submodule 기준 `make -C qstar install PREFIX=$HOME/.local`로 `~/.local/bin/qstar`를
  갱신한다.
- VSCode extension: standalone repo 또는 Cale submodule의 `editors/vscode/qstar`에서
  `npm run package:vsix` 후 `qstar-vscode-0.3.0.vsix`를 재설치한다.

## Extraction Gate

분리 전후 모두 다음 gate를 통과해야 한다.

```sh
make -C qstar check
make -C qstar qstar-v0.4-pilot-tests
make -C qstar vscode-extension-tests
git diff --check -- qstar docs/qstar
```

QStar standalone repository로 옮긴 뒤에는 같은 의미의 command가 다음처럼 바뀐다.

```sh
make check
make qstar-v0.4-pilot-tests
make vscode-extension-tests
git diff --check
```

## Standalone Ownership

QStar standalone repository가 다음을 소유한다.

- QStar source, tests, samples, wiki, editor extension.
- Lua vendor submodule and Lua license notice.
- QStar release/version/editor packaging policy.
- QStar build, install, lint, LSP, executor, corpus gates.

Cale repository는 QStar implementation file을 mirror하지 않는다. Cale build integration이
필요해지면 Cale side glue만 Cale repository에서 관리한다.

## Readiness Verdict

QStar는 독립 repo/submodule 전환 준비가 됐다. 남은 작업은 구조적 blocker가 아니라
repository operation이다. 다만 split 직후에는 `docs/`, `LICENSE/`, `.gitmodules`,
installed binary, VSCode extension packaging path의 drift를 반드시 다시 확인해야 한다.
