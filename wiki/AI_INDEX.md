# QStar AI Index

이 문서는 Codex 같은 AI agent가 QStar project를 빠르게 이해하고 안전하게 수정하기 위한
첫 진입점이다. 사람이 처음 배우는 문서는 `README.md`와 `getting-started.md`에서
시작하고, agent는 이 파일을 먼저 읽은 뒤 필요한 reference로 이동한다.

## 1. QStar의 역할

QStar는 C/C++/Cale을 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다.
QStar는 build graph, command plan, local executor, stage/package, run smoke, lint/LSP
authoring UX를 담당한다. C/C++/Cale/HCL의 언어 의미론은 compiler나 language provider가
맡는다.

QStar가 하지 않는 일:

- Cale frontend/backend 내부 API 호출
- HCL semantic import/export 해석
- package fetch, registry, lockfile resolution
- board-specific builtin target 제공
- shell-string command 실행

## 2. 반드시 지킬 authoring surface

- root entry는 `qstar.lua` 하나다.
- project metadata는 `qstar.project { name, version, root = "." }`로 둔다.
- subdir fragment는 `<folder>/<folder>.qst`다.
- `qstar.import_file("path.qst")`는 package-root 기준 `.qst` graph fragment를 읽는다.
- `qstar.import_module("folder/path")`는 `folder/path/path.qsm` helper module table을 읽는다.
- `.qsm` 안에서는 target/profile/project 같은 graph declaration이 금지된다.
- 반복 option은 `qstar.config`와 target `configs = {...}`로 공유한다.
- legacy qs fragment suffix와 `qstar.workspace`는 제거된 surface다.
- 산출물 기본 위치는 `build/qstar`다.
- CLI `-B path`는 `qstar.project.build_dir`보다 우선한다.
- CLI `-G auto`는 현재 `qstar_graph`로 resolve되고, `-G ninja`는 selector만 열린 상태다.
- compile database 기본 위치는 `build/qstar/compile_commands.json`이다.
- external command는 `qstar.cli { ... }` argv-vector로만 표현한다.
- low-level/bootloader-style project도 generic primitive로 표현한다.

## 3. 핵심 rule과 helper

Target/rule:

- `qstar.executable`
- `qstar.staticlib`
- `qstar.sharedlib`
- `qstar.test`
- `qstar.config`
- `qstar.custom_target`
- `qstar.run_target`
- `qstar.group`
- `qstar.configure_file`
- `qstar.stage`
- `qstar.target_family`

Command/path helper:

- `qstar.cli`
- `qstar.input`
- `qstar.output`
- `qstar.target_file`
- `qstar.stage_file`
- `qstar.files`
- `qstar.subdir`
- `qstar.import_file`
- `qstar.import_module`
- `qstar.select`
- `qstar.incompatible`

Profile/toolchain:

- `qstar.profile`
- `extends`
- `target`, `arch`, `abi`, `cpu`
- `cc`, `cxx`, `cale`, `ar`, `linker`
- `sysroot`, `resource_dir`
- `compile_options`, `link_options`, `linker_script`, `defsyms`
- `path_tools`, `tool_overrides`, `response_files`, `response_style`

## 4. 언어별 option 위치

언어별 option은 target top-level에 두지 않는다. 항상 `lang.*` 아래에 둔다.

```lua
qstar.staticlib "core" {
  sources = {"src/core.c"},
  lang = {
    c = {
      public_headers = {"include/core.h"},
      public_include_dirs = {"include"},
      compile_options = {"-Wall"},
    },
  },
}
```

언어 namespace:

- `lang.c`: C headers, include dirs, defines, compile options
- `lang.cxx`: C++ headers, include dirs, standard, modules skeleton, compile options
- `lang.asm`: assembler include dirs, compile options, preprocess flag
- `lang.cale`: Cale/HCL headers, include dirs, Cale profile, modules skeleton

공통 option은 target top-level로 되돌리지 말고 `qstar.config`로 선언한다. Config label은
target의 `configs`에서 참조한다.

```lua
qstar.config "freestanding_c" {
  lang = {
    c = {
      public_include_dirs = {"include"},
      system_include_dirs = {"sysroot/include"},
      compile_options = {"-std=c23", "-ffreestanding"},
    },
  },
}

qstar.staticlib "core" {
  configs = {"//:freestanding_c"},
  sources = {"src/core.c"},
  lang = {
    c = {
      compile_options = {"-DCORE_BUILD=1"},
    },
  },
}
```

Merge rule:

- list field는 `configs` 순서대로 append된다.
- target local list field는 마지막에 append된다.
- scalar field는 뒤의 config가 앞의 config를 override하고 target local scalar가 최종 override한다.
- config는 source, deps, command, output을 만들지 않는다.
- config는 사용하는 target보다 먼저 평가되어야 한다. 보통 policy `.qst`를 먼저 import하고 leaf target fragment를 나중에 읽는다.

## 5. Generated artifact와 stage

Generated file이나 image 변환은 `qstar.custom_target`으로 표현한다.

```lua
qstar.custom_target "image" {
  inputs = {qstar.target_file("//:kernel")},
  outputs = {
    qstar.output("generated/kernel.bin", {
      group = "images",
      format = "raw-binary",
      layout = "generic-kernel",
    }),
  },
  command = qstar.cli {"llvm-objcopy", "-O", "binary", qstar.input(0), qstar.output(0)},
}
```

Staging은 install과 다르다. boot partition, test fixture bundle, package tree 같은
copy-only layout은 `qstar.stage`를 쓴다.

```lua
qstar.stage "boot_image" {
  root = "stage/boot",
  files = {
    qstar.stage_file(qstar.target_file("//:image"), "kernel.bin"),
    qstar.stage_file("boot/config.txt", "config.txt"),
  },
}
```

Deps-only aggregate는 `qstar.group`으로 표현한다. Group은 command, output, artifact,
install 대상이 아니며 `qstar.target_file("//:group")`도 금지된다.

```lua
qstar.group "kernel_parts" {
  deps = {
    "//sys/kern:mm",
    "//sys/dev:drivers",
  },
}
```

## 6. Run smoke

Emulator나 external smoke wrapper는 `qstar.run_target`이다. QStar는 emulator 자체를
깊게 소유하지 않는다.

```lua
qstar.run_target "smoke" {
  deps = {"//:boot_image"},
  command = qstar.cli {"tools/smoke.sh", qstar.target_file("//:image")},
  timeout = 10,
  marker = "BOOT_OK",
  marker_log = "serial.log",
}
```

Failure class는 `marker-missing`, `timeout`, `exit-code`처럼 분리되고,
`qstar last-failure`와 `qstar replay <action-id>`가 재현 정보를 출력한다.

## 7. 안전한 Lua subset

허용:

- `local function`
- `local` 변수
- table literal
- `ipairs`, `pairs`
- `table.insert`
- safe `string.*`

금지:

- global assignment
- `io.open`
- `os.execute`
- `require`
- `load`, `loadfile`, `dofile`
- `debug`, `package`
- process/network/time/random API

Helper module은 Lua `require`가 아니라 `qstar.import_module`로 읽는다.

```lua
local common = qstar.import_module("qstar/modules/common")
qstar.import_file("qstar/policies/warnings.qst")
```

## 8. Agent가 먼저 읽을 파일

일반 authoring:

1. `README.md`
2. `getting-started.md`
3. `concepts/workspace-project-package.md`
4. `concepts/labels-and-fragments.md`
5. `concepts/targets-and-actions.md`
6. `concepts/language-namespaces.md`

Reference:

1. `reference/qstar-lua.md`
2. `reference/modules.md`
3. `reference/configs.md`
4. `reference/target-rules.md`
5. `reference/profiles.md`
6. `reference/custom-target.md`
7. `reference/run-target.md`
8. `reference/diagnostics.md`

Low-level/bootloader-style project:

1. `tutorials/freestanding-image.md`
2. `cookbook/objcopy.md`
3. `cookbook/staging.md`
4. `cookbook/qemu-smoke.md`
5. `cookbook/response-files.md`

## 9. Useful CLI

```sh
qstar docs
qstar docs --ai-index
qstar --file qstar.lua lint //...
qstar --file qstar.lua list-targets --format json
qstar --file qstar.lua explain //:target
qstar --file qstar.lua dry-run //:target
qstar --file qstar.lua build //:target --explain-cache
qstar --file qstar.lua -B out/qstar -G qstar_graph build //:target
qstar --file qstar.lua stage //:bundle --dry-run
qstar --file qstar.lua last-failure
qstar --file qstar.lua replay //:target:action:0
```

## 10. Removed surface

다음은 되살리면 안 된다.

- `Cale.toml`
- `.cale/profiles/*.toml`
- `qstar.toml`
- `qstar.workspace`
- legacy qs fragment suffix
- `qstar.exe`
- `qstar.genrule`
- `qstar.config_header`
- `qstar.write_config_header`
- top-level `include_dirs`, `public_headers`, `cflags`, `cxxflags`, `modules`
- board-specific target/rule builtin
