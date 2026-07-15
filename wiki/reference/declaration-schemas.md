# Public Declaration Schemas

QStar public Lua declaration table은 strict schema를 사용한다. Unknown field, 잘못된 Lua
type, named key나 sparse index가 섞인 list는 error다. QStar가 값을 무시하거나 기본값으로
조용히 대체하지 않는다.

```text
qstar: qstar.lua:8: qstar.config declaration '//:warnings': unknown field 'soruces'
qstar: src/core/core.qst:12: qstar.objectlib declaration '//src/core:objects': field 'sources' must be list, got string
```

모든 schema diagnostic에는 source file/line, API 이름, declaration label, 원인이 들어간다.

## Builtin Declaration Fields

| Declaration | Builtin field |
| --- | --- |
| `qstar.project` | `name`, `version`, `root`, `build_dir`, `generated_dir`, `compile_commands` string |
| `qstar.toolset` | `tools` table, response policy string/bool, `path_tools` list, absolute-tool policy string/bool |
| `qstar.config` | `lang`, link table, library/link lists, `toolset`, `artifact_name` |
| Artifact target | configs/sources/dependency/object/link/lang/toolset/artifact fields |
| `qstar.objectlib` | configs/sources/dependencies/visibility/lang/toolset와 `compile_context` |
| `qstar.interface` | dependencies, visibility, compile_usage, link_usage |
| `qstar.imported` | artifact_kind, platform artifacts, dependencies, visibility, compile_usage, link_usage |
| `qstar.tool` | path, visibility |
| `qstar.group` | dependencies와 visibility |
| `qstar.run_target` | dependencies, toolset, inputs, command, description, timeout, expect |
| `qstar.custom_target` | inputs, outputs, command, description, toolset |
| `qstar.transform` | singular input/output, command, description, toolset |
| `qstar.configure_file` | output, defines, description |
| `qstar.stage` | root, files, description |
| `qstar.target_family` | allow_shared_sources, variants, targets |
| `qstar.command`, `qstar.command_spec` | description, options, env, working_dir, steps, is_default, hidden, aliases |
| `qstar.option` | type, value, choices, description |
| `qstar.variant` | values, description, tags |

정확한 field별 type과 list item type은
[repository schema reference](https://github.com/deeyed/qstar/blob/main/docs/public-declaration-schemas.md)에 있다.

## Separate Target Schemas

Artifact target, objectlib, group, run target은 같은 target table allowlist를 공유하지 않는다.

- Objectlib는 link/library/artifact field를 받지 않는다.
- Group은 dependency와 visibility만 받는다.
- Run target은 command/input/expect field를 받지만 compile/link field를 받지 않는다.
- Artifact target은 compile/link/artifact field를 받지만 run command field를 받지 않는다.
- Typed dependency target은 각자 interface/imported/tool 전용 schema를 사용한다.
  `compile_usage`/`link_usage` nested table에는 `options`, `inputs`만 허용한다.

따라서 의미 없이 저장되지만 lowering에서 쓰이지 않는 field가 남지 않는다.

## Lists

`sources`, `deps`, `files`, `steps` 같은 list field는 1부터 시작하는 연속 integer key를
사용한다.

```lua
qstar.executable "app" {
  sources = {"src/main.c"},
}
```

아래 map은 list가 아니므로 error다.

```lua
qstar.executable "app" {
  sources = {main = "src/main.c"},
}
```

## Dynamic And Free Metadata

다음 표면은 고정 QStar field allowlist와 구분한다.

- `qstar.variant.values`: 사용자가 자유롭게 이름을 정하는 deterministic metadata.
- `lang.<provider>`: activated provider manifest의 dynamic option schema.
- `qstar.import_module()` return table: 일반 Lua helper table이므로 검사하지 않음.
- `qstar.command_spec`: 일반 module table 안에서 만들 수 있지만 command schema로 검사되고
  nested table까지 immutable snapshot이 된다. Root-only `qstar.command_set`은 non-empty
  contiguous spec list만 materialize하며 plain table은 받지 않는다.
- `qstar.imported.artifact_kind` value와 artifact id, `link`/`tool` 이외 role:
  strict field 안에 저장되는 사용자 정의 metadata. Filename이나 metadata에서 flag를
  추론하지 않음.

`arch`, `triple`, `cpu`, `board`, `mode`는 `variant.values` 안에서 사용자 metadata일 뿐
QStar builtin keyword가 아니다.

## Command Steps

`qstar.step.*` helper option도 kind별 schema를 사용한다. Helper가 만든 table을 수정해
unknown field를 추가한 경우 `qstar.command.steps` lowering이 다시 검사한다. `when`은
`qstar.param("name")`, `timeout`은 integer, `dry_run`은 boolean이어야 한다.

## Regression Gate

```sh
make qstar-malformed-declaration-tests
```

이 gate는 모든 declaration family의 negative diagnostic과 `variant.values`, module helper
table의 positive boundary를 함께 검사한다.
