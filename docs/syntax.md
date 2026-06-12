# QStar Syntax

QStar 파일은 deterministic Lua subset으로 작성한다. 이 subset은 장기적으로 Calua가 validate, execute, compile할 수 있어야 한다.

```txt
qstar.lua / <dirname>.qst
  -> Calua/QStar profile
  -> build graph declaration
  -> canonical Graph IR
```

QStar 파일은 arbitrary Lua script가 아니다. 파일 평가 결과는 deterministic해야 하고, 같은 package graph와 같은 profile input에서 같은 Graph IR를 만들어야 한다.

## Allowed Lua Subset

허용 syntax는 다음 중심으로 둔다.

- `local`
- `function`
- `return`
- `if`, `elseif`, `else`
- 제한된 `for ... in ipairs(...)`
- 제한된 `for ... in pairs(...)`
- table literal
- string, number, boolean, nil
- pure helper function
- `qstar.*` API call

Round 63 seals `local function` as official authoring surface. Helper functions
must be pure graph-construction helpers: they may build tables, iterate with
`ipairs`/`pairs`, call `table.insert`, use `string.*`, return option tables, and
call deterministic `qstar.*` helpers. They must not mutate globals or inspect
the host through unrestricted Lua APIs.

Round 1 executes this surface with a sandboxed Lua 5.4 evaluator in the
standalone `qstar` developer binary. Round 2 keeps the same syntax surface and
adds closure-aware `qstar explain` output. Only the `qstar.*` API, QStar
constants, and a narrow base/table/string subset are available.

Round 3 adds CLI-supplied package alias and profile inputs. This is intentionally
outside `qstar.lua`: package resolution and profile selection remain package
manager/profile responsibilities, not arbitrary Lua logic.

예:

```lua
local function platform_sources()
    return qstar.select {
        [qstar.os.macos] = {"src/platform/darwin.cale"},
        [qstar.os.linux] = {"src/platform/linux.cale"},
        [qstar.os.windows] = {"src/platform/windows.cale"},
        default = qstar.incompatible("unsupported platform"),
    }
end
```

함수는 허용하지만 pure helper여야 한다. Target 선언, table 조립, 조건 객체 조합, label list 생성 같은 build graph 계산만 수행한다.

## QStar Constants

Round 63 adds CMake-like deterministic constants for authoring files. They are
read-only inside QStar chunks; assigning to any global name is an error.

Global constants:

| Constant | Meaning |
| --- | --- |
| `QSTAR_VERSION` | QStar authoring/runtime version string |
| `QSTAR_VERSION_MAJOR` | major version integer |
| `QSTAR_VERSION_MINOR` | minor version integer |
| `QSTAR_VERSION_PATCH` | patch version integer |
| `QSTAR_HOST_OS` | host OS such as `macos`, `linux`, `windows`, or `unknown` |
| `QSTAR_HOST_ARCH` | host arch such as `aarch64`, `x86_64`, `riscv64`, or `unknown` |
| `QSTAR_PACKAGE_ROOT` | resolved package root containing `qstar.lua` |
| `QSTAR_PROJECT_ROOT` | resolved project root; v1 equals package root |
| `QSTAR_PROFILE` | active profile name, defaulting to `default` |
| `QSTAR_TARGET` | active target triple or `host` |

Namespace constants:

| Constant | Meaning |
| --- | --- |
| `qstar.version` | same as `QSTAR_VERSION` |
| `qstar.host.os` | same as `QSTAR_HOST_OS` |
| `qstar.host.arch` | same as `QSTAR_HOST_ARCH` |
| `qstar.project.root` | resolved project/package root |

예:

```lua
local function common_c()
    local opts = {}
    for _, flag in ipairs({"-Wall", "-Wextra"}) do
        table.insert(opts, flag)
    end
    table.insert(opts, "-DQSTAR_VERSION=" .. qstar.version)
    table.insert(opts, "-DQSTAR_TARGET=" .. QSTAR_TARGET)
    return {
        public_include_dirs = {"include"},
        compile_options = opts,
    }
end
```

## Forbidden Lua Features

다음은 금지한다.

- arbitrary `io.*`
- arbitrary `os.*`
- `debug.*`
- unrestricted `package.*`
- process spawn
- network access
- current time access
- random access
- dynamic loading
- global mutation or global assignment
- `load`, `loadfile`, `dofile`
- unrestricted `require`
- metatable trickery
- `rawget`, `rawset`
- unrestricted `coroutine`

Round 1 rejects these with a stable `qstar: forbidden Lua API` diagnostic where
the name is installed in the sandbox. Missing forbidden globals remain invalid
QStar code.

나쁜 예:

```lua
local sdk = io.popen("xcrun --show-sdk-path"):read("*l")
```

