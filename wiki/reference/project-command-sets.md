# Reusable Project Command Sets

`qstar.command_spec`과 `qstar.command_set`은 큰 root `qstar.lua`의 command data를 QSM으로
분리하되 graph declaration 권한은 root에 남기는 표면이다.

## 핵심 경계

| API | 사용 위치 | Graph 변경 | 반환/효과 |
| --- | --- | --- | --- |
| `qstar.command` | root `qstar.lua` | yes | project command 하나를 직접 선언 |
| `qstar.command_spec` | root, `.qst`, ordinary `.qsm` | no | deeply immutable command specification 값 |
| `qstar.command_set` | root `qstar.lua` only | yes | spec list를 기존 project command IR로 materialize |

`qstar.command`는 기존 stable surface로 유지된다. `qstar.command_set`은 새 executor나 별도
CLI kind를 만들지 않으며, materialized command는 direct command와 같은 name, alias,
option, step, action-log, replay, cycle diagnostic, JSON 계약을 쓴다.

## Module

```lua
-- qstar/modules/commands/commands.qsm
local specs = {}

local common_options = {
  mode = qstar.param.enum {
    choices = {"debug", "release"},
    default = "debug",
  },
  verbose = qstar.param.bool {
    default = false,
  },
}

specs[#specs + 1] = qstar.command_spec "verify" {
  description = qstar.status("Verify the project"),
  aliases = {"v"},
  options = common_options,
  env = {"PROJECT_VERIFY=1"},
  working_dir = "tools",
  steps = {
    qstar.step.check("//..."),
    qstar.step.run {
      command = qstar.cli {
        "./verify.sh",
        qstar.param("mode"),
        qstar.arg_if(qstar.param("verbose"), "--verbose"),
      },
    },
  },
}

return specs
```

Root `qstar.lua`:

```lua
local commands = qstar.import_module("qstar/modules/commands")
qstar.command_set(commands)
```

Import만 하면 command가 등록되지 않는다. Root가 `qstar.command_set`을 호출한 경우에만
`qstar verify`와 `qstar v`가 public project command가 된다.

## Specification schema

`qstar.command_spec` field는 `qstar.command`와 같다.

| Field | Type |
| --- | --- |
| `description` | `qstar.status("...")` |
| `options` | option-name -> `qstar.param.*` |
| `env` | list<string> |
| `working_dir` | package-relative string |
| `steps` | list<`qstar.step.*`> |
| `is_default`, `hidden` | bool |
| `aliases` | list<string> |

Specification은 모든 nested table까지 read-only다. 같은 mutable `options`, `env`, `steps`
table을 여러 `qstar.command_spec` 호출에 넘길 수 있지만 각 호출은 independent immutable
snapshot을 만든다. Specification에는 deterministic string, boolean, integer, table만
허용하고 nesting은 64 level 미만이어야 한다.

## Set schema와 진단

`qstar.command_set`은 exactly one non-empty contiguous list를 받는다. 모든 item은
`qstar.command_spec`이 만든 immutable 값이어야 한다. Plain table은 거절한다.

```txt
qstar: qstar.command_set item 1 must be an immutable qstar.command_spec value
qstar: qstar.command_set is only allowed in root qstar.lua
qstar: qstar.command_set is forbidden inside .qsm module; project commands must be materialized only in root qstar.lua
```

여러 set과 direct command를 함께 사용할 수 있지만 다음 검증은 전체 materialized command
집합에 적용된다.

- duplicate command name
- command/alias collision
- reserved core CLI name
- duplicate default command
- unknown `qstar.step.call` destination
- direct/set 경계를 넘는 call cycle

`qstar commands --format json`은 선언 출처와 무관하게 기존
`{"schema":"qstar-commands-v1", ...}`를 출력한다. Specification provenance는 실행 의미가
아니므로 별도 JSON command kind를 추가하지 않는다.

## 금지되는 패턴

- `.qsm`에서 `qstar.command` 또는 `qstar.command_set` 호출
- module import side effect로 command 자동 등록
- target/config/stage를 만드는 callback을 spec에 저장
- plain helper table을 command spec으로 자동 추론
- 언어·플랫폼·도메인별 전용 command field나 step 추가

정본 구현 계약과 더 긴 예제는
[docs/reusable-project-command-sets.md](https://github.com/deeyed/qstar/blob/main/docs/reusable-project-command-sets.md)에 있다.
