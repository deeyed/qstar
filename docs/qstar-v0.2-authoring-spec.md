# QStar v0.2 Authoring Spec

QStar v0.2는 Round 47/48의 hard-cut authoring surface다. 아직 QStar를 쓰는 외부
project가 없으므로 이전 v0.1 syntax compatibility layer는 두지 않는다. QStar는
C/C++/external-language을 잘 지원하지만 그 셋에 종속되지 않는 빌드시스템이다. 언어별 option은
`lang.*` namespace로 격리하고, 일반적인 외부 도구 호출은 shell string이 아니라
`qstar.cli { ... }` argv-vector로 표현한다.

## Canonical target API

```lua
qstar.executable "app" { ... }
qstar.staticlib "core" { ... }
qstar.sharedlib "plugin" { ... }
qstar.test "unit" { ... }
qstar.custom_target "generated" { ... }
qstar.run_target "smoke" { ... }
qstar.configure_file "cfg" { ... }
qstar.stage "esp" { ... }
```

Removed API는 stable diagnostic만 낸다.

- `qstar.exe removed; use qstar.executable`
- `qstar.genrule removed; use qstar.custom_target`
- `qstar.config_header removed; use qstar.configure_file`
- `qstar.write_config_header removed; use qstar.configure_file`

## Constants And Limited Lua Helpers

Round 63 makes deterministic constants and limited `local function` helpers part
of the v0.2 authoring contract. The evaluator exposes:

- `QSTAR_VERSION`, `QSTAR_VERSION_MAJOR`, `QSTAR_VERSION_MINOR`,
  `QSTAR_VERSION_PATCH`
- `QSTAR_HOST_OS`, `QSTAR_HOST_ARCH`
- `QSTAR_PACKAGE_ROOT`, `QSTAR_PROJECT_ROOT`
- `QSTAR_PROFILE`, `QSTAR_TARGET`
- `qstar.version`, `qstar.host.os`, `qstar.host.arch`, `qstar.project.root`

QStar files may define `local function` helpers, local variables, table
literals, `ipairs`, `pairs`, `table.insert`, and `string.*` based table
construction. Global assignment is a stable error. Forbidden APIs remain
forbidden: `io.open`, `os.execute`, `require`, `load`, `loadfile`, `dofile`,
`debug`, package-level dynamic loading, process/time/network/random access.

```lua
local function common_c()
  return {
    public_include_dirs = {"include"},
    compile_options = {"-Wall", "-DQSTAR_TARGET=" .. QSTAR_TARGET},
  }
end
```

## Language option namespace

Top-level C/C++ field는 v0.2에서 금지한다.

- `include_dirs`
- `public_include_dirs`
- `private_include_dirs`
- `system_include_dirs`
- `interface_include_dirs`
- `cflags`
- `cxxflags`
- `cxx_standard`

대신 언어별 option은 `lang.*` 아래에 둔다.

```lua
qstar.staticlib "core" {
  sources = {
    "lib/src/core.c",
  },
  lang = {
    c = {
      public_headers = {
        "lib/include/core.h",
      },
      public_include_dirs = {
        "lib/include",
      },
      compile_options = {
        "-ffreestanding",
      },
      defines = {
        "CORE_BUILD=1",
      },
    },
  },
}
```

```lua
qstar.executable "tool" {
  sources = {
    "tool/src/main.cpp",
  },
  lang = {
    cxx = {
      standard = "c++20",
      include_dirs = {
        "tool/include",
      },
      compile_options = {
        "-fno-exceptions",
      },
    },
  },
}
```

```lua
qstar.staticlib "boot" {
  sources = {
    "boot/start.S",
  },
  lang = {
    asm = {
      include_dirs = {
        "boot/include",
      },
      compile_options = {
        "-ffreestanding",
      },
      preprocess = true,
    },
  },
}
```

```lua
qstar.staticlib "cale_core" {
  sources = {
    "src/core.foreign",
  },
  lang = {
    external-tool = {
      profile = "safe",
      compile_options = {},
      public_headers = {
        "include/core.h",
      },
      public_include_dirs = {
        "include",
      },
    },
  },
}
```

`include_dirs`라는 이름은 유지하지만 C/C++/ASM/external language namespace 안에서만
의미가 있다. Rust, Zig, Go 같은 future provider는 include directory 개념을 강제로
상속하지 않는다.

`lang` namespace v0.2 surface:

