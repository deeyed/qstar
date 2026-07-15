# Typed Dependency Target Model

이 문서는 QStar의 artifact-free interface dependency, package-local prebuilt artifact,
executable tool dependency, compile/link usage requirement 계약을 정의한다. 이 표면은
특정 언어, compiler, operating system, package registry를 가정하지 않는다. QStar는
dependency graph와 explicit argv/input propagation만 책임지고, imported artifact에서
compiler/linker flag를 추론하지 않는다.

## Public Surface

Q275가 추가하는 public entrypoint와 helper는 다음 네 개다.

| Surface | 책임 |
| --- | --- |
| `qstar.interface` | 자기 artifact 없이 consumer에게 usage requirement와 public dependency closure를 제공한다. |
| `qstar.imported` | package root에 이미 존재하는 외부 artifact를 platform별 artifact map으로 선언한다. |
| `qstar.tool` | package-local executable file을 typed dependency target으로 선언한다. |
| `qstar.tool_file(label)` | `qstar.tool`, `role = "tool"` imported artifact, QStar executable/test target의 executable path를 argv에 넣고 producer edge를 만든다. |

Package resolver, registry, network fetch, compiler discovery는 이 모델에 포함되지 않는다.
외부 artifact와 tool은 project가 직접 source tree/vendor tree에 두거나 별도 상위 workflow가
준비해야 한다.

## Builtin과 사용자 정의 값

아래 표는 QStar core가 의미를 해석하는 이름과 사용자가 자유롭게 정하는 metadata를
구분한다.

| 이름 | 분류 | 의미 |
| --- | --- | --- |
| `compile_usage`, `link_usage` | QStar builtin field | consumer action에 전파할 명시적 usage requirement table이다. |
| `options`, `inputs` | QStar builtin nested field | 각각 argv item과 rebuild input을 뜻한다. |
| `artifact_kind`, `artifacts` | QStar builtin field | imported target의 설명 metadata와 platform artifact map이다. |
| `default`, `darwin`, `linux`, `windows`, `generic` | QStar builtin platform key | 현재 platform과 같은 key를 우선하고 없으면 `default`를 사용한다. |
| `id`, `role`, `path`, `primary` | QStar builtin artifact field | selector identity, consumer role, package path, 기본 artifact 여부다. |
| `artifact_kind`의 값 | 사용자 정의 metadata | `staticlib`, `sdk`, `runtime_bundle`처럼 자유롭게 적는다. QStar는 이 값으로 flag를 추론하지 않는다. |
| artifact `id` | 사용자 정의 identifier | `qstar.target_file(label, {artifact = "id"})` selector가 된다. 같은 platform list에서 unique해야 한다. |
| artifact `role` | 대부분 사용자 정의 identifier | `link`와 `tool`만 core가 소비 경로로 해석한다. 그 밖의 `runtime`, `debug`, `metadata` 등은 query 가능한 metadata다. |
| `compile_usage.options`, `link_usage.options` 값 | 사용자 명시 argv | QStar가 수정하거나 번역하지 않고 compile/link argv에 그대로 추가한다. |

각 platform artifact list는 1개에서 16개 artifact를 가지며 정확히 하나만
`primary = true`여야 한다. `role = "link"`와 `role = "tool"`은 각각 최대 하나다.

## Interface Dependency

```lua
qstar.interface "warnings_contract" {
  compile_usage = {
    options = {"-DPROJECT_ABI=3"},
    inputs = {"contracts/project-abi.txt"},
  },
  link_usage = {
    options = {"-pthread"},
    inputs = {"contracts/link-policy.txt"},
  },
  deps = {"//base:runtime_contract"},
  private_deps = {"//internal:validation_contract"},
  visibility = {"//..."},
}
```

`qstar.interface`의 builtin field는 다음과 같다.

