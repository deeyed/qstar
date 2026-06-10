# QStar v0.2 작성 문법

QStar v0.2부터 새 project는 아래 target/rule API를 사용한다. QStar는 C/C++/Cale을
잘 지원하지만 그 셋에 종속되지 않는 빌드시스템이다. 외부 도구 호출은 shell string이
아니라 `qstar.cli { ... }` argv-vector로 적는다.

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

## Executable

```lua
qstar.executable "app" {
  sources = {
    "src/main.c",
  },
  deps = {
    "//lib:core",
  },
}
```

## Static library

```lua
qstar.staticlib "core" {
  sources = {
    "lib/src/core.c",
  },
  public_headers = {
    "lib/include/core.h",
  },
  lang = {
    c = {
      public_include_dirs = {
        "lib/include",
      },
    },
  },
}
```

## C++ target

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

## Generated file

```lua
qstar.configure_file "cfg" {
  output = qstar.output("generated/config.h"),
  defines = {
    "HAVE_CONFIG=1",
  },
}

qstar.custom_target "generated_source" {
  inputs = {
    "tools/value.txt",
  },
  outputs = {
    qstar.output("generated/value.c"),
  },
  command = qstar.cli {
    "tools/gen-source.sh",
    qstar.input(0),
    qstar.output(0),
  },
}

qstar.executable "app" {
  sources = {
    "src/main.c",
    qstar.output("generated/value.c"),
  },
  private_headers = {
    qstar.output("generated/config.h"),
  },
  lang = {
    c = {
      include_dirs = {
        "generated",
      },
    },
  },
}
```

`qstar.input(0)`과 `qstar.output(0)`은 같은 `custom_target`의 `inputs`/`outputs`
목록을 가리키는 command placeholder다. `qstar.output("generated/value.c")`는 source
list나 header list에서 generated path를 쓰기 위한 helper다.

Round 53부터 binary/image 변환처럼 산출물 의미가 중요한 경우에는
`qstar.output(path, metadata)`를 쓴다.

```lua
qstar.custom_target "kernel_img" {
  inputs = {
    "fixtures/kernel.elf",
  },
  outputs = {
    qstar.output("generated/kernel8.img", {
      group = "images",
      format = "raw-binary",
      address = "0x80000",
      layout = "rpi5-kernel8",
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

Metadata field는 `group`, `format`, `address`, `layout` 네 문자열만 허용된다.
`format = "raw-binary"`는 `group`이 없으면 자동으로 `images` group이 된다. QStar는
metadata를 실제 linker/objcopy option으로 바꾸지 않는다. 실제 변환은 위 예시처럼
명시적인 argv-vector command가 한다. 같은 package 안에서 같은
`group + format + address + layout`을 가진 output이 둘 이상 있으면 충돌로 거절한다.

`qstar build //:kernel_img`는 generated action 자체를 직접 실행한다.
`qstar.target_file("//:kernel_img")`는 generated action의 첫 output path로 해석된다.

Round 57부터 generated output은 target의 `sources`, `public_headers`,
`private_headers`에서 소비될 때 dependency closure 안에서 먼저 실행된다. 이 정책으로
ELF fixture나 firmware blob을 assembly/object로 embed하는 패턴을 표현할 수 있다.

```lua
qstar.custom_target "embed_object" {
  inputs = {
    "fixtures/payload.elf",
  },
  outputs = {
    qstar.output("generated/payload.o", {
      format = "object",
      layout = "rpi5-elf-fixture-embed",
    }),
  },
  command = qstar.cli {
    "tools/embed-object.sh",
    qstar.input(0),
    qstar.output(0),
  },
}

qstar.executable "kernel_probe" {
  sources = {
    "src/main.c",
    qstar.output("generated/payload.o"),
  },
}
```

`format = "object"`는 `group`을 생략하면 자동으로 `objects` group이 된다. `.o`와
`.obj` source는 compile action을 만들지 않고 final archive/link input으로 직접
들어간다. Blob input, package-local embed tool, generated output content는 cache key에
반영되므로 depfile이 없는 binary/generated 흐름도 입력 변경 시 rebuild된다.

### External tool policy

`qstar.custom_target`은 여전히 `command = qstar.cli { ... }`만 사용한다. `tool = ...`
필드는 되살리지 않는다. 대신 command 첫 argv를 profile capability로 해석한다.

- `tools/gen-source.sh`: package-relative tool이므로 기본 허용.
- `llvm-objcopy`: bare PATH tool이므로 profile `path_tools` allowlist 필요.
- `/opt/tool/bin/objcopy`: absolute tool path이므로 `allow_absolute_tools = true` 필요.
- `tool_overrides`: build file spelling은 유지하고 profile별 실제 tool만 바꿈.

```toml
[profile.rpi5]
path_tools = ["llvm-objcopy", "qemu-system-aarch64"]
tool_overrides = ["llvm-objcopy=tools/fake-objcopy.sh"]
allow_absolute_tools = false
```

`qstar doctor`는 allowlist tool이 PATH에서 발견되는지, override가 package path/absolute
path/PATH tool 중 무엇으로 해석되는지 출력한다.

## Run target

`qstar.run_target`은 build artifact를 만든 뒤 외부 smoke command를 실행한다. QEMU,
UEFI image smoke, serial log marker check 같은 흐름은 별도 built-in keyword가 아니라
`qstar.cli`, `qstar.target_file`, `timeout`, `marker`, `marker_log` 조합으로 표현한다.

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

`marker`는 stdout, stderr, 그리고 선택적 `marker_log` 파일에서 찾는다. QEMU wrapper가
serial output을 `serial.log`에 쓰게 하면 QStar가 그 파일에서 boot marker를 확인할 수
있다. 실패는 세 종류로 분리된다.