| Namespace | Fields |
| --- | --- |
| `lang.c` | `public_headers`, `private_headers`, `include_dirs`, `public_include_dirs`, `private_include_dirs`, `system_include_dirs`, `compile_options`, `defines` |
| `lang.cxx` | `public_headers`, `private_headers`, `standard`, `modules`, `include_dirs`, `public_include_dirs`, `private_include_dirs`, `system_include_dirs`, `compile_options`, `defines` |
| `lang.asm` | `include_dirs`, `compile_options`, `preprocess` |
| `lang.cxx` | `public_headers`, `private_headers`, `include_dirs`, `public_include_dirs`, `private_include_dirs`, `profile`, `compile_options`, `modules` |

Unknown namespace such as `lang.rust` and unknown field such as
`lang.c.unknown_option` are lint errors until that provider explicitly defines
its own schema.

## Generic command model

`qstar.custom_target` is the generated-output rule. v0.2 removes `tool`/`args`
from the authoring surface and uses `command = qstar.cli { ... }` instead.
`qstar.cli` is always an argv-vector; QStar does not evaluate shell strings.

```lua
qstar.custom_target "generated_value" {
  inputs = {
    "tools/value.txt",
  },
  outputs = {
    qstar.output("generated/value.c"),
  },
  command = qstar.cli {
    "tools/gen-value.sh",
    qstar.input(0),
    qstar.output(0),
  },
}
```

`qstar.input(N)` and `qstar.output(N)` are command placeholders resolved against
the `inputs` and `outputs` lists of the same custom target. `qstar.output(path)`
continues to spell a generated path in source/header lists.

`qstar.run_target` is for build-artifact smoke commands. It does not produce a
normal compile/link artifact; it runs an external command after its deps are
built, records stdout/stderr logs, supports a timeout, and can check a marker in
stdout, stderr, or an optional package-relative serial log file.

```lua
qstar.run_target "smoke" {
  deps = {
    "//:app",
  },
  command = qstar.cli {
    qstar.target_file("//:app"),
  },
  timeout = 5,
  marker = "OK",
  marker_log = "serial.log",
}
```

Round 56부터 QEMU smoke도 dedicated keyword가 아니라 `run_target` 위에 표현한다.
Wrapper script가 serial output을 file로 저장하면 `marker_log`에 그 path를 둔다.
QStar는 marker missing, timeout, nonzero exit code를 각각
`status=marker-missing`, `status=timeout`, `status=exit-code`로 분리하고,
`build/qstar/logs/last-failure.replay`와 action log에 재현 command를 남긴다.
Round 59에서는 이 실패 surface를 release hardening 단계로 올려
`qstar-action-diagnostic-v1` JSON record도 함께 출력한다. Link failure, objcopy
failure, package/stage failure, QEMU timeout은 각각 `link-failure`,
`objcopy-failure`, `package-failure`, `qemu-timeout`으로 분리된다. Systems/firmware
graph는 link, raw image transform, staging, smoke wrapper가 연쇄되므로,
각 단계의 실패 원인이 같은 `last-failure`/`replay` UX와 machine-readable diagnostic에
남아야 한다.

`qstar.target_file("//pkg:target")` resolves to the primary artifact path for
that target. UEFI/RPi/firmware-style flows should be expressed with ordinary rules:
compile/link targets, `custom_target` for `llvm-objcopy` or image generation,
and `run_target` for QEMU or serial-log smoke commands. QStar does not need
built-in `uefi_app` or `embed_binary` keywords for v0.2.

## External tool command policy

`qstar.custom_target` keeps `command = qstar.cli { ... }` as the only authoring
surface. The old `tool = ...` field is not restored. Instead, QStar resolves the
first argv item through profile capability.

```lua
qstar.custom_target "kernel_img" {
  inputs = {
    qstar.target_file("//:kernel"),
  },
  outputs = {
    qstar.output("generated/kernel8.img", {
      group = "images",
    }),
  },
  command = qstar.cli {
    "llvm-objcopy",
    "-O",
    "binary",
    qstar.input(0),
    qstar.output(0),
  },
}
```

`qstar.output(path, metadata)`는 generated artifact identity를 명시한다. Metadata는 `group`,
`output_group`, `format` 문자열 field만 받는다. `format`은 생략하면 `file`이고, 명시 가능한
값은 external object bridge용 `object`뿐이다. Raw image 변환이나 load address/layout 같은
project-specific 의미는 metadata가 아니라 `qstar.custom_target` command argv, input file,
stage rule 또는 project-owned config로 표현한다.

