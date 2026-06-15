# QStar DSL Hard Cut Decision

이 문서는 QStar가 특정 downstream, kernel, bootloader, firmware, Cale 전용 도구처럼
보이지 않도록 DSL 표면을 다시 좁히기 위한 결정 문서다. 구현은 이후 라운드에서
진행하지만, 방향은 이 문서를 기준으로 한다.

## 원칙

QStar는 Meson/CMake/Bazel 계열의 범용 빌드시스템이어야 한다. Core DSL은 build graph,
target, custom action, generated output, install/stage, tool invocation을 표현한다. 특정
실행 환경, 특정 OS artifact, 특정 compiler family, 특정 언어의 policy를 QStar core가
의미론으로 소유하지 않는다.

Hard cut은 호환 layer 없이 진행한다.

- legacy wrapper를 두지 않는다.
- removed API shim을 두지 않는다.
- old spelling을 위한 migration helper를 두지 않는다.
- old spelling 전용 diagnostic을 두지 않는다.
- parser/help/wiki/snippet/manpage/test corpus에서 old spelling을 정본처럼 남기지 않는다.
- 불가피한 parser error는 일반 unknown field 또는 nil field failure 수준으로만 남긴다.

즉, 삭제된 문법은 QStar에 처음부터 없었던 것처럼 취급한다.

## Core Allowlist

다음 surface만 QStar core DSL의 기본 allowlist로 남긴다.

```lua
qstar.project
qstar.toolset
qstar.config
qstar.executable
qstar.staticlib
qstar.sharedlib
qstar.test
qstar.custom_target
qstar.configure_file
qstar.run_target
qstar.group
qstar.stage
qstar.import_file
qstar.import_module
qstar.subdir
qstar.cli
qstar.status
qstar.input
qstar.output
qstar.target_file
qstar.stage_file
qstar.files
qstar.join
qstar.copy
qstar.append
qstar.merge
qstar.extend
```

`qstar.toolset`은 새 surface다. 기존 `qstar.profile`이 맡던 tool role, response file,
allowlisted external tool policy를 대체한다. Toolset은 build graph의 tool selection을
표현할 뿐, target triple, freestanding, CPU, ABI, sysroot의 의미를 해석하지 않는다.

## Removed Surfaces

다음 surface는 core DSL에서 삭제한다.

```lua
qstar.profile
condition-object APIs
lang.cale
qstar init mixed-cale
```

다음 field 또는 alias도 삭제한다.

```lua
freestanding
arch
cpu
abi
cc
compiler
c_compiler
cxx
cxx_compiler
cale
cale_compiler
ar
archiver
linker
sysroot
resource_dir
profile-level compile_options
profile-level include_dirs
profile-level lib_dirs
profile-level link_options
profile-level dedicated linker symbol definitions
profile-level dedicated linker script path
artifact_names
tool_overrides
external_tools
external_absolute_tools
```

`tool_overrides`는 toolset이 명시 tool role을 직접 선언하므로 제거한다.
`external_tools`와 `external_absolute_tools`는 `path_tools`와 `allow_absolute_tools` alias였으므로
새 DSL에는 남기지 않는다.

## Cale Removal

Cale은 아직 안정화된 언어가 아니므로 QStar core에서 제거한다. 나중에 Cale language provider가
성숙하면 조건부 provider로 다시 편입할 수 있다.

삭제 대상:

- `lang.cale`
- `lang.cale.profile`
- `lang.cale.modules`
- `.cale`, `.cl` source suffix classification
- `toolchain = "cale"`
- `toolchain = "cale-sol"`
- `cale` compiler role
- `qstar init mixed-cale`
- Cale/HCL 전용 wiki, manpage, snippets, LSP hover

QStar는 HCL을 해석하지 않는다. HCL이 다시 등장하더라도 QStar core 문법이 아니라 Cale provider의
입력 파일 정책으로 다룬다.

## Toolset

새 toolset 문법은 다음 형태를 목표로 한다.

```lua
qstar.toolset "clang-aarch64" {
  tools = {
    c = qstar.cli {"clang"},
    cxx = qstar.cli {"clang++"},
    asm = qstar.cli {"clang"},
    archive = qstar.cli {"llvm-ar"},
    link = qstar.cli {"ld.lld"},
  },

  response_files = "on",
  response_style = "posix",

  path_tools = {
    "llvm-objcopy",
  },

  allow_absolute_tools = false,
}
```