- `status=marker-missing`: process는 성공했지만 marker가 없다.
- `status=timeout`: timeout 안에 종료하지 않았다.
- `status=exit-code`: process가 0이 아닌 exit code로 끝났다.

세 경우 모두 `qstar last-failure`와 `qstar replay <action-id>`가 재현 command를
출력한다.

## Freestanding profile과 linker script

커널, 부트로더, 펌웨어 같은 freestanding build는 target rule을 새로 만들기보다
profile로 toolchain 정책을 정한다. `Cale.toml` 또는 `.cale/profiles/<name>.toml`에
아래 값을 둘 수 있다.

```toml
profile = "kernel"

[profile.kernel]
toolchain = "clang"
target = "aarch64-none-elf"
arch = "aarch64"
cpu = "cortex-a76"
abi = "lp64"
freestanding = true
cc = "clang"
linker = "ld.lld"
linker_script = "linker/kernel.ld"
link_options = ["-nostdlib"]
defsyms = ["__kernel_base=0x80000"]
```

`freestanding = true`는 compile action에 `-ffreestanding`, `-fno-builtin`,
`-fno-stack-protector`를 추가한다. `arch = "aarch64"`는
`-mgeneral-regs-only`, `arch = "x86_64"`는 `-mno-red-zone`을 추가한다. `cpu`와
`abi`는 각각 `-mcpu=...`, `-mabi=...`로 내려간다.

Target은 profile link policy를 보강하거나 linker script를 override할 수 있다.

```lua
qstar.executable "kernel" {
  sources = {
    "src/kernel.c",
    "boot/start.S",
  },
  linker_script = "linker/rpi5-aarch64.ld",
  link_options = {
    "-Wl,-Map=kernel.map",
  },
  defsyms = {
    "__stack_top=0x80000",
  },
}
```

`linker_script`는 package-relative path여야 한다. Target `linker_script`가 있으면
profile 값을 덮어쓴다. QStar는 linker script를 link action input으로 추적하므로,
script 내용이 바뀌면 object가 그대로여도 link action은 rebuild된다.
`defsyms`는 항상 `NAME=VALUE` 형식이어야 한다.

## UEFI/PE-COFF link 예시

UEFI를 별도 target kind로 만들지 않는다. `qstar.executable`을 그대로 쓰고,
profile이 PE/COFF linker와 산출물 이름을 고른다.

```toml
profile = "uefi-x64"

[profile.uefi-x64]
toolchain = "clang"
target = "x86_64-pc-windows-msvc"
cc = "clang"
linker = "lld-link"
response_style = "msvc"
artifact_names = ["//:boot=BOOTX64.EFI"]
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

`linker = "lld-link"` 또는 `link.exe` 계열이면 QStar는 output을
`/out:.qstar/out/.../BOOTX64.EFI`로 렌더링한다. AArch64 UEFI profile은 같은
target에 `artifact_names = ["//:boot=BOOTAA64.EFI"]`를 줄 수 있다. Target 안에
`artifact_name = "BOOTLOCAL.EFI"`를 직접 쓰면 profile mapping보다 우선한다.

`artifact_name`과 `artifact_names`의 filename 부분은 slash 없는 basename이어야 한다.
`EFI/BOOT/BOOTX64.EFI`처럼 ESP 안에 배치하는 작업은 install/package/staging rule에서
처리한다.

## Boot package/staging

Round 55부터 boot image나 firmware directory layout은 `qstar.stage`로 표현한다.
`qstar.stage`는 `qstar install`과 다르다. `install`은 prefix 아래 개발 산출물과 public
header를 배치하고, `stage`는 ESP, RPi boot directory, firmware payload 같은 실행용
layout을 copy-only로 만든다.

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

```txt
qstar --file qstar.lua stage //:esp --dry-run
qstar --file qstar.lua stage //:esp
qstar --file qstar.lua stage //:esp --root /tmp/esp   # 금지: absolute root
qstar --file qstar.lua stage //:esp --root stage/dev-esp
```

`qstar.stage_file(src, dst)`에서 `src`는 package-relative file 또는
`qstar.target_file("//:label")`이다. `dst`는 stage root 기준 상대 path다. Dry-run은
실제로 복사하지 않고 `.qstar/stage/<label>/manifest.json`과
would-create/would-update/unchanged diff를 남긴다. 실제 stage는 필요한 target 또는
generated action을 먼저 build한 뒤 copy한다.

## Language namespace

`lang` v0.2 schema:

| Namespace | Fields |
| --- | --- |
| `lang.c` | `include_dirs`, `public_include_dirs`, `private_include_dirs`, `system_include_dirs`, `compile_options`, `defines` |
| `lang.cxx` | `standard`, `include_dirs`, `public_include_dirs`, `private_include_dirs`, `system_include_dirs`, `compile_options`, `defines` |
| `lang.asm` | `include_dirs`, `compile_options`, `preprocess` |
| `lang.cale` | `profile`, `compile_options`, `hcl_include_dirs` |

`lang.rust.include_dirs`처럼 아직 schema가 없는 language namespace와 field는 lint error다.
Rust/Zig/Go 같은 future provider는 include directory 개념을 강제로 상속하지 않는다.

## 제거된 문법

다음 이름은 더 이상 호환 alias가 아니다.

- `qstar.exe`: `qstar.executable` 사용.
- `qstar.genrule`: `qstar.custom_target` 사용.
- `qstar.config_header`: `qstar.configure_file` 사용.
- `qstar.write_config_header`: `qstar.configure_file` 사용.

Target top-level의 `include_dirs`, `public_include_dirs`, `private_include_dirs`,
`system_include_dirs`, `cflags`, `cxxflags`, `cxx_standard`도 제거됐다. 모두 `lang.*`
아래로 옮긴다.