SDK discovery, tool discovery, env lookup은 QStar core와 Cale host/profile resolver가 명시 API로 제공해야 한다.

## Official Style

공식 style은 `qstar.*` prefix를 유지한다.

```lua
qstar.target "app" {
    kind = "exe",
}
```

과도한 alias는 비권장이다.

```lua
local target = qstar.target
target "app" { kind = "exe" } -- style guide에서는 비권장
```

QStar는 Makefile의 `$@`, `$<` 같은 암호 기호 문화와 CMake식 global mutable state를 피한다.

## qstar.subdir

Subdir QStar fragment를 graph에 추가한다.

```lua
qstar.subdir("src/render")
qstar.subdir("src/asset")
```

탐색 순서:

```txt
src/render/render.qst
```

`qstar.subdir`는 CMake `add_subdirectory()`처럼 mutable state를 공유하지 않는다. 해당
fragment를 평가해 target declaration을 graph에 추가하는 pure graph import다.
`qstar.qst` fallback은 없고, root discovery는 가장 가까운 상위 `qstar.lua`만 본다.

## qstar query / check / explain / dry-run

Standalone QStar는 다음 진단 surface를 가진다.

```txt
qstar --file qstar.lua --dump-graph
qstar --file qstar.lua list-targets
qstar --file qstar.lua query //:app
qstar --file qstar.lua doctor
qstar --file qstar.lua check //:app
qstar --file qstar.lua explain //:app
qstar --file qstar.lua dry-run //:app
qstar --file qstar.lua build //:app
qstar --file qstar.lua --diagnostics json check //:app
qstar --file qstar.lua --package-alias @core=/workspace/core explain //:app
qstar --file qstar.lua --profile debug --target arm64-apple-macos explain //:app
```

`--dump-graph`는 raw Graph IR를 출력한다. `list-targets`와 `query`는 authoring
navigation용 view를 출력한다. `doctor`는 package 전체 authoring 상태를 검사한다.
`check`는 선택 target의 dependency closure와 package-root file existence를 검증한다.
`explain`은 선택 target의 dependency closure를 계산하고, dependency-first order와
command plan을 출력한다. unknown dependency, unresolved external package dependency,
dependency cycle은 stable diagnostic으로 실패한다. 이 단계들은 실제 compile/link
실행이 아니다.

`--package-alias`는 external label을 설명 가능한 dependency로 만들지만, 해당
package의 QStar fragment를 읽지는 않는다. `--profile`, `--target`,
`--toolchain`, `--stdlib`은 command-plan header에 보존되는 skeleton input이다.

Round 4부터 `qstar explain`은 header graph policy도 검증한다. Round 5 기준으로
이 정책은 build-system 파일 정책일 뿐이다. Public header는 `include/` 아래의
package-relative file path여야 하고, private header는 package-relative file path여야
한다. QStar는 `.hcl`, `.h`, `.hpp` 같은 확장자를 해석하지 않는다. `.hcl`의
전처리, export filter, C/Cale declaration import/export는 Cale compiler/HCL checker가
맡는다.

Round 5부터 `qstar explain`은 Build Plan IR action key skeleton도 출력한다. 이 값은
future cache/Ninja/internal executor의 입력 재료이며, 실제 process 실행이나 final
hash 계산은 아직 하지 않는다.

Round 6부터 `sources`는 explicit source discovery skeleton으로 분류된다. Round 47
기준으로 인식되는 suffix는 `.c`, `.cl`, `.cale`, `.h`, `.s`, `.S`다. QStar는 이를 `c`,
`cale`, `cale`, `header`, `asm`, `asm-cpp` language로 표시한다. Round 50부터 `.s`와
`.S`도 host/clang compiler driver를 통해 object compile input으로 허용된다. `.h`는
header list로 옮기라는 stable diagnostic을 낸다.

Round 7부터 `qstar.custom_target`과 `qstar.output(path)`를 사용할 수 있다. Generated
output은 effective `qstar.project.generated_dir` 아래 package-relative path여야 한다.
기본값은 `generated`다. Generated source로 쓰이면 정확히 하나의 producer가 있어야
한다. QStar는 generated edge를 explain하지만 generator를 실행하지 않는다.

Round 8부터 `qstar dry-run`을 사용할 수 있다. `dry-run`은 `explain`과 같은 graph
validation, target closure, generated edge, source classification, resolver skeleton을
사용하지만 출력은 future executor가 소비할 수 있는 `dry_run_step` record 중심이다.
모든 step은 `execute=no`이며, QStar는 여전히 process를 실행하지 않는다.

```txt
qstar --file qstar/tests/manual/hello/qstar.lua dry-run //:app
```

