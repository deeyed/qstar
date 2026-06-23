# QStar 설정 가능 빌드 표면

상태: 구현 기준 문서. 이 문서는 project build option, 사용자 정의 variant,
objectlib, 계층적 fragment authoring을 QStar가 어떤 문법으로 구현해야 하는지 정의한다.
목표는 domain-specific profile을 되살리는 것이 아니라, CMake/Meson/Xmake 수준의 범용
빌드시스템에서 자연스러운 설정 표면을 QStar 방식으로 정리하는 것이다.

이 문서는 다음 구현 라운드의 정본이다. 여기의 문법은 QStar가 정확히 구현해야 할
사용자-facing surface를 정의한다. 구현 전에는 기존 runtime reference와 구분해서 읽어야
하지만, 구현 후에는 `wiki/reference/qstar-lua.md`와 manpage의 stable DSL 표면으로
승격한다.

## 핵심 원칙

QStar는 project graph, option evaluation, artifact dependency, command workflow,
stage/layout, source/provider lowering을 맡는다. QStar core는 특정 언어, 특정 OS,
특정 toolchain, 특정 board, 특정 image format, 특정 software domain을 builtin
의미론으로 알면 안 된다.

허용되는 설계:

- project가 자기 vocabulary로 build option을 정의한다.
- project가 자기 vocabulary로 variant metadata를 정의한다.
- Lua `if`가 option과 metadata를 읽고 explicit compiler/linker argv를 만든다.
- GLP provider가 자기 namespace 안에서 언어별 option schema와 source unit을 정의한다.
- object artifact를 만들 수 있는 source/provider/action을 generic object collection으로
  묶는다.

금지되는 설계:

- `arch`, `triple`, `cpu`, `board`, `mode`, `runtime`처럼 project마다 의미가 달라질 수
  있는 vocabulary를 QStar builtin field로 해석하는 것.
- target triple, sysroot, resource dir, stdlib, standalone runtime flag를 QStar가 자동 argv로
  주입하는 것.
- C/C++/ASM만 사용할 수 있는 top-level 문법을 추가하는 것.
- 외부 언어를 GLP가 아닌 QStar core 전용 special case로 넣는 것.
- shell variable처럼 보이는 `$ARCH`, `$TRIPLE` 같은 QStar-only mini-language를
  Lua 문법 위에 덧씌우는 것.

## `qstar.option`

`qstar.option`은 graph evaluation 전에 CLI `-D` 값으로 override할 수 있는 project build
option이다. CMake/Meson의 build option과 같은 역할을 하지만, QStar는 이 값을 자동으로
compiler flag로 해석하지 않는다.

```lua
local ARCH <const> = qstar.option "arch" {
  type = "combo",
  choices = {"host", "armv7m", "aarch64"},
  value = "host",
  description = "Project architecture selection",
}
```

`qstar.option`은 호출 결과로 실제 Lua 값을 반환한다. 따라서 일반 Lua `if`로 분기한다.

```lua
if ARCH == "armv7m" then
  qstar.config "arch_flags" {
    lang = {
      c = {
        compile_options = {
          "-mcpu=cortex-m7",
        },
      },
    },
  }
end
```

CLI는 CMake/Meson 사용자에게 익숙한 `-D`를 쓴다.

```sh
qstar -D arch=armv7m build //:app
qstar -Darch=armv7m check //...
```

규칙:

- `-D name=value`와 `-Dname=value`를 모두 허용한다.
- 선언되지 않은 option 이름은 error다.
- 같은 option이 여러 번 들어오면 마지막 값이 이긴다.
- 최종 option 값은 graph/cache identity에 포함된다.
- `-D`는 QStar project option만 설정한다. 환경변수를 설정하거나 source file 내부의
  문자열을 치환하지 않는다.

QStar builtin field:

| 필드 | 필수 | 의미 |
| --- | --- | --- |
| `type` | yes | Option type. |
| `value` | yes | CLI override 전 기본값. Meson 용어를 따른다. |
| `choices` | `combo`에서만 yes | `combo`가 허용하는 값 목록. |
| `description` | no | 진단과 future option listing에 쓰는 설명. |

