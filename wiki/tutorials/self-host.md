# Self-Host

QStar repository는 Makefile을 canonical bootstrap과 release build path로 유지하면서,
동시에 `qstar.lua` self-host graph를 제공한다. Self-host graph는 QStar-built binary가 QStar
repository를 다시 이해하고 빌드할 수 있는지 확인하는 backend parity gate다.

## 최소 예제

```sh
make all
build/bin/qstar --file qstar.lua check
build/bin/qstar --file qstar.lua -B build/qstar-self build //:qstar
build/qstar-self/out/___qstar/qstar --version
```

## 전체 예제

```sh
make all
make qstar-self-host-tests
```

`make qstar-self-host-tests`는 다음을 확인한다.

- Makefile-built `build/bin/qstar`가 root `qstar.lua`를 `check`할 수 있음
- Stella executor가 `//:qstar`를 빌드할 수 있음
- Stella-built binary의 `--version`이 Makefile-built binary와 일치함
- Stella-built binary가 sample project와 QStar self graph를 `check`할 수 있음
- `//:self_host` group이 version/check smoke를 실행함
- Ninja backend가 `//:qstar`를 빌드할 수 있음
- Ninja-built binary의 `--version`이 Makefile-built binary와 일치함
- `compile_commands.json`이 생성됨
- Ninja backend가 repository root에 `.ninja_log`나 `.ninja_deps`를 만들지 않음

## Self-Host Targets

QStar repository의 root `qstar.lua`는 다음 target을 제공한다.

```sh
build/bin/qstar --file qstar.lua list-targets
build/bin/qstar --file qstar.lua build //:qstar
build/bin/qstar --file qstar.lua build //:self_host
```

주요 target:

- `//:lua_vendor`: vendored Lua C sources를 archive한다.
- `//:qstar_core`: QStar core C sources를 archive한다.
- `//:qstar`: self-host QStar executable을 만든다.
- `//:self_version`: self-host binary의 `--version` smoke를 실행한다.
- `//:self_check_sample`: self-host binary로 sample project를 `check`한다.
- `//:self_check_graph`: self-host binary로 QStar repository의 `qstar.lua`를 `check`한다.
- `//:self_host`: 위 smoke target을 묶는 dependency-only group이다.

## Makefile 유지 원칙

Self-host graph는 Makefile을 대체하지 않는다. Makefile은 다음 이유로 계속 유지한다.

- 깨끗한 bootstrap path
- release binary 기준 build
- install/docs/manpage packaging
- QStar evaluator나 executor 회귀가 생겼을 때 복구 가능한 safety net

Self-host가 안정화될수록 release gate에서 차지하는 비중은 커질 수 있지만, Makefile 제거는 별도
결정이다.

## 관련 CLI

```sh
make qstar-self-host-tests
qstar --file qstar.lua check
qstar --file qstar.lua build //:qstar
qstar --file qstar.lua build //:self_host
qstar -G ninja --file qstar.lua build //:qstar
```

## 관련 문서

- [Getting Started](../getting-started.md)
- [Installation](../installation.md)
- [Target Rules](../reference/target-rules.md)
- [Run Target](../reference/run-target.md)
