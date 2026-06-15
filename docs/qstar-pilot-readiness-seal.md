# QStar Pilot Readiness Seal

QStar pilot-readiness seal은 v0.4 Stella standalone surface 위에 formatter/help/wiki/CLI
동기화를 더해, 다음 real systems-project pilot에 넣어도 authoring UX가 크게 흔들리지
않는 상태를 고정한다.

```txt
status: pilot-readiness seal
runtime version: qstar 0.4.0-beta.1
release gate: make -C qstar qstar-pilot-readiness-tests
```

이 seal은 board-specific builtin을 뜻하지 않는다. UEFI, RPi, QEMU smoke, binary image
변환은 모두 `qstar.profile`, `qstar.custom_target`, `qstar.run_target`, `qstar.stage`,
`qstar.target_family` 같은 generic primitive로 표현한다.

## Stable UX Contract

- `qstar.lua`가 단일 project entrypoint다.
- Subdir fragment는 `.qst`다.
- Mandatory TOML 설정은 없다.
- Default work directory는 `build/qstar`다.
- `qstar <subcommand> --help`는 graph 평가 없이 help를 출력한다.
- `qstar docs`는 installed wiki, AI index, manpage 진입점을 출력한다.
- `qstar fmt`는 `qstar.*` authoring block만 canonicalize하고, `local function` 같은
  ordinary Lua helper는 보존한다.
- Wiki examples와 CLI behavior는 drift test로 묶는다.
- `wiki/AI_INDEX.md`와 installed manpage는 AI agent가 QStar surface를 빠르게 찾는
  문서 entrypoint다.
- Systems corpus는 firmware-specific builtin 없이 generic QStar primitive만 사용한다.

## Release Gate

Recommended local gate:

```txt
make -C qstar check
make -C qstar qstar-pilot-readiness-tests
make -C qstar qstar-wiki-cli-sync-tests
make -C qstar qstar-systems-corpus-tests
make -C qstar vscode-extension-tests
```

현재 대부분의 Makefile release alias는 QStar-local smoke harness를 실행한다.
`vscode-extension-tests`는 예외적으로 VSCode extension package metadata와 payload만
검증하는 좁은 target이다. Alias는 release script와 외부 pilot automation이 안정적인
이름을 쓰기 위한 surface다.

## Drift Lockdown

다음 drift는 실패로 본다.

- Wiki authoring example이 removed `timeout_sec` field를 쓰는 경우. Canonical field는
  `timeout`이다.
- `qstar build --help`, `qstar test --help`, `qstar stage --help`,
  `qstar dry-run --help`가 target label처럼 처리되는 경우.
- `qstar docs`, `man qstar`, `wiki/AI_INDEX.md`가 서로 다른 entrypoint를 가리키는
  경우.
- Systems corpus가 dedicated UEFI/RPi/embed-binary builtin 같은 board-specific
  builtin을 쓰는 경우.
- Root-scattered `.qstar/`, `generated/`, `stage/`, root `compile_commands.json`를
  default output으로 되돌리는 경우.

## Deferred

- `downstream build` integration.
- Remote package resolver and lockfile policy.
- Non-C/C++/external-language provider plugin ABI.
- Full shared library executor policy.
- C++ modules executor.