| Field | Type | Required | 의미 |
| --- | --- | --- | --- |
| `deps` | list<label> | no | Public dependency. 현재 target consumer에도 적용되고 transitive consumer에도 계속 전파된다. |
| `public_deps` | list<label> | no | `deps`와 같은 public dependency alias다. |
| `private_deps` | list<label> | no | 현재 선언의 own action에는 적용되지만 그 target을 넘어 재전파되지 않는다. Interface는 own action이 없으므로 build/order closure에만 남는다. |
| `visibility` | list<string> | no | 기존 target visibility 계약과 같다. |
| `compile_usage` | table | no | Consumer compile action requirement다. |
| `link_usage` | table | no | Consumer link action requirement다. |

Interface target은 artifact와 action이 없다. 따라서 `qstar.target_file("//:iface")`는
error이고, Stella에서는 metadata node, Ninja에서는 dependency-only phony alias로만
표현된다.

## Usage Requirement

`compile_usage`와 `link_usage`는 같은 nested schema를 사용한다.

| Field | Type | 의미 |
| --- | --- | --- |
| `options` | list<string> | Consumer의 compile 또는 link argv에 순서대로 추가한다. |
| `inputs` | list<package-relative string 또는 `qstar.target_file(...)`> | argv에는 추가하지 않고 action key/rebuild dependency와 producer edge에만 추가한다. |

QStar는 option 문자열의 언어, flag family, target triple, sysroot 의미를 모른다.
예를 들어 `-D`, `/D`, `-pthread`, response-file option 중 무엇이 맞는지는 project가
선택한 toolset/config/platform branch의 책임이다.

전파 순서는 consumer에 직접 적힌 `deps`, `private_deps` 순서이며 각 dependency의
자기 usage를 먼저 받고 public `deps` closure를 depth-first로 따른다. 같은 option/input은
처음 나타난 항목만 유지한다. Dependency의 `private_deps`는 그 dependency를 넘어
전파되지 않는다.

Artifact target도 `compile_usage`와 `link_usage`를 선언할 수 있다. 따라서 static library가
자기 consumer에게 필요한 ABI define이나 runtime link option을 명시적으로 제공할 수 있다.
기존 `deps`, `public_deps`, `private_deps`, `libs`, `lib_dirs`, `link_options`,
`link_inputs`는 그대로 유지된다.

## Imported Artifact

```lua
qstar.imported "codec" {
  artifact_kind = "prebuilt_codec",
  artifacts = {
    default = {
      {
        id = "archive",
        role = "link",
        path = "vendor/codec/libcodec.a",
        primary = true,
      },
    },
    windows = {
      {
        id = "runtime",
        role = "runtime",
        path = "vendor/codec/bin/codec.dll",
        primary = true,
      },
      {
        id = "import_lib",
        role = "link",
        path = "vendor/codec/lib/codec.lib",
        primary = false,
      },
    },
  },
  compile_usage = {
    options = {"-DCODEC_ABI=4"},
    inputs = {"vendor/codec/include/codec.h"},
  },
  link_usage = {
    options = {"-pthread"},
  },
}
```

`qstar.imported`의 builtin field는 `artifact_kind`, `artifacts`, `deps`,
`public_deps`, `private_deps`, `visibility`, `compile_usage`, `link_usage`다.
현재 platform key가 있으면 그 list만 선택하고, 없으면 `default` list를 선택한다.
둘 다 없으면 graph validation error다.

`qstar.target_file("//:codec")`는 selected list의 primary artifact를 반환한다.
`qstar.target_file("//:codec", {artifact = "import_lib"})`는 selected list의 id selector를
사용한다. Link consumer는 selected list에서 `role = "link"` artifact를 사용하고,
그 role이 없을 때만 primary artifact를 사용한다.

중요하게도 `artifact_kind = "staticlib"`이나 filename suffix가 `.a`, `.lib`, `.so`라고
해서 QStar가 `-L`, `-l`, `/LIBPATH`, runtime path, framework, compiler define을 만들지
않는다. Link action에는 selected artifact path 자체와 명시적으로 작성한
`link_usage.options`만 들어간다. Cross compile argv도 이 선언 때문에 바뀌지 않는다.

