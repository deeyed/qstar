# Configurable Build Surface

이 문서는 `qstar.option`, `qstar.variant`, `qstar.objectlib`, fragment-relative
path, nested `qstar.subdir`를 구현하기 위한 사용자-facing 정본이다. 아직 runtime에 모두
들어간 문법이 아니라, 다음 구현 라운드가 따라야 하는 spec이다. 구현이 끝나면 이 내용은
`reference/qstar-lua.md`, manpage, help, LSP reference로 승격된다.

## 원칙

QStar core는 project graph, option evaluation, artifact dependency, command workflow,
stage/layout, GLP source/provider lowering을 맡는다. 특정 언어, 특정 OS, 특정 toolchain,
특정 board, 특정 image format, 특정 software domain은 QStar builtin으로 넣지 않는다.

따라서 다음 단어들은 QStar builtin field가 아니다.

```txt
arch, triple, cpu, board, mode, runtime, project-specific image names,
project-specific runner names, project-specific platform names
```

Project가 이런 단어를 쓰고 싶다면 `qstar.option` 이름이나 `qstar.variant.values` 안의
사용자 metadata key로만 쓴다. QStar는 이 이름들을 보고 compiler/linker argv를 자동 생성하지
않는다.

## `qstar.option`

`qstar.option`은 CLI `-D`로 override할 수 있는 project build option이다.

```lua
local ARCH <const> = qstar.option "arch" {
  type = "combo",
  choices = {"host", "armv7m", "aarch64"},
  value = "host",
  description = "Project architecture selection",
}
```

CLI:

```sh
qstar -D arch=armv7m build //:app
qstar -Darch=armv7m check //...
```

규칙:

- `-D name=value`와 `-Dname=value`를 모두 허용한다.
- 선언되지 않은 option 이름은 error다.
- 같은 option이 여러 번 들어오면 duplicate override error다.
- 최종 option 값은 graph/cache identity에 포함된다.

QStar builtin field:

| 필드 | 필수 | 의미 |
| --- | --- | --- |
| `type` | yes | `string`, `boolean`, `integer`, `combo`, `list`. |
| `value` | yes | 기본값. Meson vocabulary를 따른다. |
| `choices` | only for `combo` | 허용되는 값. |
| `description` | no | 사람이 읽는 설명. |

`qstar.option`은 실제 Lua 값을 반환한다.

```lua
if ARCH == "armv7m" then
  qstar.config "arch_flags" {
    lang = {
      c = {
        compile_options = {"-mcpu=cortex-m7"},
      },
    },
  }
end
```

Option local은 대문자 `local ARCH <const>`처럼 쓰는 것을 권장한다. `<const>`는 Lua 5.4
문법이므로 안전하다. `$ARCH`나 `<option>` 같은 QStar 전용 mini-language는 도입하지 않는다.

## `qstar.variant`

`qstar.variant`는 named read-only metadata table이다. Profile이 아니며 toolchain이나
cross compile policy를 자동 선택하지 않는다.

```lua
local DEVICE_A <const> = qstar.variant "device_a" {
  description = "Example device-like project variant",
  tags = {"example"},
  values = {
    arch = "armv7m",
    triple = "arm-none-eabi",
    cpu = "cortex-m7",
    board = "vendor-board-a",
  },
}
```

`arch`, `triple`, `cpu`, `board`는 전부 사용자 metadata다. QStar builtin이 아니다.

QStar builtin field:

| 필드 | 필수 | 의미 |
| --- | --- | --- |
| name argument | yes | Variant id. |
| `values` | yes | Free-form deterministic user metadata. |
| `description` | no | 사람이 읽는 설명. |
| `tags` | no | Free-form string label. |

Top-level free-form key는 금지한다. 사용자 metadata는 반드시 `values` 안에 둔다.

```lua
-- bad: arch가 builtin처럼 보인다.
qstar.variant "bad" {
  arch = "armv7m",
}

-- good: arch는 사용자 metadata다.
qstar.variant "good" {
  values = {
    arch = "armv7m",
  },
}
```