Tool roles are generic QStar roles, not compiler-family semantics. QStar does not infer
`-target`, `-mcpu`, `-mabi`, `--sysroot`, `-resource-dir`, freestanding flags, standard
library mode, or linker behavior from a toolset.

Targets opt into a toolset explicitly.

```lua
qstar.executable "app" {
  toolset = "//:clang-aarch64",
  configs = {
    "//:cross_c_options",
  },
  sources = {
    "src/main.c",
  },
}
```

## Config Replaces Profile Policy

기존 profile의 compile/link policy는 `qstar.config`로 표현한다.

기존 형태:

```lua
qstar.profile "rpi5-aarch64" {
  target = "aarch64-none-elf",
  arch = "aarch64",
  cpu = "cortex-a76",
  abi = "lp64",
  freestanding = true,
  cc = "clang",
  ar = "llvm-ar",
  linker = "ld.lld",
  sysroot = "sysroot/aarch64-none",
  resource_dir = "toolchains/clang-resource",
  compile_options = {
    "-ffreestanding",
  },
  link_options = {
    "-nostdlib",
  },
}
```

교정 형태:

```lua
qstar.toolset "clang-aarch64" {
  tools = {
    c = qstar.cli {"clang"},
    cxx = qstar.cli {"clang++"},
    asm = qstar.cli {"clang"},
    archive = qstar.cli {"llvm-ar"},
    link = qstar.cli {"ld.lld"},
  },
  response_files = "on",
  response_style = "posix",
  path_tools = {"llvm-objcopy"},
}

qstar.config "cross_c_options" {
  lang = {
    c = {
      compile_options = {
        "-target", "aarch64-none-elf",
        "-mcpu=cortex-a76",
        "-ffreestanding",
        "-fno-builtin",
        "--sysroot=sysroot/aarch64-none",
        "-resource-dir", "toolchains/clang-resource",
      },
    },
    asm = {
      compile_options = {
        "-target", "aarch64-none-elf",
        "-mcpu=cortex-a76",
        "-ffreestanding",
      },
    },
  },
  link_options = {
    "-nostdlib",
  },
}

qstar.executable "app" {
  toolset = "//:clang-aarch64",
  configs = {
    "//:cross_c_options",
  },
  sources = {
    "src/start.S",
    "src/main.c",
  },
}
```

이 방식은 기존 기능을 모두 표현할 수 있다. 차이는 QStar가 의미를 추론하지 않는다는 점이다.
QStar는 `freestanding`, `cpu`, `abi`, `sysroot` 같은 domain word를 해석하지 않고, 사용자가
필요한 argv를 명시한다.

## Linker-Specific Builtins

Dedicated linker script and linker symbol definition fields expose GNU/low-level
linker policy as core DSL, so they are removed.
대신 generic link option과 link-time input을 조합한다.

```lua
qstar.executable "app" {
  sources = {
    "src/main.c",
  },
  link_inputs = {
    "link/layout.ld",
  },
  link_options = {
    "-T",
    "link/layout.ld",
    "-Wl,--defsym=APP_BASE=0x100000",
  },
}
```

`link_inputs`는 generic field다. Link command에 직접 들어가는 인자가 아니라 rebuild dependency
tracking만 담당한다.

## Output Metadata

`qstar.output(path, { format = "object" })`는 유지한다. 이는 external language/object producer
bridge에 필요하고, QStar가 해당 언어를 소유하지 않는다는 원칙과 맞는다.

Load address나 binary layout 같은 domain-specific metadata는 core DSL에 두지 않는다.
그 의미는 command argv, input file, stage rule 또는 project-owned config가 담당한다.

이 값들은 firmware/image layout 의미를 QStar core가 암시한다. 필요한 경우 사용자가 output path,
description, custom command argv 안에서 직접 관리한다.

## Platform And Conditions

Condition-object API는 삭제한다. OS/arch 조건은 grammar가 아니라 실행 시점에 주입되는
read-only host table과 일반 Lua 조건문으로 표현한다.

```lua
local platform_configs = {}

if qstar.host.os == "macos" then
  qstar.config "macos_link" {
    link = {
      frameworks = {
        "Foundation",
      },
    },
  }
  platform_configs = {"//:macos_link"}
end

qstar.executable "app" {
  configs = qstar.append({
    "//:base_options",
  }, platform_configs),
  sources = {
    "src/main.c",
  },
}
```

