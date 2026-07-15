# Test Resources And Results

`qstar.test_resource`와 `qstar.test` policy는 parallel test scheduler가 공유 자원을
generic하게 제한하고 결과를 일관된 protocol로 수집하는 표면이다. `board`, `gpu`,
`serial-port` 같은 이름은 모두 사용자 resource id이며 QStar builtin 의미가 없다.

```lua
qstar.test_resource "shared.slot" {
  capacity = 1,
  description = qstar.status("Exclusive test slot"),
}

qstar.test "integration" {
  sources = {"tests/integration.c"},
  resources = { ["shared.slot"] = 1 },
  retry = { count = 2, on = {"fail", "timeout"} },
  setup = qstar.cli {"tools/setup-fixture"},
  cleanup = qstar.cli {"tools/cleanup-fixture"},
  timeout = 30,
  manual = false,
}
```

## Builtin field

`qstar.test_resource`는 required positive integer `capacity`와 optional status
`description`만 받는다. Resource id는 letter/digit/`_`/`-`/`.`로 구성된 사용자 정의
식별자다.

`qstar.test`의 test policy field:

| Field | Type |
| --- | --- |
| `resources` | resource-id -> positive integer map |
| `retry` | `{count = integer, on = list<fail/error/timeout>}` |
| `setup`, `cleanup` | `qstar.cli { ... }` |
| `timeout` | non-negative integer seconds |
| `manual` | boolean |
| `skip` | `qstar.status("...")` |

Resource는 setup, test body, cleanup 전체 동안 유지되며 retry 전에 반환된다. Cleanup은
setup/body failure나 timeout 뒤에도 실행을 시도한다.

고정 result status는 `pass`, `fail`, `skip`, `error`, `timeout`이다. Resource id와 suite
tag는 사용자 vocabulary지만 이 result status는 QStar protocol builtin이다.

```sh
qstar test --jobs 8
qstar test --include-manual
qstar test --report-json build/results/tests.json
qstar test --output-junit build/results/junit.xml
```

무필터 `qstar test`는 manual leaf를 skip으로 기록한다. Explicit target/suite/tag 선택은
manual leaf를 실행하며 declarative `skip`은 항상 skip이다. JSON 기본 path는
`<build_dir>/test-results.json`, schema는 `qstar-test-results-v1`이다. JUnit report는
선택적으로 생성한다.

각 phase action id는 `<label>:test:<setup|test|cleanup>:<attempt>` 형식이며 기존
`qstar action-log`과 `qstar replay`로 읽을 수 있다. Stella와 Ninja는 같은 scheduler와
result contract를 사용한다.

자세한 정본 계약과 금지 설계는
[repository documentation](https://github.com/deeyed/qstar/blob/main/docs/test-resources-and-results.md)에 있다.
