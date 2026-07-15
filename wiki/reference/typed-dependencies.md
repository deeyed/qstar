# Typed Dependencies

QStar는 내부 build target label뿐 아니라 artifact 없는 interface dependency,
package-local prebuilt artifact, executable tool dependency를 first-class target으로
표현한다. 이 기능은 package manager나 특정 언어/toolchain 문법이 아니다.

## Interface

```lua
qstar.interface "api_contract" {
  compile_usage = {
    options = {"-DAPI_LEVEL=3"},
    inputs = {"contracts/api.txt"},
  },
  link_usage = {
    options = {"-pthread"},
  },
  deps = {"//base:contract"},
  private_deps = {"//internal:checks"},
  visibility = {"//..."},
}
```

`qstar.interface`는 artifact와 실행 action이 없다. `deps`/`public_deps` usage는
transitive하게 전파되고, `private_deps` usage는 interface consumer에게 전파되지 않는다.
일반 artifact target의 own action은 자기 direct private dependency usage를 받는다.

## Usage

`compile_usage`와 `link_usage`는 `options`와 `inputs`만 받는다.

| Field | 의미 |
| --- | --- |
| `options` | Consumer compile/link argv에 그대로 추가하는 string list. |
| `inputs` | argv에는 넣지 않고 rebuild/action producer dependency로 추적하는 package file 또는 `qstar.target_file(...)` list. |

QStar는 flag를 번역하거나 추론하지 않는다. Cross target, sysroot, runtime option은
project가 toolset/config/platform branch에서 명시한다.

## Imported

```lua
qstar.imported "codec" {
  artifact_kind = "prebuilt_codec",
  artifacts = {
    default = {
      {id = "archive", role = "link", path = "vendor/libcodec.a", primary = true},
    },
    windows = {
      {id = "runtime", role = "runtime", path = "vendor/codec.dll", primary = true},
      {id = "import_lib", role = "link", path = "vendor/codec.lib", primary = false},
    },
  },
  compile_usage = {options = {"-DCODEC_ABI=4"}},
}
```

Platform key는 `default`, `darwin`, `linux`, `windows`, `generic`이다. 현재 platform
entry가 우선이고, 없으면 `default`를 쓴다. 각 list는 정확히 하나의 primary를 가진다.
`qstar.target_file("//:codec")`는 primary를, named selector는 해당 id를 반환한다.
Link consumer는 `role = "link"` artifact를 사용한다.

`artifact_kind`, artifact `id`, 대부분의 `role` 값은 사용자 metadata다.
단, `link`와 `tool` role은 QStar가 소비 경로로 해석한다. Imported target은 suffix나
metadata에서 `-l`, `-L`, `/LIBPATH` 같은 option을 만들지 않는다.

## Tool

```lua
qstar.tool "generator" {
  path = "tools/generator",
}

qstar.custom_target "generated" {
  inputs = {"schema/input.txt"},
  outputs = {qstar.output("generated/output.c")},
  command = qstar.cli {
    qstar.tool_file("//:generator"),
    qstar.input(0),
    qstar.output(0),
  },
}
```

`qstar.tool_file`은 `qstar.tool`, `role = "tool"` imported artifact,
`qstar.executable`, `qstar.test`를 받을 수 있다. QStar-built executable을 가리키면
Stella/Ninja가 그 executable을 generated action보다 먼저 빌드한다.

## Builtin 경계

- Builtin field: `compile_usage`, `link_usage`, `options`, `inputs`,
  `artifact_kind`, `artifacts`, `id`, `role`, `path`, `primary`.
- Fixed platform key: `default`, `darwin`, `linux`, `windows`, `generic`.
- User metadata: `artifact_kind` value, artifact id, `link`/`tool` 이외 role name.
- Core semantic role: `link`, `tool`, primary selector.
- Out of scope: registry, fetch, package resolver, network, compiler flag inference.

상세 schema와 backend 전파 순서는
[repository typed dependency contract](https://github.com/deeyed/qstar/blob/main/docs/typed-dependency-targets.md)에 있다.