Round 9부터 `qstar check`를 사용할 수 있다. 이 command는 authoring gate다.
`sources`, `public_headers`, `private_headers`, `qstar.custom_target`의 plain file
`inputs`가 package root 아래 실제 파일로 존재해야 한다.
`qstar.output("generated/foo.c")`로 표시된 generated output은 producer가 있으면 아직
존재하지 않아도 된다. Round 68부터 `qstar.custom_target`의 `inputs`는
`qstar.target_file("//:label")`도 받을 수 있으며, 이 경우 해당 label의 artifact를 먼저
빌드하는 graph edge로 취급한다.

```txt
qstar --file qstar/tests/manual/hello/qstar.lua check //:app
```

Round 10부터 diagnostic origin과 query UX가 들어간다. `qstar query //:app`은 target
kind, source/header/dependency, declaration origin을 보여준다. Round 15 기준
`--diagnostics json`은 LSP/editor가 소비할 수 있는 JSON diagnostic skeleton을
출력한다. 기존 `--diagnostic-format line`은 호환 alias로만 남긴다.

Round 11부터 `qstar.files`는 package root 기준 glob/exclude를 확장한다.

```lua
qstar.staticlib "core" {
    sources = qstar.files {
        "src/*.c",
        exclude = {
            "src/skip.c",
        },
    },
}
```

Round 11부터 `qstar.select`는 placeholder가 아니라 실제 branch를 반환한다.

```lua
qstar.executable "app" {
    sources = qstar.select {
        [qstar.os.macos] = {"src/darwin.c"},
        [qstar.os.linux] = {"src/linux.c"},
        default = {"src/portable.c"},
    },
}
```

Round 66부터 QStar는 profile 파일을 읽지 않는다. Profile은 `qstar.lua` 안의
`qstar.profile` DSL로 선언한다. Round 66 기준 profile surface는 host application,
freestanding kernel, firmware command planning에 필요한 최소 key를 포함한다.

```lua
qstar.profile "kernel" {
  target = "aarch64-none-elf",
  toolchain = "clang",
  stdlib = "none",
  arch = "aarch64",
  cpu = "cortex-a76",
  abi = "lp64",
  freestanding = true,
  cc = "clang",
  cxx = "clang++",
  ar = "llvm-ar",
  linker = "ld.lld",
  sysroot = "sdk",
  resource_dir = "resource",
  include_dirs = {"profile/include"},
  lib_dirs = {"profile/lib"},
  link_options = {"-nostdlib"},
  linker_script = "linker/kernel.ld",
  defsyms = {"__kernel_base=0x80000"},
  artifact_names = {"//:boot=BOOTX64.EFI"},
  path_tools = {"llvm-objcopy"},
  tool_overrides = {"llvm-objcopy=tools/fake-objcopy.sh"},
  allow_absolute_tools = false,
  response_files = "auto",
  response_style = "posix",
}
```

CLI `--target`, `--toolchain`, `--stdlib`은 `qstar.profile` 선언보다 우선한다.
현재 resolver 이름은 `profile-schema-v2`이며 toolchain profile은 `host`, `clang`,
`cale`만 정의되어 있다.

`freestanding = true`는 C/C++/ASM compile action에 보수적 freestanding compile
option을 추가한다. `arch`, `cpu`, `abi`는 target triple에서 모호한 policy를 분리하기
위한 profile hint다. `linker_script`, `link_options`, `defsyms`는 link argv에
반영되며, package-relative linker script는 link action input으로 추적된다.
`path_tools`는 `qstar.custom_target`에서 허용할 bare PATH tool allowlist다.
`tool_overrides`는 `NAME=VALUE` 형식으로 build file의 command spelling은 유지하면서
profile별 실행 tool을 바꾼다. `allow_absolute_tools`는 absolute command path를 여는
명시 capability이며 기본값은 false다.
`artifact_names`는 `LABEL=FILENAME` 또는 `target-name=FILENAME` list다. 예를 들어
UEFI x64 profile은 `//:boot=BOOTX64.EFI`, AArch64 profile은
`//:boot=BOOTAA64.EFI`를 줄 수 있다. 값은 파일명 basename이어야 하며
`EFI/BOOT/...` 같은 staging path는 별도 install/package rule에서 표현한다.

Round 13부터 `qstar build`가 들어간다. v1 executor는 package-local generated
tool, C source compile, static archive, exe link를 실행한다. `dry-run`은 계속
process를 실행하지 않는 argv plan 출력이고, `build`만 `build/qstar/out`과
`build/qstar/logs`에 파일을 만든다.

Round 14/15부터 build executor는 반복 빌드용 state를 가진다.

