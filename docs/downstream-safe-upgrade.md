# Downstream Safe Upgrade Gate

상태: Q284 downstream compatibility seal

이 문서는 QStar의 stable 후보 문법과 backend 동작이 실제 대규모 프로젝트에서
계속 평가되는지 확인하는 opt-in 호환성 gate의 정본이다. 이 gate는 QStar core에
특정 제품, 실행 환경, 언어 정책을 추가하지 않는다. 외부 프로젝트가 이미 작성한
generic target, object library, generated action, stage, test, root command를 QStar가
이전과 같은 Graph IR과 argv로 해석하는지만 확인한다.

## 책임 경계

QStar 저장소는 다음만 소유한다.

- `tests/downstream-compat/*.json`의 호환성 snapshot
- `tests/downstream-safe-upgrade.py`의 opt-in runner
- `make qstar-downstream-safe-upgrade-tests` entrypoint
- local v1 release-candidate gate와 문서 drift guard 연결

외부 프로젝트의 source, 정책, marker, board metadata, 실행 결과의 의미는 해당
프로젝트가 소유한다. snapshot 안에 나타나는 이름은 QStar builtin keyword가 아니며,
QStar가 그 문자열을 해석하거나 추론한다는 뜻도 아니다.

## 원본 저장소 보호

runner는 전달받은 원본 경로에서 `qstar.lua` 존재 여부와 commit id만 읽는다. 실제
graph 평가, dry-run, build, test는 모두 `${TMPDIR}` 아래의 새 복사본에서 실행한다.
기본 copy는 기존 `build/`를 제외하며, graph load에 필요한 generated config가 있으면
그 하위만 복사한다. Delos형 version input을 위해 필요한 repository metadata도 복사본에
보존한다. 원본 저장소에는 state, output, cache, action log를 쓰지 않는다.

## Snapshot 의미

snapshot schema는 `qstar-downstream-compat-v1`이다. snapshot은 다음을 기록한다.

| 항목 | 검증 의미 |
| --- | --- |
| `minimums` | target, generated action, stage, root command의 최소 graph 규모 |
| `minimum_target_kinds` | objectlib, test, run target 등 핵심 kind의 최소 개수 |
| `targets` | label, kind, compile context, 대표 artifact path |
| `generated_actions` | generated producer label, toolset, output path |
| `stages` | stage label, root, destination layout |
| `commands` | baseline root command 이름 전체를 required subset으로 보존 |
| `dry_runs` | 대표 label의 argv와 artifact path marker |
| `builds`, `smokes` | 명시적으로 opt-in할 때만 실행할 대표 동작 |

비교는 compatible-subset 방식이다. baseline label, command, artifact path가 사라지거나
의미가 바뀌면 실패하지만, downstream이 새 target이나 command를 추가하는 것은 허용한다.
snapshot의 `source_commit`과 실제 commit이 다르면 그 사실을 출력하되, stable subset이
유지되면 통과한다. 의도적인 rename/removal은 downstream 변경과 QStar snapshot 변경을
같이 검토해야 한다.

## 기본 실행

외부 경로를 지정하지 않은 release host와 CI에서는 명확한 skip 결과를 낸다.

```sh
make qstar-downstream-safe-upgrade-tests
```

세 프로젝트를 모두 필수로 검사하려면 다음처럼 실행한다. Ribon 경로를 생략하면
Parus root의 `stand/Ribon`이 존재할 때 자동으로 사용한다.

```sh
QSTAR_PARUS_ROOT=/path/to/parus \
QSTAR_DELOS_ROOT=/path/to/delos \
QSTAR_RIBON_ROOT=/path/to/ribon \
QSTAR_DOWNSTREAM_REQUIRED=1 \
make qstar-downstream-safe-upgrade-tests
```

기본 opt-in 검증은 각 프로젝트에 대해 다음을 수행한다.

1. `/tmp` 복사본 생성
2. `qstar check //...`
3. `list-targets --format json`과 `commands --format json` snapshot 비교
4. 대표 label의 Stella dry-run
5. 같은 label의 Ninja dry-run
6. 두 backend의 `command_argv` line 완전 일치 확인

## 실제 Build와 Smoke

실제 compiler/tool 실행은 host 의존성이 있으므로 추가 opt-in이다.

```sh
QSTAR_PARUS_ROOT=/path/to/parus \
QSTAR_DELOS_ROOT=/path/to/delos \
QSTAR_RIBON_ROOT=/path/to/ribon \
QSTAR_DOWNSTREAM_REQUIRED=1 \
QSTAR_DOWNSTREAM_RUN_BUILDS=1 \
QSTAR_DOWNSTREAM_RUN_SMOKES=1 \
QSTAR_DOWNSTREAM_KEEP_TEMP=1 \
make qstar-downstream-safe-upgrade-tests
```

`QSTAR_DOWNSTREAM_KEEP_TEMP=1`이면 통과 후에도 복사본과 evidence를 남긴다. 실패하면
원인 분석을 위해 temp root를 항상 보존하고 경로를 stderr에 출력한다.

## Evidence와 Claim 경계

graph-only 통과는 다음만 증명한다.

- public declaration schema가 downstream 파일을 계속 수용한다.
- target/objectlib/generated/stage/command label과 artifact path가 유지된다.
- 대표 argv가 Stella와 Ninja에서 같다.

실제 build/smoke 통과는 선택한 host와 선택한 대표 label의 실행 evidence를 추가한다.
한 simulator 또는 host test의 성공을 다른 hardware나 전체 제품의 성공으로 승격하지
않는다. release asset publish/download도 이 gate의 책임이 아니다.

## 문서, Manpage, LSP

Q284는 새 public DSL symbol이나 field를 추가하지 않는다. 따라서 LSP completion/hover
table에는 downstream 이름이나 새 keyword를 넣지 않는다. `make check`의 기존 LSP,
formatter, strict schema, generic backend parity를 그대로 통과시키고, 이 문서와 wiki,
manpage, AI index에는 gate의 사용법과 claim 경계만 연결한다.

## v1 Gate 연결

`make qstar-v1-release-candidate-tests`는 이 opt-in gate를 호출한다. 외부 checkout이 없는
3OS hosted lane에서는 `status=skipped`를 기록하고 기존 release matrix를 계속 검증한다.
checkout 경로를 제공한 통합 lane에서는 같은 target이 snapshot 검증을 수행한다. 이
연결은 GitHub Release asset을 생성하거나 publish하지 않는다.
