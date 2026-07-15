# QStar Public Declaration Schema

이 문서는 QStar public Lua DSL의 declaration table 검증 계약을 정의한다. QStar는 public
declaration에서 알 수 없는 field나 잘못된 Lua type을 무시하지 않는다. 이 계약의 목적은
오타가 유효한 graph처럼 보이거나, 값이 기본값으로 조용히 대체되는 일을 막는 것이다.

## 진단 계약

Declaration schema 오류는 다음 정보를 모두 포함한다.

- Lua source file과 line
- public API 이름
- declaration label 또는 이름
- unknown field, 기대 type, 실제 type 같은 원인

예:

```text
qstar: qstar.lua:8: qstar.config declaration '//:warnings': unknown field 'soruces'
qstar: src/core/core.qst:12: qstar.objectlib declaration '//src/core:objects': field 'sources' must be list, got string
```

`list` field는 Lua table이기만 해서는 안 된다. Key는 1부터 시작하는 연속 integer여야 하고,
각 item도 field schema가 허용한 type이어야 한다. 따라서 아래 선언은 모두 error다.

```lua
qstar.executable "app" {
  sources = "src/main.c",
}

qstar.executable "app" {
  sources = {main = "src/main.c"},
}
```

## Type 이름

| 문서 type | Lua 값 |
| --- | --- |
| `string` | Lua string. Number의 암묵적 string 변환은 허용하지 않는다. |
| `boolean` | `true` 또는 `false`. |
| `integer` | Lua integer. 실수는 허용하지 않는다. |
| `table` | Named field를 가진 Lua table 또는 QStar helper token. |
| `list<T>` | 1부터 시작하는 연속 index와 `T` item을 가진 Lua table. |

## Project

`qstar.project` builtin field:

| Field | Type |
| --- | --- |
| `name` | string |
| `version` | string |
| `root` | string |
| `build_dir` | string |
| `generated_dir` | string |
| `compile_commands` | string |

## Toolset And Config

`qstar.toolset` builtin field:

| Field | Type |
| --- | --- |
| `tools` | table |
| `response_files` | string 또는 boolean |
| `response_style` | string 또는 boolean |
| `path_tools` | list<string> |
| `allow_absolute_tools` | string 또는 boolean |

`tools.archive`와 `tools.link`는 core role이다. Activated language provider namespace는
`tools.<namespace> = { ... }` 형태의 dynamic provider role table이다. Provider namespace가
아닌 임의의 top-level tool role은 허용되지 않는다.

`qstar.config` builtin field:

| Field | Type |
| --- | --- |
| `lang` | table |
| `libs`, `lib_dirs`, `link_options` | list<string> |
| `link` | table |
| `link_inputs` | list<string 또는 `qstar.target_file(...)`> |
| `toolset`, `artifact_name` | string |

`lang.c`, `lang.cxx`, `lang.asm`은 builtin schema를 사용한다. Activated external provider의
`lang.<namespace>` option은 provider manifest의 dynamic schema만 사용한다. QStar core가
external provider option 이름을 별도 allowlist로 하드코딩하지 않는다.

## Targets

Artifact target은 `qstar.target`, `qstar.executable`, `qstar.staticlib`,
`qstar.sharedlib`, `qstar.test`를 뜻한다.

| Artifact target field | Type |
| --- | --- |
| `kind` | string |
| `configs`, `deps`, `public_deps`, `private_deps`, `objects`, `visibility` | list<string> |
| `sources` | list<string 또는 provider source token> |
| `libs`, `lib_dirs`, `link_options` | list<string> |
| `link` | table |
| `link_inputs` | list<string 또는 `qstar.target_file(...)`> |
| `lang` | table |
| `toolset`, `artifact_name` | string |
| `compile_usage`, `link_usage` | `{options = list<string>, inputs = list<string 또는 qstar.target_file(...)>}` |

Typed dependency target은 별도 strict schema를 사용한다.

| Declaration | Builtin field |
| --- | --- |
| `qstar.interface` | `deps`, `public_deps`, `private_deps`, `visibility`, `compile_usage`, `link_usage` |
| `qstar.imported` | `artifact_kind`, `artifacts`, `deps`, `public_deps`, `private_deps`, `visibility`, `compile_usage`, `link_usage` |
| `qstar.tool` | `path`, `visibility` |

`compile_usage`와 `link_usage` 안에는 `options`, `inputs`만 허용한다.
`qstar.imported.artifacts`의 platform key는 `default`, `darwin`, `linux`, `windows`,
`generic`만 builtin이다. 각 artifact item의 builtin field는 `id`, `role`, `path`,
`primary`다. `artifact_kind` value, artifact `id`, `link`/`tool` 이외의 `role` value는
사용자 metadata이며 QStar가 compiler/linker flag를 추론하는 keyword가 아니다.
`role = "link"`, `role = "tool"`, `primary = true`만 artifact 소비 경로에 의미가 있다.

`qstar.objectlib` builtin field:

| Field | Type |
| --- | --- |
| `kind` | string |
| `configs`, `deps`, `public_deps`, `private_deps`, `visibility` | list<string> |
| `sources` | list<string 또는 provider source token> |
| `compile_context` | string (`"own"` 또는 `"consumer"`) |
| `lang` | table |
| `toolset` | string |