지원하는 `type`:

| Type | Lua value | CLI value |
| --- | --- | --- |
| `string` | string | 그대로의 문자열 |
| `boolean` | boolean | `true` 또는 `false` |
| `integer` | integer | base-10 integer |
| `combo` | string | `choices` 중 하나 |
| `list` | list of strings | comma-separated string list |

`qstar.option`에서는 `default`를 쓰지 않는다. 기본값 field 이름은 `value`다. Meson의
option vocabulary와 맞추고 같은 개념에 두 단어가 생기는 것을 피하기 위해서다. 반대로
`qstar.command` runtime parameter는 이미 별도 표면이므로 기존 `default` field를 유지한다.

CLI option 이름은 lowercase와 hyphen/underscore 중심으로 안정적으로 둔다. Lua local은
project configuration constant처럼 보이도록 대문자 이름을 권장한다.

```lua
local TARGET_TRIPLE <const> = qstar.option "target-triple" {
  type = "combo",
  choices = {"host", "arm-none-eabi"},
  value = "host",
}
```

`<const>`는 선택 사항이지만 project constant에는 권장한다. 이것은 Lua 5.4 문법이므로
QStar가 Lua parser를 fork할 필요가 없다. `$ARCH`, `$TRIPLE`, `<option>` 같은 QStar 전용
표기는 금지 방향이다.

## `qstar.variant`

`qstar.variant`는 이름 있는 read-only 사용자 metadata table이다. Profile이 아니다.
Toolchain을 선택하지 않고, argv를 주입하지 않으며, QStar가 아는 platform model을 만들지
않는다.

```lua
local HOST_SIM <const> = qstar.variant "host_sim" {
  description = "Host simulation variant",
  tags = {"host", "debug"},
  values = {
    execution_mode = "host",
    platform_name = "native",
  },
}

local DEVICE_A <const> = qstar.variant "device_a" {
  values = {
    arch = "armv7m",
    triple = "arm-none-eabi",
    cpu = "cortex-m7",
    board = "vendor-board-a",
  },
}
```

`arch`, `triple`, `cpu`, `board`, `execution_mode`, `platform_name`은 전부
`values` 안의 사용자 metadata key다. QStar field가 아니다. QStar는 이 이름들을 모르며
의미를 부여해서는 안 된다.

QStar builtin field:

| 필드 | 필수 | 의미 |
| --- | --- | --- |
| name argument | yes | `qstar.variant "name"`에서 오는 variant id. |
| `values` | yes | 자유로운 deterministic user metadata. |
| `description` | no | 사람이 읽는 설명. |
| `tags` | no | Project tooling과 future listing에 쓰는 free-form string label. |

Top-level free-form key는 거부한다. 사용자 metadata는 반드시 `values` 아래에 둔다. 그래야
`arch = "..."`나 `board = "..."`가 QStar builtin schema처럼 보이지 않는다.

`values`에 들어갈 수 있는 값은 deterministic literal data여야 한다. string, boolean,
integer, list, 같은 종류의 nested table은 허용한다. function, userdata, thread,
graph declaration은 metadata가 아니다.

## Custom Triple

Custom triple은 project가 정의한 option 값일 뿐이다. Target triple처럼 생겼더라도 QStar는
parse하지 않는다.

```lua
local PROJECT_TRIPLE <const> = qstar.option "project-triple" {
  type = "combo",
  choices = {
    "host",
    "sample-armv7m-none",
    "sample-aarch64-none",
  },
  value = "host",
  description = "Project-defined build triple",
}

if PROJECT_TRIPLE == "sample-armv7m-none" then
  qstar.config "triple_flags" {
    lang = {
      c = {
        compile_options = {
          "--target=arm-none-eabi",
          "-mcpu=cortex-m7",
          "-mthumb",
        },
      },
      asm = {
        compile_options = {
          "--target=arm-none-eabi",
          "-mcpu=cortex-m7",
          "-mthumb",
        },
      },
    },
    link_options = {
      "--target=arm-none-eabi",
    },
  }
else
  qstar.config "triple_flags" {}
end
```