## Executable Tool Dependency

미리 존재하는 package-local executable:

```lua
qstar.tool "schema_compiler" {
  path = "tools/schema-compiler",
  visibility = {"//..."},
}

qstar.custom_target "generated_schema" {
  inputs = {"schema/model.idl"},
  outputs = {qstar.output("generated/model.c")},
  command = qstar.cli {
    qstar.tool_file("//:schema_compiler"),
    qstar.input(0),
    qstar.output(0),
  },
}
```

QStar가 빌드하는 executable도 같은 helper를 쓴다.

```lua
qstar.executable "codegen" {
  sources = {"tools/codegen.c"},
}

qstar.custom_target "generated_table" {
  inputs = {"data/table.txt"},
  outputs = {qstar.output("generated/table.c")},
  command = qstar.cli {
    qstar.tool_file("//:codegen"),
    qstar.input(0),
    qstar.output(0),
  },
}
```

두 번째 예제에서 `codegen` executable build는 generated action의 first-class producer
dependency다. Stella scheduler와 Ninja edge 모두 codegen을 먼저 빌드하고, executable
path를 generated command의 argv[0]으로 사용하며 action input/cache material에도 포함한다.
QStar는 그 executable이 build host에서 실행 가능한지 target metadata로 추론하지 않는다.
Cross project는 host tool target과 target artifact toolset/config를 명시적으로 분리해야 한다.

`qstar.tool_file`이 허용하는 target은 다음과 같다.

- `qstar.tool`
- selected artifact map에 `role = "tool"`이 있는 `qstar.imported`
- `qstar.executable`
- `qstar.test`

다른 target kind는 executable tool artifact가 없다는 diagnostic으로 거부한다.

## Backend와 관찰 표면

Stella와 Ninja는 같은 계약을 사용한다.

- Compile action: effective `compile_usage.options`를 argv에 추가하고
  `compile_usage.inputs` producer/file edge를 action input으로 추적한다.
- Link action: effective `link_usage.options`를 argv에 추가하고
  `link_usage.inputs` producer/file edge를 action input으로 추적한다.
- Imported dependency: link role artifact를 implementation dependency로 사용한다.
- Interface/tool/imported target: 자체 compiler/linker action을 생성하지 않는다.
- Tool token: target producer closure, executable path resolution, cache/action input을
  동일하게 처리한다.

`qstar query`, `qstar explain`, `qstar dry-run`, `qstar list-targets --format json`은
direct usage, effective usage, selected artifact map, imported declaration metadata,
tool path를 보여준다. JSON은 `compile_usage`, `link_usage`,
`effective_compile_usage`, `effective_link_usage`, `artifact_kind`, `tool_path`,
`imported_artifacts`, `artifacts` field를 제공한다.

## 금지 설계

- Imported filename이나 `artifact_kind`에서 compiler/linker flag를 추론하지 않는다.
- `arch`, `triple`, `cpu`, `board`, language runtime 이름을 typed dependency builtin field로
  추가하지 않는다.
- Download URL, registry name, package version resolver, credential, network fetch를
  `qstar.imported`에 넣지 않는다.
- Shell command string을 tool dependency로 저장하지 않는다. Executable path와 argv-vector를
  분리한다.
- Usage input을 argv에 몰래 넣지 않는다. `options`와 `inputs`의 역할을 섞지 않는다.

## Regression Gate

```sh
make qstar-typed-dependency-target-tests
```

이 gate는 interface public/private propagation, platform primary/link artifact 선택,
prebuilt static archive, static/shared/executable consumer, package-local tool,
QStar-built executable tool, generated usage input producer, Stella/Ninja parity,
query/explain/list-targets JSON, malformed declaration diagnostic을 검증한다.