```txt
qstar --file qstar.lua build //:app
qstar --file qstar.lua build //:app --explain-cache
qstar --file qstar.lua why-rebuild //:app
qstar --file qstar.lua log //:app
qstar --file qstar.lua last-failure
qstar --file qstar.lua clean --target //:app
qstar --file qstar.lua clean
```

`build`는 `build/qstar/state/actions.json`에 action key를 기록하고, key와 output이
그대로면 `status=skip`을 출력한다. `why-rebuild`는 같은 key 계산을 실행 없이
보여준다. `clean`은 전체 `build/qstar` 상태 또는 선택 target output을 지운다.
`--diagnostics json`은 editor/LSP 준비용 JSON diagnostic skeleton이다.

Round 16부터 `.cale` source는 `toolchain = "cale"` 또는 `cale-sol`에서
object-producing compile action으로 들어간다. 이것은 Cale compiler process를
호출하는 build-system 통합이며, Cale frontend/backend 내부 API 연결이 아니다.
`host`/`clang` toolchain에서 `.cale` source를 빌드하면 stable diagnostic이다.

```lua
qstar.executable "mixed" {
    toolchain = "cale",
    sources = {"src/main.c", "src/tool.cale"},
}
```

Round 17부터 generated source/header chaining과 config header generation을 쓸 수
있다.

```lua
qstar.custom_target "version_c" {
    outputs = {qstar.output("generated/version.c")},
    command = qstar.cli {
        "tools/gen-version.sh",
        qstar.output(0),
    },
}

qstar.configure_file "config" {
    output = qstar.output("generated/config.h"),
    defines = {"APP_VERSION=7", "HAVE_FAST_PATH"},
}

qstar.executable "app" {
    sources = {"src/main.c", qstar.output("generated/version.c")},
    lang = {
        c = {
            private_headers = {qstar.output("generated/config.h")},
            include_dirs = {"generated"},
        },
    },
}
```

`qstar.configure_file`의 `defines` 항목은
`NAME=VALUE` 또는 `NAME` 형식을 받아 deterministic C header를 만든다. Generated
output은 effective `qstar.project.generated_dir` 아래에 있어야 하며, 같은 output을 두
action이 생산하면 stable diagnostic이다. `generated_dir`의 기본값은 `generated`다.

Round 18부터 link surface가 조금 넓어진다.

```lua
qstar.staticlib "core" {
    sources = {"src/core.c"},
    lang = {
        c = {
            public_headers = {"include/core.h"},
            public_include_dirs = {"include"},
            private_include_dirs = {"src/core/private"},
        },
    },
}

qstar.executable "app" {
    sources = {"src/main.c"},
    deps = {"//:core"},
    libs = {"m"},
    lib_dirs = {"third_party/lib"},
    frameworks = {"Foundation"}, -- Darwin-like target only
}
```

`deps`와 `public_deps`는 public include propagation을 허용한다. `private_deps`는
build/link에는 참여하지만 consumer include path에는 영향을 주지 않는다.
`sharedlib` target은 graph/dry-run에는 나타나지만 Round 18 local executor에서는
stable unsupported다.

Round 19부터 test/install skeleton이 들어간다.

```lua
qstar.test "unit" {
    sources = {"tests/unit.c"},
}
```

```txt
qstar --file qstar.lua test //:unit
qstar --file qstar.lua test //...
qstar --file qstar.lua install //:app --prefix /tmp/pkg --dry-run
qstar --file qstar.lua install //:app --prefix /tmp/pkg
qstar --file qstar.lua stage //:esp --dry-run
qstar --file qstar.lua stage //:esp
```

`qstar test`는 test executable을 먼저 build하고 실행한다. stdout/stderr는
`build/qstar/logs`에 보존된다. `qstar install`은 build된 exe/staticlib artifact와 public
header만 prefix로 복사한다. package fetch, registry metadata, sharedlib install은
아직 범위 밖이다.

Round 56부터 `qstar.run_target`은 stdout, stderr, 선택적 `marker_log` 파일에서 marker를
찾는다. QEMU나 emulator wrapper는 `qstar.cli { ... }`로 표현하고, serial output을
package-relative log file로 쓰면 `marker_log = "serial.log"`로 검증할 수 있다. 실패는
`marker-missing`, `timeout`, `exit-code`로 분리되며 `qstar last-failure`와
`qstar replay <action-id>`가 재현 command를 출력한다.

Round 59부터 build failure diagnostic은 사람이 읽는 line과 함께
`qstar-action-diagnostic-v1` JSON record를 남긴다. Systems/firmware flow에서 자주
섞이는 실패 단계는 stable `failure_kind`로 분리한다.

- `link-failure`: final link 또는 PE/COFF link command 실패
- `objcopy-failure`: `llvm-objcopy`류 artifact transform 실패
- `package-failure`: `qstar stage`/copy-only package 단계 실패
- `qemu-timeout`: QEMU/emulator wrapper `run_target` timeout