CLI:

```sh
qstar -D project-triple=sample-armv7m-none build //:app
```

이 구조는 old profile-era behavior와 의도적으로 다르다. Option 값은 project가 직접 argv로
작성할 때만 compiler/linker argv가 된다.

## `qstar.objectlib`

`qstar.objectlib`는 first-class object collection target이다. Object file을 compile하거나
기여하지만 archive, shared library, executable을 만들지 않는다. Leaf fragment가 source
ownership을 갖고, 상위 fragment나 root target이 조합을 결정할 수 있게 하기 위한 표면이다.

```lua
qstar.objectlib "core_objects" {
  compile_context = "own",
  configs = {"//qstar/policies:common"},
  sources = {
    "./core.c",
    "./clock.c",
  },
}
```

Consumer 예:

```lua
qstar.executable "app" {
  configs = {"//qstar/policies:app_flags"},
  sources = {
    "./main.c",
  },
  objects = {
    "//src/core:core_objects",
    "//src/drivers:driver_objects",
  },
}
```

QStar builtin field:

| 필드 | 필수 | 의미 |
| --- | --- | --- |
| `sources` | yes | Source string, generated object token, provider source token. |
| `compile_context` | no | `"own"` 또는 `"consumer"`. 기본값은 `"own"`. |
| `configs` | no | Reusable config label. |
| `deps`, `public_deps`, `private_deps` | no | Artifact target과 같은 visibility vocabulary를 쓰는 dependency/usage edge. |
| `lang` | no | Target-local `lang.<namespace>` option table. |
| `toolset` | no | `"own"` context에서 쓰는 explicit toolset label. |
| `visibility` | no | Label visibility policy. |

금지 field:

- `libs`
- `lib_dirs`
- `link`
- `link_options`
- `link_inputs`
- `artifact_name`
- `command`
- `outputs`
- `timeout`
- `expect`

`qstar.objectlib`에는 final link/archive action이 없고 primary artifact도 없다.
따라서 `qstar.target_file("//:core_objects")`는 error다. 소비자는 `target_file`이 아니라
`objects` field를 쓴다.

`compile_context` 값은 정확히 두 개다.

| 값 | 의미 |
| --- | --- |
| `"own"` | Objectlib가 자기 configs, target-local lang option, toolset으로 한 번 compile된다. CMake `OBJECT` library에 가장 가깝다. |
| `"consumer"` | Objectlib source가 각 consuming target의 effective compile context 안에서 compile된다. Leaf fragment가 source list를 소유하고 upper target이 platform/config를 소유할 때 쓴다. |

기본값은 `"own"`이다. 숨은 재compile을 줄이고 일반 object library 기대와 더 잘 맞기
때문이다. CMake식 계층적 source ownership과 upper-level platform selection을 원하면
`compile_context = "consumer"`를 명시한다.

`"consumer"`는 C/C++ 전용이라는 뜻이 아니다. Built-in C/C++/ASM provider나 활성화된 외부
GLP provider가 object artifact를 emit할 수 있다면 같은 model을 사용해야 한다.

`qstar.objectlib`는 object artifact 개념이지 C 전용 문법이 아니다. 반드시 다음을 받을 수
있어야 한다.

- built-in `lang.c`, `lang.cxx`, `lang.asm` source
- `"./main.zig"` 같은 활성화된 GLP raw source string
- `zig.object("./main.zig")` 같은 explicit provider source token
- `qstar.output(path, {format = "object"})`로 선언된 generated object output

QStar core는 `cflags`, `asmflags`, `domain_sources`, `platform_sources` 같은 field를
추가하면 안 된다. Language policy는 계속 `lang.<namespace>`와 provider helper 아래에 둔다.

## 계층적 fragment

