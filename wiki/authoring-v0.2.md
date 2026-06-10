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

## Run target

`qstar.run_target`은 build artifact를 만든 뒤 외부 smoke command를 실행한다. QEMU,
UEFI image smoke, serial log marker check 같은 흐름은 별도 built-in keyword가 아니라
`qstar.cli`, `qstar.target_file`, `timeout`, `marker` 조합으로 표현한다.

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
}
```

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