`qstar build //:kernel_img`처럼 `custom_target` label을 직접 빌드할 수 있다. Direct
generated build는 compile/link target closure를 만들지 않고 해당 generated action만
실행한다. `qstar.target_file("//:kernel_img")`는 generated action의 첫 output path로
해석된다.

Round 68부터 `custom_target.inputs`도 `qstar.target_file("//:label")`을 받는다.
이 표기는 command argv placeholder가 아니라 graph edge다. `qstar.input(0)`으로 command에
전달하면 executor는 먼저 `//:label`을 빌드하고, cache key에는 token이 아니라 resolved
artifact path와 file metadata/content hash를 넣는다. 따라서 `kernel.elf -> objcopy ->
kernel8.img -> stage` 흐름이 wrapper script 없이 graph 안에서 표현된다.

Round 57부터 generated action output은 다른 target의 `sources` 또는
`lang.c`/`lang.cxx`/`lang.cxx`의 `public_headers`/`private_headers`에 등장하면 해당
target build 전에 실행된다. 이 edge는 C/C++ depfile 없이도 action key에 들어가며,
binary blob input이나 generated object content가 바뀌면 downstream compile/link
action이 `input-changed`로 rebuild된다. `format = "object"`는
group 기본값이 `objects`이고, `.o`/`.obj` source는 compile action 없이 final
archive/link input으로 직접 소비된다.

Round 58부터 `qstar/tests/projects/systems-firmware`가 이 흐름의 canonical release
corpus다. Kernel ELF는 ordinary `qstar.executable` target으로 만들고,
`qstar.custom_target "kernel_img"`는 `inputs = { qstar.target_file("//:kernel") }`와
`qstar.input(0)`을 통해 `llvm-objcopy -O binary` 형태의 raw image를 만든다. 이
target-file artifact edge는 generated action cache input에도 들어가므로 kernel ELF가
바뀌면 image action도 rebuild 대상이 된다. Stage target은 kernel ELF를 먼저 빌드한 뒤
image transform과 copy-only package layout을 실행한다.

```lua
qstar.custom_target "embed_payload" {
  inputs = {
    "fixtures/payload.elf",
  },
  outputs = {
    qstar.output("generated/payload.o", {
      format = "object",
    }),
  },
  command = qstar.cli {
    "tools/embed-object.sh",
    qstar.input(0),
    qstar.output(0),
  },
}

qstar.executable "probe" {
  sources = {
    "src/main.c",
    qstar.output("generated/payload.o"),
  },
}
```

## Boot package/staging rule

Round 55부터 `qstar.stage`는 install과 구분되는 copy-only staging surface다.
`install`은 prefix 아래 개발 산출물과 public header를 배치하는 흐름이고, `stage`는
ESP, RPi boot directory, firmware package처럼 정해진 layout을 만드는 흐름이다.
QStar는 `uefi_app`, `rpi_image`, `embed_binary` 같은 dedicated target kind를 추가하지
않고, compile/link target, `custom_target`, `stage`를 조합한다.

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

`qstar.stage_file(src, dst)`의 `src`는 package-relative file 또는
`qstar.target_file("//:label")`이다. `dst`는 stage root 기준 package-relative path다.
Duplicate destination, parent/child layout collision, package root 밖 source/output,
unknown target label은 stable diagnostic으로 거절한다. `qstar stage //:esp --dry-run`은
copy하지 않고 staged manifest와 would-create/would-update/unchanged diff를 출력한다.
Round 69부터 manifest는 `qstar-stage-manifest-v2`이며 각 entry의 source kind(`file`,
`target`, `custom_target`, `custom_output`)와 producer label을 기록한다. 실제
`qstar stage //:esp`는 target/generated artifact source를 먼저 build한 뒤 copy한다.

## Target Family Lint Policy

Multi-arch firmware나 OS project는 같은 source file을 여러 target variant가 의도적으로
공유할 수 있다. 이 경우 duplicate source lint를 전역으로 끄지 않고, family 안에서만
허용한다.

```lua
qstar.target_family "boot_family" {
  variants = {"x86_64", "aarch64", "riscv64"},
  allow_shared_sources = true,
}
```