`last-failure`와 `replay`는 같은 `failure_kind`, action id, owner label, stdout/stderr
log path를 공유하므로 editor, CI, 외부 도구가 같은 실패 원인을 소비할 수 있다.

`qstar stage`는 install과 별개인 copy-only staging/package command다. ESP, RPi boot
directory, firmware package처럼 실행 layout이 중요한 산출물을 package root 아래에
배치한다. Dry-run은 copy하지 않고 manifest와 diff만 남긴다.

## qstar.target

가장 일반적인 target 선언이다.

```lua
qstar.target "engine" {
    kind = "staticlib",
    lang = {
        cale = {
            modules = qstar.modules {
                root = "src",
                include = {
                    "render",
                    "asset",
                },
            },
            public_headers = {
                "include/engine/engine.hcl",
            },
            public_include_dirs = {
                "include",
            },
        },
    },

    deps = {
        "//src/render:render",
        "//src/asset:asset",
    },

    toolchain = "host",
    stdlib = "system",
}
```

주요 field:

| Field | 의미 |
| --- | --- |
| `kind` | 산출물 종류 |
| `sources` | target-specific source, C source, generated source |
| `lang` | language-specific header/include/compile/module namespace |
| `deps` | target dependency label list |
| `toolchain` | host/freestanding/target profile reference |
| `stdlib` | `system`, `cale`, `none` |
| `linker_script` | freestanding/kernel linker script |
| `artifact_name` | target-local output filename override such as `BOOTX64.EFI` |
| `c` | C language mode 같은 build-relevant compile mode |
| `cale` | Cale edition 같은 build-relevant compile mode |

Secure profile과 UB category override는 여기 넣지 않는다. Secure, audit, safety
policy는 compiler/package policy layer가 담당하고 QStar core DSL에는 넣지 않는다.

## Convenience Target APIs

다음 wrapper는 내부적으로 `qstar.target`으로 낮춘다.

```lua
qstar.executable "app" {
    lang = {
        cale = {
            modules = qstar.modules {
                root = "src",
                include = {"app"},
            },
        },
    },
}

qstar.staticlib "core" {
    lang = {
        cale = {
            modules = qstar.modules {
                root = "src",
                include = {"core"},
            },
        },
    },
}

qstar.sharedlib "plugin" {
    lang = {
        cale = {
            modules = qstar.modules {
                root = "src",
                include = {"plugin"},
            },
        },
    },
}

qstar.test "core_test" {
    sources = {"tests/core_test.cale"},
    deps = {":core"},
}
```

공식 target kind:

| Kind | 의미 |
| --- | --- |
| `exe` | executable |
| `staticlib` | archive/static library |
| `sharedlib` | dynamic/shared library |
| `objectlib` | object collection |
| `modulelib` | Cale module group |
| `test` | test executable or test action |
| `gen` | generated file action |

## qstar.modules

Cale module set을 선언한다.

```lua
lang = {
    cale = {
        modules = qstar.modules {
            root = "src",
            include = {
                "app",
                "util",
                "render::vulkan",
            },
        },
    },
}
```

Optional `exclude`:

```lua
lang = {
    cale = {
        modules = qstar.modules {
            root = "src",
            include = {
                "render",
                "render::vulkan",
            },
            exclude = {
                "render::experimental",
            },
        },
    },
}
```

`root`는 module root directory다. `include`와 `exclude`는 logical module path를 사용한다.

## sources

`sources`는 파일 단위 input이다.

```lua
sources = {
    "src/platform/common.cale",
    "src/native/bridge.c",
}
```

`.cale` 구현은 가능하면 `modules`로 묶고, C/C++ source나 target-specific extra source, generated source는 `sources`에 넣는다.

## qstar.files

Glob/file group이다.

```lua
sources = qstar.files {
    "src/*.c",
    "src/**/*.cale",
}
```

규칙:

- glob result는 deterministic sort를 거친다.
- unmatched glob은 기본 error다.
- optional glob은 `qstar.optional(...)`로 표시한다.
- hidden file은 기본 제외다.

```lua
sources = qstar.files {
    "src/*.c",
    qstar.optional("generated/*.c"),
}
```

## qstar.select

Target/platform condition에 따라 값을 선택한다.

```lua
sources = qstar.select {
    [qstar.os.macos] = {"src/platform/darwin.cale"},
    [qstar.os.linux] = {"src/platform/linux.cale"},
    [qstar.os.windows] = {"src/platform/windows.cale"},
    default = qstar.incompatible("unsupported platform"),
}
```

복합 조건:

