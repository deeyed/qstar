# 재사용 가능한 Root Project Command Specification

Status: Q276 구현 계약. 이 문서는 `qstar.command_spec`과
`qstar.command_set`의 정본 authoring 규칙이다.

## 1. 목적과 권한 경계

큰 프로젝트는 build, verify, package, simulator, export 같은 project command가 많아지면서
root `qstar.lua`가 길어질 수 있다. 그렇다고 `.qsm` helper에 `qstar.command` 선언 권한을
주면 graph declaration이 여러 module에 숨어 root entrypoint가 더 이상 전체 public CLI를
통제하지 못한다.

QStar는 이 문제를 specification과 materialization으로 분리한다.

- `qstar.command_spec "name" { ... }`는 graph를 변경하지 않는 immutable Lua 값이다.
- `.qsm`은 command specification list만 만들어 반환할 수 있다.
- root `qstar.lua`의 `qstar.command_set(specs)`만 specification을 실제 project command로
  materialize한다.
- 기존 `qstar.command "name" { ... }`는 stable surface로 그대로 유지한다.
- `.qsm` 안의 target/config/stage/command 같은 graph declaration 금지도 그대로 유지한다.

따라서 command 정의의 반복은 module로 옮길 수 있지만, 어떤 command set을 public CLI로
노출할지는 root entrypoint가 계속 결정한다.

## 2. 권장 프로젝트 구조

```txt
qstar.lua
qstar/
  modules/
    commands/
      commands.qsm
src/
  ...
tools/
  verify.sh
```

`qstar/modules/commands/commands.qsm`:

```lua
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

local common_env = {
  "PROJECT_VERIFY=1",
}

specs[#specs + 1] = qstar.command_spec "verify" {
  description = qstar.status("Verify the project"),
  aliases = {"v"},
  options = common_options,
  env = common_env,
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

specs[#specs + 1] = qstar.command_spec "check-graph" {
  aliases = {"cg"},
  steps = {
    qstar.step.check("//..."),
  },
}

specs[#specs + 1] = qstar.command_spec "verify-all" {
  steps = {
    qstar.step.call("cg"),
  },
}

return specs
```

Root `qstar.lua`:

```lua
qstar.project {
  name = "example",
  root = ".",
}

local commands = qstar.import_module("qstar/modules/commands")
qstar.command_set(commands)

qstar.subdir("src")
```

사용자 CLI는 declaration 방식과 무관하다.

```sh
qstar commands
qstar commands --format json
qstar verify --mode release --verbose
qstar v --mode debug
```

## 3. `qstar.command_spec`

형식:

```lua
local spec = qstar.command_spec "name" {
  -- qstar.command와 같은 field
}
```

괄호형도 동일하다.

```lua
local spec = qstar.command_spec("name", {
  steps = {qstar.step.check("//...")},
})
```

허용 field는 `qstar.command`와 완전히 같다.

| Field | Type | 의미 |
| --- | --- | --- |
| `description` | `qstar.status("...")` | command list/help 설명 |
| `options` | option-name -> `qstar.param.*` | typed runtime option schema |
| `env` | list<string> | command-level environment overlay |
| `working_dir` | string | package-relative run working directory |
| `steps` | list<`qstar.step.*`> | ordered generic command steps |
| `is_default` | bool | default project command 지정 |
| `hidden` | bool | 목록에서는 숨기고 직접 호출은 허용 |
| `aliases` | list<string> | 대체 CLI 이름 |

`qstar.command_spec`은 graph target이나 project command를 등록하지 않는다. 그래서 일반
`.qsm` 안에서 호출해도 된다. 반환값은 다음 속성을 가진다.

- specification 전체와 모든 nested table은 깊게 read-only다.
- caller는 `options`, `aliases`, `env`, `steps`, CLI token table을 변경할 수 없다.
- string, boolean, integer, table로 이루어진 deterministic data만 담을 수 있다.
- nesting은 64 level 미만이어야 한다.
- `name`은 read-only specification metadata로 노출된다.

다음 변경은 error다.

```lua
local specs = qstar.import_module("qstar/modules/commands")
specs[1].options.mode.default = "release"
-- qstar: qstar.command_spec is read-only: default
```

같은 plain option/step table을 여러 spec 생성에 재사용할 수 있다. 각
`qstar.command_spec` 호출이 immutable snapshot을 만들기 때문에 이후 다른 spec과
상태를 공유하지 않는다.

## 4. `qstar.command_set`

형식:

```lua
qstar.command_set {
  qstar.command_spec "check-local" {
    steps = {qstar.step.check("//...")},
  },
  qstar.command_spec "lint-local" {
    steps = {qstar.step.lint("//...")},
  },
}
```

또는 cached module export를 직접 넘긴다.

```lua
qstar.command_set(qstar.import_module("qstar/modules/commands"))
```

계약:

- root `qstar.lua`에서만 호출할 수 있다.
- 인자는 비어 있지 않은 contiguous list 하나여야 한다.
- 모든 item은 QStar가 만든 immutable `qstar.command_spec`이어야 한다.
- plain table이나 임의의 `name`/kind tag를 specification으로 받아들이지 않는다.
- 여러 command set을 materialize할 수 있지만 전체 command name/alias는 서로 유일해야 한다.
- direct `qstar.command`와 command set 사이에도 같은 충돌 규칙이 적용된다.
- `qstar.step.call`은 direct/set 구분 없이 command 또는 alias를 찾고 전체 graph에서 cycle을
  검사한다.
- default command는 direct/set 전체에서 하나만 허용된다.

`.qst`나 `.qsm`에서 materialize하면 다음과 같이 거절된다.

```txt
qstar: qstar.command_set is only allowed in root qstar.lua
qstar: qstar.command_set is forbidden inside .qsm module; project commands must be materialized only in root qstar.lua
```

## 5. Materialization 이후의 동일성

`qstar.command_set`은 별도 executor나 별도 command kind를 만들지 않는다. 각 spec은 기존
`qstar.command` Graph IR로 낮아진다. 따라서 다음 항목은 direct command와 동일하다.

- command name과 alias CLI dispatch
- typed option parsing과 bool helper
- command-level/step-level env merge 및 redaction
- package-relative working directory
- `qstar.step.*` execution
- action-log/replay/timeout/expect
- reserved name, duplicate name, alias collision, call cycle 진단
- `qstar commands` text output
- `qstar commands --format json`의 `qstar-commands-v1` schema

JSON에는 command가 direct declaration에서 왔는지 specification set에서 왔는지를 나타내는
별도 semantic field가 없다. Materialization 출처는 실행 의미가 아니며, 사용자 CLI와
도구가 두 선언 방식을 구분할 이유가 없기 때문이다.

## 6. 금지 설계

다음 설계는 Q276 계약에 포함되지 않는다.

- `.qsm` 안에서 `qstar.command` 또는 `qstar.command_set` 호출
- QSM import만으로 command를 암묵적으로 등록하는 side effect
- command specification이 target/config/stage를 선언하는 callback 보유
- shell command string, 언어·플랫폼·도메인별 전용 command kind나 step 추가
- command set이 package resolver, network fetch, plugin discovery를 수행
- plain Lua table을 trusted specification으로 자동 승격

Command module은 immutable data를 만들고, root는 materialization 권한을 행사하며, 실제
workflow는 기존 generic `qstar.step.*`만 사용한다.
