# Composable Test Suites

`qstar.test_suite`는 새 executable이나 action을 만드는 target이 아니다. 기존
`qstar.test`, `qstar.run_target`, 다른 suite label을 이름 붙은 분류 집합으로 조합하는
Graph IR primitive다.

```lua
qstar.test_suite "host_units" {
  tests = {"//:scheduler_unit", "//:queue_unit"},
  tags = {"host", "fast"},
  description = qstar.status("Host-classified unit tests"),
}

qstar.test_suite "runtime_smoke" {
  tests = {"//:runtime_probe"},
  tags = {"simulator", "smoke"},
}

qstar.test_suite "verification" {
  tests = {"//:host_units", "//:runtime_smoke"},
}
```

## Field

| Field | Type | 의미 |
| --- | --- | --- |
| `tests` | `list<string>` | Existing test, run target, or nested suite labels. Required and non-empty. |
| `tags` | `list<string>` | Exact user metadata used by CLI selection. |
| `description` | `qstar.status("...")` | One-line description. |
| `manual` | boolean | Exclude from implicit tag root discovery. Default `false`. |

Unknown field, wrong type, duplicate member/tag, unknown label, unsupported target kind, suite
cycle은 모두 graph validation error다. Nested suite는 작성 순서대로 depth-first flatten하고
target label을 ordered unique list로 만든다.

## Tag는 builtin 의미가 아니다

`host`, `simulator`, `emulator`, `hardware`, `nightly` 같은 값은 project가 자유롭게 정한
문자열이다. QStar는 tag에서 platform, runner, compiler argv, timeout, evidence 등급을
추론하지 않는다. Simulator 실행 성공도 hardware 성공으로 자동 승격하지 않는다.

## CLI

```sh
qstar test --suite //:verification
qstar test --tag fast
qstar test --tag host --tag smoke
qstar test --suite //:verification --exclude-tag simulator
```

`--suite`, `--tag`, `--exclude-tag`는 반복할 수 있다. Explicit suite와 include tag는
합집합이고 include tag끼리는 OR다. Excluded suite는 nested 위치에서도 subtree가
제외된다. `manual = true` suite는 tag discovery에서는 빠지지만 explicit `--suite` 또는
다른 suite의 nested member로는 실행된다.

기존 `qstar test //:unit`과 filter 없는 `qstar test`는 바뀌지 않는다. Filter 없는 명령은
기존 `qstar.test` target만 실행하며 suite의 `run_target`을 암시적으로 추가하지 않는다.

## 관찰 표면

`list-targets --format json`의 suite record는 `tests`, `resolved_tests`를 제공한다. Target
record는 `direct_test_suites`, transitive `test_suites`를 제공한다. `query <label>
--format json`은 `qstar-query-v1`의 target 또는 test-suite record를 반환한다. Stella와
Ninja는 동일한 resolver 결과와 기존 member dependency closure를 사용한다.

필드별 전체 schema와 증거 해석 규칙은
[repository canonical reference](https://github.com/deeyed/qstar/blob/main/docs/composable-test-suites.md)에 있다.