```lua
sources = qstar.select {
    [qstar.all { qstar.os.linux, qstar.arch.x86_64 }] = {
        "src/platform/linux_x64.cale",
    },
    default = qstar.incompatible("unsupported target"),
}
```

`qstar.select`는 source-level `#if`를 줄이기 위한 핵심 API다.

Round 1 records `qstar.select` as a placeholder in the explain dump. Target
condition evaluation is a later command-plan step.

## Condition Objects

조건은 graph condition object다.

```lua
qstar.os.macos
qstar.os.linux
qstar.os.windows

qstar.arch.x86_64
qstar.arch.aarch64
qstar.arch.riscv64

qstar.feature "gui"
qstar.profile "freestanding-cale"

qstar.all { ... }
qstar.any { ... }
qstar.not_(...)
```

Lua `not`은 keyword이므로 API 이름은 `not_`로 둔다.

## qstar.join

Path segment를 `/`로 합친다. Lua local 변수와 함께 쓰면 Makefile식 `$VAR` 치환 없이도
반복 path를 명확하게 만들 수 있다.

```lua
local triple = "aarch64-unknown-none-elf"
local libc_include = qstar.join("lib/libc", triple, "include")
```

기존 authoring 호환을 위해 table form은 list를 한 단계 flatten한다.

```lua
sources = qstar.join {
    {"src/main.c"},
    platform_sources(),
}
```

List/table helper는 option 조립에 쓴다.

```lua
local common_c = qstar.merge({
    compile_options = {"-Wall"},
}, {
    compile_options = qstar.append({"-Wextra"}, "-Werror"),
})
```

## qstar.incompatible

현재 target/profile에서 사용할 수 없는 branch를 나타낸다.

```lua
default = qstar.incompatible("unsupported graphics backend")
```

QStar는 incompatible target을 graph에서 skip하거나, 사용자가 직접 빌드할 때 명확한 diagnostic으로 실패시킬 수 있다.

## Labels

공식 label 문법:

```txt
:local
//:root_target
//src/render:render
//src/render/vulkan:vulkan
@pkg//:lib
@pkg//src/lib:lib
```

의미:

| Label | 의미 |
| --- | --- |
| `:local` | 현재 fragment path의 target |
| `//path:name` | current package root 기준 target |
| `@pkg//path:name` | resolved dependency package target |

`@pkg`는 package manager나 CLI가 resolve한 alias만 사용할 수 있다. QStar가 dependency
version을 직접 풀지 않는다.

## deps

Target dependency list다.

```lua
deps = {
    ":local_core",
    "//src/render:render",
    "@core//src/mem:mem",
}
```

`deps`는 label만 허용한다. Filesystem path dependency는 금지한다.

## Header Fields

```lua
lang = {
    cale = {
        public_headers = {
            "include/engine/engine.hcl",
            "include/engine/render.hcl",
        },
        private_headers = {
            "include/engine/internal/debug.hcl",
        },
        public_include_dirs = {
            "include",
        },
    },
}
```

`public_headers`는 install/export할 header file surface다. 항목은 `.h`, `.hcl`,
`.hpp` 같은 확장자를 가질 수 있지만 QStar는 확장자를 의미론으로 해석하지
않는다. `.hcl`은 `#include`로 소비하지만 QStar label이나 Cale `import`와 섞지
않는다. QStar는 HCL 내용을 해석하지 않고, header path가 허용된 include root
안에 있는지와 graph policy에 맞는지만 검증한다.

`.hcl` 자체의 언어 처리는 Cale compiler가 맡는다. `.hcl`은 smart preprocessing, parse, export filter를 거쳐 includer에게 `export` declaration만 노출하는 header surface이며, legacy `.h`처럼 raw textual paste로 정의하지 않는다.

QStar는 `.hcl`의 `export`, `export using`, `export opaque` 같은 declaration 의미를 판단하지 않는다. QStar가 검증하는 것은 `public_headers`가 설치/export surface로 등록되어 있는지, path가 허용된 include root 아래인지, generated header가 선언된 action의 output인지 같은 build graph 정책이다. HCL grammar와 C declaration import/export 규칙은 Cale compiler와 HCL checker가 담당한다.
Include search path는 target top-level이 아니라 `lang.c.include_dirs`,
`lang.cxx.include_dirs`, `lang.asm.include_dirs`, `lang.cale.public_include_dirs`
아래에 둔다.

## Toolchain And Stdlib

```lua
toolchain = "host"
stdlib = "system"
```

초기 toolchain profile reference:

```txt
host
macos-system
linux-gnu-system
linux-musl-system
windows-mingw-system
freestanding-cale
```

초기 stdlib policy:

```txt
system
cale
none
```

QStar는 toolchain profile을 정의하지 않는다. 정의와 discovery는 Cale profile resolver가 담당한다.

