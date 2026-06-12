# QStar Self-Host

QStar self-host graph는 QStar repository가 자기 자신을 QStar로 빌드할 수 있는지 검증하는
parallel build path다. 이 path는 Makefile을 대체하지 않는다.

## Contract

- Makefile은 canonical bootstrap과 release build path로 유지한다.
- Root `qstar.lua`는 QStar repository의 source graph를 표현한다.
- `//:qstar`는 QStar-built binary를 만든다.
- `//:self_host`는 self-host smoke target을 묶는 dependency-only group이다.
- Self-host gate는 Makefile-built binary와 QStar-built binary의 `--version` 출력을 비교한다.
- Stella executor와 Ninja backend 모두 `//:qstar` build path를 검증한다.
- Ninja backend는 repository root에 `.ninja_log`나 `.ninja_deps`를 만들면 안 된다.

## Source Graph

Root `qstar.lua`는 다음 구조를 가진다.

- `qstar/policies/selfhost.qst`: C option, include path, vendored Lua option을 `qstar.config`로 선언
- `qstar/modules/sources/sources.qsm`: QStar core source list와 Lua vendor source list를 반환
- `//:lua_vendor`: vendored Lua archive
- `//:qstar_core`: QStar core archive
- `//:qstar`: QStar executable
- `//:self_version`: self-host binary version smoke
- `//:self_check_sample`: self-host binary sample project check
- `//:self_check_graph`: self-host binary QStar repository graph check
- `//:self_host`: smoke target group

## Release Gate Candidate

Self-host gate는 release 전에 다음 명령으로 실행한다.

```sh
make qstar-self-host-tests
```

이 target은 `make all`로 만든 Makefile-built binary를 사용해 QStar self-host graph를 평가하고,
Stella executor와 Ninja backend가 같은 repository graph를 빌드할 수 있는지 확인한다.

## Non-Goals

- Makefile 제거
- dependency fetcher 도입
- release binary 기준을 즉시 self-host binary로 전환
- QStar가 자기 source list를 filesystem glob으로 암묵 scan

QStar self-host는 deterministic source list와 explicit target graph를 유지한다.
