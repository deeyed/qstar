# QStar Generic DSL Backend Seal

이 문서는 generic DSL hard cut 이후 Stella와 Ninja backend가 같은 current
authoring surface를 이해하고, medium performance gate에서 회귀가 없음을 확인하는
Q193 release gate다.

```txt
status: generic DSL backend seal
round: Q193
surface: qstar.toolset + qstar.config + generated/object/sharedlib + generic workflow
platform scope: macOS local, Linux CI
timing policy: structural failure is hard fail; timing regression uses perf-summary hard threshold
```

## Gate Command

개발자는 다음 target을 실행한다.

```sh
make qstar-generic-dsl-backend-parity-tests
```

이 target은 `tests/generic-dsl-backend-seal.sh`를 실행한다. 이 스크립트는
다음 gate를 한 번에 묶는다.

- QStar self-host graph: Stella build와 Ninja build의 `qstar --version` 일치
- Stella generated corpus: `configure_file`, `custom_target`, run target, generated_dir
- Stella object artifact bridge: generated object를 executable/staticlib/sharedlib가 소비
- Ninja backend parity: generated/object/sharedlib/stage/install/action-log/replay
- Generic workflow fixture: `qstar.transform`, `run_target.inputs`, `qstar.stage_dir`,
  `qstar.command`, bool argv helper, `qstar.step.export_stage`
- medium performance gate: Stella/Ninja clean, no-op, incremental line protocol
- Linux validation script: generated_dir, compile database, install docs/man smoke

## Performance Regression Policy

Q193의 목적은 syntax hard cut이 backend fast path를 느리게 만들지 않았다는 것을
확인하는 것이다. Timing은 host와 filesystem cache에 흔들리지만, 다음 구조적 항목은
hard fail이다.

- `qstar.toolset` 기반 self-host graph가 Stella와 Ninja 양쪽에서 build된다.
- generated object artifact가 Stella와 Ninja 양쪽에서 link/archive input이 된다.
- macOS/Linux sharedlib artifact가 Stella/Ninja parity corpus에서 생성된다.
- `tests/projects/generic-command-artifact-workflow`가 Stella/Ninja 양쪽에서 transform,
  stage-as-input, run target expect, project command export를 실행한다.
- medium gate가 Stella/Ninja clean, no-op, incremental phase를 모두 출력한다.
- `perf-summary` hard threshold를 넘지 않는다.

기본 threshold는 다음과 같다.

```txt
warning threshold: Stella/Ninja 2.0x + 250ms
hard threshold: Stella/Ninja 2.5x + 500ms
```

Release 후보에서 더 좁게 보고 싶으면 환경변수로 조정한다.

```sh
QSTAR_GENERIC_DSL_WARN_RATIO_X100=150 \
QSTAR_GENERIC_DSL_WARN_SLACK_MS=100 \
QSTAR_GENERIC_DSL_HARD_RATIO_X100=200 \
QSTAR_GENERIC_DSL_HARD_SLACK_MS=250 \
make qstar-generic-dsl-backend-parity-tests
```

## Linux CI

`.github/workflows/linux-validation.yml`의 gcc/clang matrix는 같은 target을 실행하고
다음 artifact를 업로드한다.

```txt
dist/perf/generic-dsl-<compiler>/generic-dsl-medium-perf.txt
dist/perf/generic-dsl-<compiler>/generic-dsl-medium-summary.txt
dist/perf/generic-dsl-<compiler>/generic-dsl-medium-summary.md
dist/perf/generic-dsl-<compiler>/generic-dsl-self-host.txt
dist/perf/generic-dsl-<compiler>/generic-dsl-ninja-backend-parity.txt
dist/perf/generic-dsl-<compiler>/generic-dsl-linux-validation.txt
```

Linux CI에서 이 gate가 green이면 current generic DSL surface는 macOS local run과
Linux hosted validation에서 같은 backend 품질 계약을 가진다.

## Release Gate Usage

Generic DSL hard cut 이후 release 후보는 최소 다음 명령을 통과해야 한다.

```sh
make all
make check
make qstar-self-host-tests
make qstar-medium-project-readiness-tests
make qstar-ninja-backend-parity-tests
make qstar-generic-dsl-backend-parity-tests
git diff --check
```

`make qstar-release-candidate-tests`는 Q193 이후 generic DSL backend seal target을
가리킨다.