## Language Mode Fields

Build graph에 필요한 compile mode만 둔다.

```lua
c = {
    standard = "gnu17",
}

cale = {
    edition = "preview",
}
```

금지:

```lua
secure = {
    profile = "kernel-fast",
}
```

Secure policy와 UB category override는 QStar core DSL 밖의 compiler/package policy
layer에서 관리한다.

## qstar.option

Build graph 모양을 바꾸는 option이다.

```lua
qstar.option "with_gui" {
    type = "bool",
    default = false,
    help = "Build GUI frontend",
}
```

사용:

```lua
deps = qstar.select {
    [qstar.feature "with_gui"] = {":gui"},
    default = {},
}
```

CLI 방향:

```txt
qstar build -Dwith_gui=true
```

`qstar.option`은 build graph shape만 바꾼다. Compiler safety, UB, audit policy는
QStar core DSL 밖의 compiler/package policy layer가 맡는다.

## qstar.custom_target

Generated file action이다.

```lua
qstar.custom_target "version_header" {
    inputs = {
        "VERSION",
    },
    outputs = {
        qstar.output("generated/version.hcl"),
    },
    command = qstar.cli {
        "gen-version",
        "--out", qstar.output(0),
    },
}
```

규칙:

- `outputs` 필수
- `command = qstar.cli { ... }` 필수
- command는 shell string이 아니라 deterministic argv-vector여야 한다.
- command 첫 argv가 `tools/gen.sh` 같은 package-relative path면 기본 허용한다.
- command 첫 argv가 `llvm-objcopy` 같은 bare PATH tool이면 active profile의
  `path_tools` allowlist 또는 `tool_overrides`가 필요하다.
- absolute tool path는 `allow_absolute_tools = true` profile에서만 허용한다.
- env는 직접 읽지 않고 명시 input으로 선언한다.
- Round 7은 실행하지 않고 `generated_action`/`generated_edge`만 출력한다.

## qstar.output

Generated action 안에서 named path를 참조한다.

```lua
qstar.output("generated/version.hcl")
```

`qstar.output(path)`은 path spelling helper다. Makefile의 `$<`, `$@` 같은 암호
기호를 쓰지 않는다.

Round 53부터 `qstar.output(path, metadata)`도 허용한다. 이 형태는 path helper에
generated artifact identity를 붙인다.

```lua
qstar.output("generated/kernel8.img", {
    group = "images",
    format = "raw-binary",
    address = "0x80000",
    layout = "rpi5-kernel8",
})
```

- `group`: output group. 예: `generated`, `images`, `binaries`.
- `format`: output format. 예: `file`, `raw-binary`, `elf`.
- `address`: boot/load address 같은 artifact identity metadata.
- `layout`: RPi/UEFI/firmware packaging layout 같은 artifact identity metadata.

`format = "raw-binary"`이고 `group`이 비어 있으면 QStar는 group을 `images`로
분류한다. Metadata는 command argv를 자동 생성하지 않는다. `llvm-objcopy -O binary`
같은 실제 변환은 여전히 `qstar.custom_target`의 `command = qstar.cli { ... }`가
담당한다. 같은 package 안에서 같은 `group + format + address + layout` metadata를
가진 output이 둘 이상 있으면 duplicate artifact collision으로 거절한다.

Round 68부터 `custom_target.inputs = { qstar.target_file("//:kernel") }`가 generated
action의 artifact dependency edge로 추적된다. `qstar.input(0)`은 command argv에서 실제
artifact path로 해석되고, action key는 token이 아니라 resolved artifact file을 입력으로
hash한다. Direct generated build도 이 edge를 따라 kernel을 먼저 빌드한 뒤 image
transform을 실행한다. `qstar/tests/projects/systems-firmware`가 이 패턴의 canonical
corpus다.

## qstar.stage

Round 55부터 `qstar.stage`는 install과 별개의 boot/package staging rule이다.

```lua
qstar.stage "esp" {
    root = "stage/esp",
    files = {
        qstar.stage_file(qstar.target_file("//:boot"), "EFI/BOOT/BOOTX64.EFI"),
    },
}

qstar.stage "rpi" {
    root = "stage/rpi",
    files = {
        qstar.stage_file("boot/config.txt", "config.txt"),
        qstar.stage_file(qstar.target_file("//:kernel_img"), "kernel8.img"),
        qstar.stage_file("boot/payload.bin", "payload.bin"),
    },
}
```

규칙:

- `root` 필수. Package-relative path여야 한다.
- `files` 필수. 각 항목은 `qstar.stage_file(src, dst)`여야 한다.
- `src`는 package-relative file 또는 `qstar.target_file("//:label")`이다.
- `dst`는 stage root 기준 package-relative path다.
- 같은 stage 안에서 duplicate destination은 거절한다.
- 같은 stage 안에서 `EFI/BOOT`와 `EFI/BOOT/BOOTX64.EFI`처럼 file/dir layout이
  충돌하는 destination도 거절한다.
- `qstar stage //:esp --dry-run`은 copy하지 않고
  `build/qstar/stage/<label>/manifest.json`과 diff를 기록한다. Manifest schema는
  `qstar-stage-manifest-v2`이며 entry마다 `kind`와 `producer`를 기록한다.
- 실제 `qstar stage //:esp`는 target/generated artifact source를 먼저 build하고
  package-local stage root 아래로 copy한다.

## qstar.target_family

Round 69부터 `qstar.target_family`는 multi-arch target variant가 source file을
의도적으로 공유할 때 duplicate source lint를 family 내부로만 조정하는 primitive다.

```lua
qstar.target_family "boot" {
    variants = {"x86_64", "aarch64", "riscv64"},
    allow_shared_sources = true,
}
```

규칙:

- `variants` 또는 `targets` 중 하나는 있어야 한다.
- `targets = {"//:boot_x64"}`는 explicit member label이며 실제 target이어야 한다.
- `targets`를 생략하면 `<family>_<variant>` 또는 `<family>-<variant>` target name을
  family member로 본다.
- `allow_shared_sources = true`인 family 안에서만 `QSTAR043` duplicate source warning을
  억제한다.
- 이 기능은 lint/cache grouping primitive이며, board-specific target kind가 아니다.

## qstar.generated / qstar.generated_dir

Generated target output을 다른 target에서 참조하는 future API다. Round 7 구현은
아직 이 API를 제공하지 않고, `sources = { qstar.output("generated/foo.c") }` 형태로
명시 path를 적는다.

```lua
qstar.configure_file "version_header" {
    output = qstar.output("generated/version.h"),
    defines = {"APP_VERSION=1"},
}
```

## qstar.tool

외부 tool capability를 선언한다.

```lua
qstar.tool "protoc" {
    kind = "program",
    required = true,
}
```

Tool discovery는 build file이 직접 하지 않는다. QStar core와 host/profile resolver가 수행하고, 결과는 graph input으로 기록한다.

## qstar init

Round 21부터 QStar는 sample corpus와 drift-tested project skeleton을 생성한다.

```txt
qstar init c-app my-app
qstar init c-lib my-lib
qstar init generated my-generated-app
qstar init mixed-cale my-mixed-app
```

생성되는 구조는 다음 manual sample과 같은 surface를 유지한다.

```txt
qstar/tests/manual/c-only
qstar/tests/manual/generated
qstar/tests/manual/mixed-cale
```

v0에서 유지할 authoring surface와 deferred 범위는 `docs/qstar-v0-seal.md`에
고정한다.

## QStar v0.2 language options

Round 47부터 C/C++/ASM/Cale option은 target top-level이 아니라 `lang.*` 아래에 둔다.
`include_dirs`라는 이름은 유지하지만 `lang.c.include_dirs`, `lang.cxx.include_dirs`,
`lang.asm.include_dirs`, `lang.cale.public_include_dirs`처럼 language namespace 안에서만
의미가 있다.

```lua
qstar.executable "mixed" {
    sources = {"src/main.c", "src/widget.cpp"},
    lang = {
        c = {
            include_dirs = {"include"},
            compile_options = {"-DAPP_C=1"},
        },
        cxx = {
            include_dirs = {"include"},
            compile_options = {"-DAPP_CXX=1"},
            standard = "c++11",
        },
    },
}
```

Removed authoring API는 stable diagnostic만 낸다. `qstar.exe`는 `qstar.executable`,
`qstar.genrule`은 `qstar.custom_target`, `qstar.config_header`와
`qstar.write_config_header`는 `qstar.configure_file`을 사용한다.

## C++ fields

Round 24부터 C++ source는 build-system input으로 사용할 수 있다.

```lua
qstar.executable "mixed" {
    sources = {"src/main.c", "src/widget.cpp"},
    lang = {
        c = {
            include_dirs = {"include"},
            compile_options = {"-DAPP_C=1"},
        },
        cxx = {
            include_dirs = {"include"},
            compile_options = {"-DAPP_CXX=1"},
            standard = "c++11",
        },
    },
}
```

QStar는 C++ parser가 아니다. `.cc/.cpp/.cxx`를 C++ compiler action으로 분류하고,
target에 C++ source가 있으면 C++ linker를 사용한다. `.cppm`/`.ixx` C++ module
source는 scanner/BMI policy가 없으므로 v1에서는 stable diagnostic으로 막는다.
