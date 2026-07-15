# QStar Test Resources And Result Protocol

이 문서는 `qstar.test_resource`, test-local resource request, retry, setup/cleanup,
manual/skip, JSON/JUnit result report의 정식 계약이다. 이 기능은 특정 test 장비나 실행
환경을 QStar builtin으로 모델링하지 않는다. QStar가 아는 것은 **이름**, **capacity**,
**요청량**, **결과 상태**뿐이다.

## 설계 경계

- `serial-port`, `board`, `gpu`, `database`, `license`, `exclusive-lab` 같은 resource id는
  모두 사용자 정의 문자열이다.
- QStar는 resource id에서 device 종류, platform, runner, evidence 등급을 추론하지 않는다.
- Setup/cleanup은 shell 문자열이 아니라 `qstar.cli { ... }` argv vector다.
- Test executable의 성공 계약은 기존과 같다. Process exit code 0은 pass이고 non-zero는
  fail이다.
- QStar는 test 결과를 기록하고 scheduling하지만 domain-specific output protocol이나
  hardware 성공 조건을 해석하지 않는다.

## Resource 선언

```lua
qstar.test_resource "shared.slot" {
  capacity = 1,
  description = qstar.status("Generic exclusive scheduler slot"),
}
```

| Field | Type | Required | 의미 |
| --- | --- | --- | --- |
| `capacity` | positive integer | yes | 동시에 점유할 수 있는 총량 |
| `description` | `qstar.status("...")` | no | 사람용 한 줄 설명 |

Resource id는 비어 있지 않아야 하며 ASCII letter/digit, `_`, `-`, `.`만 쓸 수 있다.
선언은 graph-global registry에 들어가므로 같은 id의 중복 선언은 오류다. ID vocabulary는
사용자 자유지만 declaration field는 위 두 개만 builtin이다.

## Test 정책

```lua
qstar.test "integration" {
  sources = {"tests/integration.c"},
  resources = {
    ["shared.slot"] = 1,
    gpu = 2,
  },
  retry = {
    count = 2,
    on = {"fail", "timeout"},
  },
  setup = qstar.cli {"tools/setup-fixture", "--fresh"},
  cleanup = qstar.cli {"tools/cleanup-fixture"},
  timeout = 30,
  manual = false,
}
```

`qstar.test`는 기존 artifact target field에 다음 builtin field를 추가한다.

| Field | Type | Default | 의미 |
| --- | --- | --- | --- |
| `resources` | `map<string, positive integer>` | `{}` | named resource별 요청량 |
| `retry` | table | disabled | retry 횟수와 retry 대상 상태 |
| `setup` | `qstar.cli { ... }` | none | 각 attempt의 test body 전에 실행 |
| `cleanup` | `qstar.cli { ... }` | none | 각 attempt 뒤 항상 실행 시도 |
| `timeout` | non-negative integer | 5 seconds at runtime | 각 setup/test/cleanup process 제한 |
| `manual` | boolean | `false` | 무필터 자동 test 선택에서 제외 |
| `skip` | `qstar.status("...")` | none | process를 실행하지 않고 skip 결과 기록 |

`resources`만 의도적으로 dynamic key map이다. Map key는 선언된 사용자 resource id이고
value는 요청량이다. 요청량이 0 이하이거나 resource capacity보다 크거나 id가 선언되지
않았으면 graph validation 오류다. 나머지 test policy table은 strict schema이며 오타난
field를 무시하지 않는다.

Retry table의 builtin field는 다음과 같다.

| Field | Type | Default |
| --- | --- | --- |
| `count` | integer 0..100 | `0` |
| `on` | list of `fail`, `error`, `timeout` | count > 0이면 세 상태 전체 |

`count`는 최초 실행 뒤 허용할 추가 attempt 수다. 예를 들어 `count = 2`면 최대 세 번
실행한다. `pass`와 `skip`은 retry 조건으로 사용할 수 없다.

## Scheduling 계약

`qstar test --jobs N`은 동시에 resource를 보유할 수 있는 test attempt 수의 상한이다.
Scheduler는 모든 resource request가 capacity 안에 들어오는 attempt만 시작한다. 여러
resource를 요청한 test는 전부를 한 번에 확보해야 시작한다.