`targets = {"//:boot_x64", "//:boot_aa64"}`로 explicit member를 적을 수도 있다.
`targets` label은 실제 target이어야 하며, 오타는 `qstar check`에서 거절한다.
`targets`를 생략하면 `<family>_<variant>` 또는 `<family>-<variant>` target name을
자동 family member로 본다. `target_family`는 lint/cache grouping primitive일 뿐이며
UEFI, RPi 같은 board-specific target kind를 만들지 않는다.

By default, only package-relative path tools such as `tools/gen.sh` are allowed.
Bare PATH tools must be allowlisted in the active profile.

```lua
qstar.profile "rpi5" {
  path_tools = {
    "llvm-objcopy",
    "qemu-system-aarch64",
  },
}
```

Absolute tools are disabled by default and require an explicit profile
capability:

```lua
qstar.profile "local" {
  allow_absolute_tools = true,
}
```

Profiles may override the implementation of a PATH tool without changing the
build file spelling:

```lua
qstar.profile "test" {
  tool_overrides = {
    "llvm-objcopy=tools/fake-objcopy.sh",
  },
}
```

This keeps QStar language-neutral: UEFI, RPi, image conversion, binary packing,
and smoke wrappers are ordinary argv-vector actions, not built-in target kinds.
`qstar doctor` reports PATH tool discovery and override status. The resolved
tool path is used in dry-run/build argv and action key material.

The systems firmware corpus follows the same rule. UEFI is not a dedicated
UEFI builtin; it is `qstar.executable` plus profile-selected artifact name
(`BOOTX64.EFI` or `BOOTAA64.EFI`), MSVC response style, and
`/subsystem:efi_application` link options. RPi packaging is not a dedicated
RPi-image builtin; it is `qstar.custom_target` for the image and `qstar.stage`
for the boot directory layout.

## Freestanding and linker policy

Freestanding, kernel, and firmware builds are profile-driven. The
build file describes targets, source edges, and the selected machine/toolchain
policy through `qstar.profile`. QStar has a single project entry point:
`qstar.lua`.

```lua
qstar.profile "rpi5" {
  toolchain = "clang",
  target = "aarch64-none-elf",
  arch = "aarch64",
  cpu = "cortex-a76",
  abi = "lp64",
  freestanding = true,
  cc = "clang",
  linker = "ld.lld",
  link_options = {
    "-nostdlib",
    "-T",
    "linker/rpi5-aarch64.ld",
    "--defsym=__kernel_base=0x80000",
  },
  link_inputs = {
    "linker/rpi5-aarch64.ld",
  },
}
```

`freestanding = true` adds conservative compile flags through the profile:
`-ffreestanding`, `-fno-builtin`, and `-fno-stack-protector`. The selected
architecture may add arch-specific safety flags. `aarch64`/`arm64` currently
adds `-mgeneral-regs-only`; `x86_64`/`amd64` adds `-mno-red-zone`. `cpu` and
`abi` render as `-mcpu=<value>` and `-mabi=<value>` when present.

Link policy can be set globally in the profile and refined on a target:

```lua
qstar.executable "kernel" {
  sources = {
    "src/kernel.c",
    "boot/start.S",
  },
  link_options = {
    "-Wl,-Map=kernel.map",
    "-T",
    "linker/kernel.ld",
    "--defsym=__stack_top=0x80000",
  },
  link_inputs = {
    "linker/kernel.ld",
  },
  lang = {
    asm = {
      preprocess = true,
    },
  },
}
```

Target `link_options` are rendered into the link argv exactly as authored. Files
or artifacts that the linker reads, but that should not be added to argv by
QStar, are listed in `link_inputs`. A package-relative link input is tracked as
a link action input, so changing it rebuilds the link action even if object files
are unchanged.

## PE/COFF and UEFI link surface

QStar does not add a hardcoded `uefi_app` rule. UEFI applications are expressed
as ordinary executable targets plus profile-selected linker/artifact policy.

```lua
qstar.profile "uefi-x64" {
  toolchain = "clang",
  target = "x86_64-pc-windows-msvc",
  cc = "clang",
  linker = "lld-link",
  response_style = "msvc",
  artifact_names = {
    "//:boot=BOOTX64.EFI",
  },
}
```

```lua
qstar.executable "boot" {
  sources = {
    "src/efi_main.c",
  },
  lang = {
    c = {
      compile_options = {
        "-ffreestanding",
      },
    },
  },
  link_options = {
    "/subsystem:efi_application",
    "/entry:efi_main",
    "/nodefaultlib",
  },
}
```