`qstar.host.os`와 `qstar.host.arch`는 condition grammar가 아니라 evaluator가 주입하는 read-only
environment value다. CMake의 `CMAKE_SYSTEM_NAME` 같은 특수 상수에 가깝다.

## Frameworks

`frameworks`는 macOS 전용 link concept이다. 새 DSL에서는 macOS branch 안에서만 노출한다.

허용되는 관행:

```lua
if qstar.host.os == "macos" then
  qstar.config "macos_frameworks" {
    link = {
      frameworks = {
        "Foundation",
        "CoreServices",
      },
    },
  }
end
```

금지되는 관행:

```lua
qstar.executable "app" {
  frameworks = {
    "Foundation",
  },
}
```

`frameworks`는 generic target/config top-level field가 아니다. macOS host or target context가
명시된 branch 안에서만 문서와 snippets에 등장해야 한다.

## Run Target Marker

`marker`는 generic enough이므로 유지 가능하다. `marker_log`는 serial/QEMU smell이 강하므로
삭제하거나 일반화한다.

목표 형태:

```lua
qstar.run_target "smoke" {
  command = qstar.cli {"tools/smoke.sh"},
  expect = {
    contains = "OK",
    file = "build/qstar/logs/smoke.log",
  },
}
```

`expect`는 새 후보 field다. 이름에 serial, emulator, QEMU 의미를 넣지 않는다.

## Documentation Cleanup Scope

다음 문서는 hard cut 이후 정리 대상이다.

- `README.md`
- `README.ko.md`
- `wiki/AI_INDEX.md`
- `wiki/reference/profiles.md`
- `wiki/reference/qstar-lua.md`
- `wiki/reference/language-providers.md`
- `wiki/reference/lang-cale.md`
- `wiki/reference/custom-target.md`
- `wiki/reference/run-target.md`
- `wiki/reference/target-rules.md`
- `wiki/tutorials/freestanding-image.md`
- `wiki/cookbook/qemu-smoke.md`
- `wiki/cookbook/objcopy.md`
- `wiki/cookbook/staging.md`
- `docs/syntax.md`
- `docs/examples.md`
- `docs/graph-ir.md`
- `docs/rule-model.md`
- `docs/language-provider-backend-contract.md`
- `docs/windows-path-process.md`
- `docs/windows-artifact-policy.md`
- `man/man1/qstar.1`
- `man/man5/qstar-lua.5`
- `editors/vscode/qstar/snippets/qstar.json`

삭제가 필요한 public wording:

- Cale
- HCL
- freestanding
- kernel
- bootloader
- firmware
- RPi/Raspberry Pi/rpi5
- UEFI/EFI/BOOTX64/BOOTAA64
- QEMU
- serial log
- GNU-only `defsym` as first-class DSL

Platform support 문서에서 macOS/Linux/Windows라는 단어를 쓰는 것은 허용된다. 다만 그 단어는
release/support matrix, host validation, artifact policy 문맥에만 둔다. Core authoring examples의
default vocabulary가 되어서는 안 된다.

## Test Corpus Cleanup Scope

다음 corpus는 이름과 예제를 일반화하거나 legacy/internal corpus로 격리한다.

- `tests/corpus/toolchain-app`
- `tests/projects/package-flow`
- `tests/medium-project-performance.sh`
- `tests/large-project-performance.sh`
- `tests/projects/binary-blob-embed`
- Windows artifact tests that rely on profile-level artifact maps
- response-file tests that rely on `qstar.profile`

성능 corpus는 다음처럼 generic project shape로 둔다.

- `app_core`
- `platform_adapter`
- `service_*`
- `module_*`
- `plugin_*`
- `asset_pipeline`
- `package_bundle`

## Implementation Order

권장 hard cut 순서:

1. `qstar.toolset` schema 추가.
2. target/config allowlist v2 확정.
3. `qstar.profile` registration 제거.
4. profile data structures, resolver, auto flags 제거.
5. Cale language provider surface 제거.
6. Condition-object API 제거.
7. `frameworks`를 macOS-scoped link config로만 노출.
8. Dedicated linker-script/symbol fields, `address`, `layout`, `marker_log` 제거.
9. docs/wiki/manpage/snippets/test corpus 정리.
10. release/readiness 문서에서 old syntax가 없음을 확인.

이 순서는 compatibility보다 correctness를 우선한다.