## Custom Triple

Custom triple은 project-defined option value다. QStar는 값을 parse하지 않는다.

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

```sh
qstar -D project-triple=sample-armv7m-none build //:app
```

## `qstar.objectlib`

`qstar.objectlib`는 object file collection target이다. Archive, shared library, executable을
만들지 않는다. Leaf fragment가 source ownership을 갖고, 상위 fragment나 최종 target이
조합을 맡는 구조를 위해 추가한다.

```lua
qstar.objectlib "core_objects" {
  compile_context = "own",
  sources = {
    "./core.c",
    "./clock.c",
  },
}

qstar.executable "app" {
  configs = {"//qstar/policies:app_flags"},
  sources = {
    "./main.c",
  },
  objects = {
    "//src/core:core_objects",
  },
}
```

QStar builtin field:

| 필드 | 필수 | 의미 |
| --- | --- | --- |
| `sources` | yes | Source strings, generated object tokens, or provider source tokens. |
| `compile_context` | no | `"own"` 또는 `"consumer"`. 기본값은 `"own"`. |
| `configs` | no | Reusable config labels. |
| `deps`, `public_deps`, `private_deps` | no | Dependency/usage edges. |
| `lang` | no | Target-local `lang.<namespace>` tables. |
| `toolset` | no | Explicit toolset label for `"own"` context. `"consumer"` context source는 consuming target의 effective toolset을 쓴다. |
| `visibility` | no | Label visibility policy. |

`compile_context` 값:

| 값 | 의미 |
| --- | --- |
| `"own"` | Objectlib가 자기 context로 한 번 compile된다. |
| `"consumer"` | Objectlib는 source ownership만 갖고, consuming artifact target의 effective configs/lang/toolset으로 source가 per-consumer object로 compile된다. |

`qstar.objectlib`는 generic하다. Built-in C/C++/ASM source, 활성화된 GLP raw source
string, `"own"` context의 explicit provider source token, generated object output을
처리해야 한다. Provider helper가 이미 concrete action/output을 담는 explicit source token은
현재 consumer-context 재-lowering 대상이 아니므로 raw source string이나 `"own"` context를
사용한다.
`cflags` 같은 C-specific field나 `domain_sources` 같은 domain-specific field를 늘리면
안 된다.

`qstar.objectlib`에는 primary artifact가 없다. `qstar.target_file("//:core_objects")`는
error다. Consuming target은 `objects = {...}`를 쓴다.

## 계층적 fragment

Nested `qstar.subdir`는 CMake/Meson-style hierarchical project처럼 동작해야 한다.

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

`qstar.subdir("core")` inside `src/src.qst` resolves `src/core/core.qst`.

Path rule:

| Path form | 의미 |
| --- | --- |
| `"src/core/core.c"` | Package-root-relative path. |
| `"./core.c"` | Current authoring file directory relative path. |
| `"../x.c"` | Rejected if it escapes package root; discouraged otherwise. |

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
targets/
  targets.qst
tests/
  tests.qst
```

Root owns project metadata and coarse orchestration. Leaf fragments own source
lists. Mid-level fragments compose objectlibs or target labels. `.qsm` helper
modules may return pure helper tables but should not hide actual source
registration.

## CMake/Meson 대응

| QStar | CMake/Meson 대응 | 경계 |
| --- | --- | --- |
| `qstar.option` + `-D` | CMake cache variable / Meson option | QStar validates value; project turns it into argv. |
| `qstar.variant` | Project-authored toolchain/cross metadata | Metadata only, no builtin semantics. |
| Lua `if` | CMake/Meson condition | Ordinary Lua branch. |
| `qstar.objectlib` `"own"` | CMake `OBJECT` library | Own compile context. |
| `qstar.objectlib` `"consumer"` | Interface/source-set-like ownership | Consumer compile context. |
| Nested `qstar.subdir` | `add_subdirectory()` / `subdir()` | Explicit fragment hierarchy. |
| `"./path"` | Current source dir relative path | Explicit fragment-relative path. |