When the selected linker path contains `lld-link` or `link.exe`, QStar renders
the output argument as `/out:build/qstar/out/.../BOOTX64.EFI` instead of `-o`.
`artifact_names` maps a target label or target name to a profile-specific
filename. For AArch64 UEFI, the profile can map the same `//:boot` target to
`BOOTAA64.EFI`. A target-local `artifact_name = "BOOTLOCAL.EFI"` overrides the
profile mapping.

Artifact names are basenames, not install/staging paths. `EFI/BOOT/BOOTX64.EFI`
belongs in `qstar.stage`, not `artifact_name`; putting a slash in
`artifact_name` or `artifact_names` is rejected.

## Fragment naming

Root entry는 `qstar.lua`다. `qstar.workspace` marker는 제거되었다. QStar는 현재
authoring file에서 위로 올라가며 가장 가까운 `qstar.lua`를 package root로 삼는다.
Root `qstar.lua`는 project metadata를 선언할 수 있다.

```lua
qstar.project {
  name = "my-project",
  version = "0.1.0",
  root = ".",
}
```

v1에서 `root`는 `"."`만 허용한다.

`qstar.subdir("foo")`는 `foo/foo.qst`를 읽는다. `qstar.subdir("app/src")`는
`app/src/src.qst`를 읽는다. Label package도 fragment directory를 따른다.

- `lib/lib.qst`의 `qstar.staticlib "core"` -> `//lib:core`
- `app/src/src.qst`의 `qstar.executable "app"` -> `//app/src:app`

v0.2에서는 package-root style과 source-dir style을 둘 다 정본으로 둔다.

```txt
lib/
├── lib.qst
├── include/
└── src/
```

```txt
app/
└── src/
    ├── src.qst
    └── main.c
```

## v0.2 keyword audit

Current authoring keywords are intentionally small and language-neutral.

| Group | Supported surface |
| --- | --- |
| Target/rule API | `qstar.executable`, `qstar.staticlib`, `qstar.sharedlib`, `qstar.test`, `qstar.custom_target`, `qstar.run_target`, `qstar.configure_file`, `qstar.stage` |
| Lint grouping API | `qstar.target_family` |
| Command helpers | `qstar.cli`, `qstar.input`, `qstar.output`, `qstar.target_file`, `qstar.stage_file` |
| Graph helpers | `qstar.subdir`, `qstar.files`, `qstar.modules`, `qstar.join`, `qstar.copy`, `qstar.append`, `qstar.merge`, `qstar.extend` |
| Constants | `QSTAR_VERSION`, `QSTAR_HOST_OS`, `QSTAR_HOST_ARCH`, `QSTAR_PACKAGE_ROOT`, `QSTAR_PROJECT_ROOT`, `QSTAR_PROFILE`, `QSTAR_TARGET`, `qstar.version`, `qstar.host.os`, `qstar.host.arch`, `qstar.project.root` |
| Link policy | `link_options`, `link_inputs` |
| Language namespaces | `lang.c`, `lang.cxx`, `lang.asm`, `lang.cxx` |
| Removed API | `qstar.exe`, `qstar.genrule`, `qstar.config_header`, `qstar.write_config_header` |
| Removed top-level language fields | `include_dirs`, `public_include_dirs`, `private_include_dirs`, `system_include_dirs`, `interface_include_dirs`, `public_headers`, `private_headers`, `modules`, `header_include_dirs`, `cflags`, `cxxflags`, `cxx_standard` |

QStar v0.2 does not add built-in `uefi_app`, `rpi_image`, `embed_binary`, or
`qemu_smoke` keywords. Those flows are represented by generic targets,
`qstar.cli` command vectors, profile/toolchain policy, `qstar.stage`, and
`qstar.run_target`.

## Current executor status

`lang.c`, `lang.cxx`, `lang.asm`은 local executor command rendering에 연결되어 있다.
`qstar.custom_target`은 package-local generated output action으로 실행된다.
`qstar.run_target`은 build artifact 이후 외부 command smoke를 실행한다. `.s`/`.S`
source는 compiler driver 기반 assembler action으로 object를 만들며,
`lang.asm.include_dirs`, `lang.asm.compile_options`, `lang.asm.preprocess`를 적용한다.
`lang.cxx.public_include_dirs`와 `lang.cxx.private_include_dirs`는 header-language header include
surface를 표현한다. QStar는 `.h` 의미론을 해석하지 않고 path와 graph 정책만
검증한다.
