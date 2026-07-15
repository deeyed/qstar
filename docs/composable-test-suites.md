# QStar Composable Test Suite

이 문서는 `qstar.test_suite`와 `qstar test --suite/--tag/--exclude-tag`의 정식 계약을
정의한다. Test suite는 새로운 실행 파일이나 action을 만드는 target이 아니다. 이미 선언된
`qstar.test`, `qstar.run_target`, 다른 suite label을 이름 붙은 분류 집합으로 조합하는
Graph IR primitive다. 기존 target label과 target의 build/run 의미는 변경하지 않는다.

## 설계 목표

- Leaf 폴더는 자신이 소유한 test 또는 run target을 기존 방식으로 선언한다.
- 상위 fragment는 기존 label을 다시 작성하지 않고 suite로 조합한다.
- Suite는 다른 suite를 포함할 수 있다.
- `tags`는 project가 자유롭게 정하는 exact string metadata다.
- Stella와 Ninja는 하나의 공통 resolver가 만든 같은 ordered unique member closure를 실행한다.
- Test 결과는 실제로 실행한 member의 성공만 증명한다. 한 실행 환경의 성공을 다른 환경의
  성공으로 해석하지 않는다.

## 문법

```lua
qstar.test "scheduler_unit" {
  sources = {"tests/scheduler_unit.c"},
}

qstar.run_target "runtime_probe" {
  command = qstar.cli {"tools/runtime-probe"},
  timeout = 10,
  expect = {
    contains = "PROBE_OK",
  },
}

qstar.test_suite "host_units" {
  tests = {"//:scheduler_unit"},
  tags = {"host", "fast"},
  description = qstar.status("Host-classified unit tests"),
}

qstar.test_suite "runtime_smoke" {
  tests = {"//:runtime_probe"},
  tags = {"simulator", "smoke"},
}

qstar.test_suite "verification" {
  tests = {
    "//:host_units",
    "//:runtime_smoke",
  },
  tags = {"verification"},
}
```

### Builtin field

| Field | Type | Required | 의미 |
| --- | --- | --- | --- |
| `tests` | `list<string>` | yes | Existing `qstar.test`, `qstar.run_target`, or `qstar.test_suite` label. |
| `tags` | `list<string>` | no | Exact user metadata strings used only by CLI selection. |
| `description` | `qstar.status("...")` | no | One-line human-facing suite description. |
| `manual` | boolean | no | `true`이면 implicit tag discovery의 root 후보에서 제외. Default is `false`. |

이 네 field만 builtin이다. Unknown field와 잘못된 type은 source location과 suite label을
포함한 declaration error다. `tests`와 `tags`는 1부터 시작하는 contiguous list여야 하며,
빈 `tests`, duplicate member, duplicate tag는 허용하지 않는다.

### User metadata 경계

`tags` item은 모두 사용자 정의 값이다. 예를 들어 `host`, `simulator`, `emulator`,
`hardware`, `slow`, `nightly`는 QStar keyword가 아니다. QStar는 이 문자열로 다음을 하지
않는다.

- host 또는 target platform 판정
- compiler/linker argv 주입
- runner 선택
- timeout 또는 retry 정책 추론
- simulator 성공을 hardware 성공으로 승격
- hardware 실행 여부 자동 탐지

필요한 실제 실행 의미는 각 `qstar.test` 또는 `qstar.run_target`의 target, command,
inputs, timeout, expect 계약에 명시한다.

## 중첩과 순서

Suite member는 작성 순서대로 depth-first로 펼쳐진다. 같은 target이 여러 경로로 들어오면
첫 번째 위치만 유지한다. 따라서 resolver 결과는 deterministic ordered unique label list다.
Unknown member, test/run target이 아닌 target kind, suite cycle은 graph validation error다.

```lua
qstar.test_suite "all" {
  tests = {
    "//:host_units",
    "//:runtime_smoke",
    "//:scheduler_unit", -- host_units에 이미 있으므로 한 번만 실행
  },
}
```

## Manual suite

`manual = true`는 tag 기반 implicit root discovery만 막는다. 다음 두 경우에는 manual suite도
정상적으로 포함된다.

1. `qstar test --suite //:hardware_manual`처럼 직접 지정
2. 명시 또는 tag-selected suite의 nested member로 포함

이 규칙은 manual suite가 graph에서 보이지 않게 사라지는 것을 막는다. 자동 선택에서 완전히
분리해야 한다면 다른 suite의 `tests`에도 넣지 않는다.

## CLI selection

```sh
qstar test --suite //:verification
qstar test --suite //:host_units --suite //:runtime_smoke
qstar test --tag fast
qstar test --tag host --tag smoke
qstar test --suite //:verification --exclude-tag simulator
```

- `--suite`, `--tag`, `--exclude-tag`는 반복할 수 있다.
- Explicit suite와 tag selection은 합집합이며 target member는 중복 제거된다.
- 여러 include tag는 OR다. Tag 하나라도 exact match하는 non-manual suite가 root가 된다.
- `--exclude-tag`와 일치하는 suite는 root 또는 nested 위치에서 subtree 전체가 제외된다.
- Exclude tag만 주면 모든 non-manual suite를 root 후보로 삼은 뒤 제외한다.
- 선택 결과가 비면 error다.
- Positional target label과 suite/tag filter는 함께 쓸 수 없다.

기존 CLI는 그대로다.

```sh
qstar test //:scheduler_unit  # 단일 qstar.test
qstar test                    # 모든 qstar.test; run_target/suite를 암시적으로 추가하지 않음
```

## Backend 계약

Suite resolver는 backend-independent Graph IR code다. Stella와 Ninja는 같은 label closure를
받는다.

- `qstar.test` member: target을 build한 뒤 test executable을 실행한다.
- `qstar.run_target` member: 해당 backend의 정상 build/run dependency closure를 실행한다.
- 중첩 suite: resolver 단계에서 leaf target label로 평탄화된다.

따라서 suite는 별도 shell loop나 backend 전용 phony graph를 만들지 않는다. Member target의
generated artifact, objectlib, stage input, tool dependency도 기존 producer edge 계약을 그대로
사용한다.

## Query와 JSON

```sh
qstar list-targets
qstar list-targets --format json
qstar query //:verification
qstar query //:verification --format json
qstar query //:scheduler_unit --format json
```

`qstar-targets-v1`은 additive field로 `test_suite_count`, `test_suites`를 제공한다. Suite
record에는 `tests`와 flattened `resolved_tests`가 있다. Target record에는 직접 포함한
`direct_test_suites`와 nested closure까지 포함한 `test_suites`가 있다.

`query --format json`은 `qstar-query-v1` schema를 사용하며 `kind`는 `target` 또는
`test_suite`다. Text query도 같은 membership을 보여준다.

## 증거 해석

Suite 실행 성공은 선택된 member action이 해당 invocation에서 성공했다는 뜻이다. Tag는
증거 등급이 아니다. 특히 `simulator`, `emulator`, `hardware`라는 tag 이름만으로 QStar가
실행 환경을 검증하거나 한 환경의 결과를 다른 환경의 결과로 승격하지 않는다. Project의
release/readiness 문서는 실제 runner, device, artifact, log를 별도로 기록해야 한다.

## Regression gate

```sh
make qstar-composable-test-suite-tests
```

이 gate는 test executable과 run target을 함께 포함한 nested suite를 Stella/Ninja에서
실행하고, tag/manual/exclude selection, cycle/unknown/wrong-kind diagnostics,
list/query JSON membership, 기존 positional `qstar test` 호환성을 검증한다.