QStar는 source를 helper module 안에 숨기지 않고도 CMake처럼 계층적 책임 구조를 표현할 수
있어야 한다.

```lua
-- qstar.lua
qstar.subdir("src")
qstar.subdir("targets")
qstar.subdir("tests")
```

```lua
-- src/src.qst
qstar.subdir("core")
qstar.subdir("drivers")
qstar.subdir("app")
```

```lua
-- src/core/core.qst
qstar.objectlib "core_objects" {
  compile_context = "consumer",
  sources = {
    "./core.c",
    "./time.c",
  },
}
```

`src/src.qst`에서 `qstar.subdir("core")`를 호출하면 `src/core/core.qst`를 읽는다.
Root `qstar.lua`에서 `qstar.subdir("src")`를 호출하면 `src/src.qst`를 읽는다.

Path resolution:

| Path form | 의미 |
| --- | --- |
| `"src/core/core.c"` | Package-root-relative path. |
| `"./core.c"` | Current authoring file directory relative path. |
| `"../x.c"` | Package root를 벗어나면 거부한다. Package 안으로 normalize되더라도 권장하지 않는다. |

이 규칙은 기존 package-root model을 유지하면서 leaf fragment의 긴 root-relative path 문제를
줄인다.

권장 구조:

```text
qstar.lua
qstar/
  policies/
    common.qst
src/
  src.qst
  core/
    core.qst
  drivers/
    drivers.qst
  app/
    app.qst
targets/
  targets.qst
tests/
  tests.qst
```

Root는 project metadata와 큰 orchestration을 맡는다. Leaf fragment는 source list를 소유한다.
Mid-level fragment는 leaf objectlib나 target label을 조합한다. `qstar/modules` 아래 helper
module은 pure table/function을 반환할 수 있지만 실제 source declaration을 숨기면 안 된다.

## CMake/Meson 대응

| QStar surface | CMake/Meson analogue | QStar boundary |
| --- | --- | --- |
| `qstar.option` + `-D` | CMake cache variable / Meson option | QStar는 값을 검증해 반환하고, project가 argv로 바꾼다. |
| `qstar.variant` | Project-authored toolchain/cross metadata | Metadata만 저장하고 key를 해석하지 않는다. |
| Option 위의 Lua `if` | CMake `if()` / Meson `if` | 별도 condition DSL이 아니라 ordinary Lua branch다. |
| `qstar.objectlib compile_context = "own"` | CMake `OBJECT` library | 자기 compile context를 가진 object collection. |
| `qstar.objectlib compile_context = "consumer"` | CMake interface source / Meson source-set-like ownership | Leaf는 source를 소유하고 consumer가 effective compile context를 소유한다. |
| Nested `qstar.subdir` | CMake `add_subdirectory()` / Meson `subdir()` | 명시적 fragment hierarchy다. Implicit recursive globbing이 아니다. |
| `"./path"` in fragment | Current source dir relative path | 명시적 fragment-relative path syntax다. |

## 구현 체크리스트

- Graph-evaluation option registry를 추가한다.
- Lua evaluation 전에 global `-D name=value`와 `-Dname=value`를 parse한다.
- Builtin field validation을 가진 `qstar.option`을 추가한다.
- `values` free metadata와 top-level free-form key rejection을 가진 `qstar.variant`를 추가한다.
- `qstar.objectlib`를 추가한다.
- Artifact target에 `objects` consumer field를 추가한다.
- `compile_context = "own" | "consumer"` lowering을 추가한다.
- Objectlib source classification이 built-in source registry와 GLP source registry를 모두
  통과하게 한다.
- Source/input/output/import/subdir 등 적용 가능한 context에서 explicit `./`
  fragment-relative path resolution을 추가한다.
- Nested `qstar.subdir`가 current authoring fragment 기준으로 resolve되게 한다.
- 구현 후 docs/wiki/man/help/LSP/formatter/tests를 갱신한다.
- Profile-era field와 domain-specific builtin vocabulary가 재등장하지 않도록 drift guard를
  추가한다.