Object library는 final artifact가 없으므로 `objects`, `libs`, `lib_dirs`, `link`,
`link_options`, `link_inputs`, `artifact_name`을 받지 않는다.

`qstar.group` builtin field:

| Field | Type |
| --- | --- |
| `kind` | string |
| `deps`, `public_deps`, `private_deps`, `visibility` | list<string> |

`qstar.run_target` builtin field:

| Field | Type |
| --- | --- |
| `kind` | string |
| `deps`, `public_deps`, `private_deps`, `visibility` | list<string> |
| `toolset` | string |
| `inputs` | list<string 또는 artifact/stage token> |
| `command` | `qstar.cli { ... }` table |
| `description` | `qstar.status("...")` table |
| `timeout` | integer |
| `expect` | table |

`expect`에는 `contains` string과 optional `file` string만 허용한다.

## Generated Actions And Layouts

| Declaration | Builtin field와 type |
| --- | --- |
| `qstar.custom_target` | `inputs` list, `outputs` list, `command` CLI table, `description` status table, `toolset` string |
| `qstar.transform` | `input` string/token, `output` string/token, `command` CLI table, `description` status table, `toolset` string |
| `qstar.configure_file` | `output` string, `defines` list<string>, `description` status table |
| `qstar.stage` | `root` string, `files` list<`qstar.stage_file(...)`>, `description` status table |
| `qstar.target_family` | `allow_shared_sources` boolean, `variants` list<string>, `targets` list<string> |

`qstar.custom_target`과 `qstar.transform`은 같은 generated action contract로 낮아지지만,
single input/output sugar인 transform에는 plural `inputs`와 `outputs`가 없다.

## Project Commands

`qstar.command`과 pure `qstar.command_spec`의 builtin field:

| Field | Type |
| --- | --- |
| `description` | `qstar.status("...")` table |
| `options` | option-name to `qstar.param.*` table |
| `env`, `aliases` | list<string> |
| `working_dir` | string |
| `steps` | list<`qstar.step.*`> |
| `is_default`, `hidden` | boolean |

Command option helper field:

| Field | Type |
| --- | --- |
| `description` | string 또는 status table |
| `required` | boolean |
| `default` | option type에 맞는 typed literal |
| `choices` | list<string> |

Command step option은 step kind마다 분리된다.

| Step | Option field |
| --- | --- |
| `build`, `test`, `check`, `lint`, `call` | `when` param table |
| `stage` | `root` string, `dry_run` boolean, `when` param table |
| `export_stage` | `to` string/param table, `dry_run` boolean, `when` param table |
| `run` | `command` CLI table, `inputs` list, `env` list<string>, `working_dir` string, `description` status table, `timeout` integer, `expect` table, `when` param table |

Helper가 만든 step table을 나중에 수정해 unknown field를 추가해도 `qstar.command.steps`
lowering에서 다시 검사한다.

`qstar.command_spec`은 이 schema를 깊게 immutable한 deterministic table로 snapshot하며
graph declaration을 만들지 않는다. Root-only `qstar.command_set`은 non-empty contiguous
list 하나만 받고, 모든 item이 QStar가 만든 `qstar.command_spec`인지 검사한 뒤 기존
`qstar.command` Graph IR로 materialize한다. Plain table은 command specification으로
암묵 변환하지 않는다.

## Project Option And Variant

`qstar.option` builtin field:

| Field | Type |
| --- | --- |
| `type` | string |
| `value` | option type에 맞는 literal |
| `choices` | list<string> |
| `description` | string |

`qstar.variant` builtin field:

| Field | Type |
| --- | --- |
| `values` | table |
| `description` | string |
| `tags` | list<string> |

`qstar.variant.values` 안의 key는 QStar builtin이 아니다. `arch`, `triple`, `cpu`, `board`,
`mode` 같은 이름을 포함해 사용자가 자유롭게 정의한다. 값은 deterministic literal
string, boolean, integer, nested named table, list로 제한되지만 field 이름은 자유다.

## 검사하지 않는 일반 Lua Table

`qstar.import_module()`이 반환한 helper table은 public declaration이 아니다. QStar는 그
table의 임의 key를 declaration schema로 검사하지 않는다. 그 table이 나중에 public
declaration field로 들어갈 때만 해당 field contract가 적용된다.

예외적으로 helper가 `qstar.command_spec`을 만들면 그 specification은 생성 시점과 root
`qstar.command_set` materialization 시점에 command schema로 검사된다. Module export와
specification nested table은 read-only지만 module에 포함된 다른 일반 helper data가 public
declaration으로 자동 승격되는 것은 아니다.

Provider option도 같은 원칙을 따른다. `lang.<namespace>`는 activated provider가 선언한
dynamic option schema로 검사하며, provider에 없는 option은 unknown provider option error다.

## Regression Corpus

`tests/corpus/malformed-declarations`와 `tests/malformed-declarations.sh`는 unknown field,
잘못된 type, malformed list, objectlib forbidden field, command option/step mutation,
source location과 declaration label을 반복 검증한다.

```sh
make qstar-malformed-declaration-tests
```