Resource는 다음 전체 구간 동안 유지된다.

```text
acquire -> setup -> test body -> cleanup -> release
```

Retry할 때는 cleanup을 마친 뒤 resource를 먼저 반환하고 다음 attempt에서 다시
획득한다. 따라서 retry 대기 중에 다른 test가 resource를 사용할 수 있다. Cleanup은 setup
또는 test body가 실패하거나 timeout이어도 실행을 시도한다. Cleanup 자체가 실패하거나
timeout이면 최종 상태는 `error`다.

## 결과 상태

QStar result protocol의 고정 상태는 다섯 개다.

| Status | 의미 |
| --- | --- |
| `pass` | setup, test body, cleanup이 성공했고 body exit code가 0 |
| `fail` | test body가 non-zero exit code로 종료 |
| `skip` | `skip = qstar.status(...)` 또는 자동 선택에서 제외된 manual test |
| `error` | process start/signal/setup/cleanup failure 또는 cleanup timeout |
| `timeout` | setup 또는 test body가 선언된 timeout을 초과 |

이 상태 이름은 QStar builtin이다. 반대로 resource id와 suite tag vocabulary는 builtin이
아니다.

## Manual과 declarative skip

```lua
qstar.test "interactive" {
  sources = {"tests/interactive.c"},
  manual = true,
}

qstar.test "unavailable" {
  sources = {"tests/unavailable.c"},
  skip = qstar.status("Feature is not enabled in this configuration"),
}
```

- Filter 없는 `qstar test`는 manual test를 실행하지 않고 skip result를 기록한다.
- `qstar test --include-manual`은 무필터 선택에도 manual test를 포함한다.
- `qstar test //:interactive` 또는 explicit suite/tag selection은 manual test를 실행한다.
- Declarative `skip`은 explicit selection에서도 실행하지 않는다.
- `qstar.test_suite.manual`은 suite discovery 정책이고, `qstar.test.manual`은 leaf test 실행
  정책이다. 둘은 서로 다른 field contract다.

## CLI와 report

```sh
qstar test --jobs 8
qstar test --suite //:verification --jobs 4
qstar test --include-manual
qstar test --report-json build/results/tests.json
qstar test --output-junit build/results/junit.xml
```

JSON 경로를 생략하면 `<build_dir>/test-results.json`을 쓴다. `--report-json`과
`--output-junit` path는 package-relative여야 한다. Test failure가 있어 command가 non-zero로
끝나도 scheduler는 선택된 test들을 계속 수집하고 report를 쓴다.

JSON schema id는 `qstar-test-results-v1`이다. 각 record는 `label`, `status`, `attempts`,
`exit_code`, `action_id`, `stdout`, `stderr`, `skip_reason`을 제공한다. JUnit output은 같은
결과를 `<failure>`, `<error>`, `<skipped>`로 호환 표현한다.

## Action Log와 replay

각 attempt phase는 다음 action id를 가진다.

```text
<target-label>:test:setup:<attempt>
<target-label>:test:test:<attempt>
<target-label>:test:cleanup:<attempt>
```

예를 들어:

```sh
qstar action-log '//:integration:test:test:2'
qstar replay '//:integration:test:test:2'
```

Result record의 `action_id`, `stdout`, `stderr`는 test body를 가리킨다. Setup failure나
cleanup failure처럼 body보다 lifecycle failure가 최종 원인인 경우에는 해당 phase를
가리킨다. Stella와 Ninja는 같은 scheduler, 상태 모델, report schema, action id 형식을
사용한다.

## 금지 설계

- `qstar.board`, `qstar.gpu`, `qstar.serial_port` 같은 domain-specific declaration 추가
- resource id에 따라 자동 tool/runner/env를 선택하는 동작
- `hardware`, `emulator`, `interactive` 같은 문자열에 builtin evidence 의미 부여
- test output marker나 protocol을 QStar core가 해석하는 동작
- setup/cleanup을 shell command string으로 저장하는 동작

검증 gate는 `make qstar-test-resources-results-tests`다. Capacity exclusion, retry, timeout,
cleanup failure, manual/skip, JSON/JUnit, action-log/replay, Stella/Ninja parity를 함께 검사한다.
