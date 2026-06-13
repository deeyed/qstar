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
- `qstar.import_module`에는 `.qsm` 파일이 아니라 폴더만 넘긴다.
- `.qsm` 안에서는 target/profile/project 같은 graph declaration이 금지된다.
- 반복 option은 `qstar.config`와 target `configs = {...}`로 공유한다.
- 반복 path/list/table 조립은 `qstar.join`, `qstar.copy`, `qstar.append`, `qstar.merge`,
  `qstar.extend`를 쓴다.
- 사용자 정의 build step 문구는 `description = qstar.status("...")`로 지정한다.
  이 field는 `qstar.custom_target`, `qstar.configure_file`, `qstar.run_target`,
  `qstar.stage`에서 지원된다. Raw string, empty string, newline, 240 byte 초과 문자열은
  diagnostic 대상이다.
- Makefile식 `$VAR` 문자열 치환은 없다. Lua `local` 변수와 helper function을 쓴다.
- legacy qs fragment suffix와 `qstar.workspace`는 제거된 surface다.
- 산출물 기본 위치는 `build/qstar`다.
- generated action output 기본 root는 `generated`이고, `qstar.project.generated_dir`로
  package-relative generated root를 바꿀 수 있다.
- QStar가 직접 지원하지 않는 언어 source는 `sources`에 그대로 넣지 않는다. 외부 compiler를
  `qstar.custom_target`으로 호출하고 `qstar.output(path, {format = "object"})`로 `.o` 또는
  `.obj`를 만든 뒤, consuming target의 `sources`에 그 generated object를 넣는다. QStar는
  Objective-C, Rust, Zig, Swift 같은 언어 의미론을 파싱하거나 소유하지 않고 object artifact
  edge만 관리한다.
- CLI `-B path`는 `qstar.project.build_dir`보다 우선한다.
- CLI `-G auto`는 현재 `stella`로 resolve된다.
- CLI `-G ninja build [label]`은 C/C++/ASM compile, `qstar.configure_file`,
  `qstar.custom_target`, staticlib, sharedlib, executable/test link, `qstar.run_target`,
  `qstar.group` phony graph를 Ninja로 실행한다. `stage`와 `install`은 copy와 manifest를
  QStar가 처리하지만, 참조 target artifact는 effective generator로 먼저 build한다.
  Cale source action은 Q116 기준 Stella-only language-provider action이므로 `-G stella`가
  필요하다. Ninja wrapper lowering은 deferred이며, HCL은 QStar가 해석하지 않는
  header-like path다.
  `sharedlib`는 Darwin-like profile에서 `.dylib`, Linux-like profile에서 `.so`를 만들며,
  Windows `.dll`/import-library/PDB 정책은 deferred diagnostic으로 거부한다.
- `make qstar-ninja-backend-parity-tests`는 staticlib, sharedlib, executable/test,
  generated, configure_file, run_target, stage/install producer integration,
  action-log/replay, Windows sharedlib diagnostic, `.ninja_log`/`.ninja_deps` root
  pollution 방지를 확인한다.
- `qstar emit-ninja [label]`은 `build/qstar/ninja/build.ninja`와 policy-controlled
  `compile_commands.json`을 생성한다.
- compile database 기본 위치는 `build/qstar/compile_commands.json`이다.
- QStar repository 자체는 Makefile을 canonical bootstrap/release build path로 유지하면서
  root `qstar.lua` self-host graph를 제공한다. `make qstar-self-host-tests`는
  Makefile-built binary와 Stella/Ninja self-host binary의 `--version` 일치, `//:qstar`
  build, `//:self_host` smoke, compile database, Ninja root pollution 방지를 확인한다.
- Public beta runtime package는 `make qstar-public-beta-release-tests`로 만든다. 이 gate는
  installed binary version, installed wiki/manpages, macOS codesign, prefix-style
  tarball layout, `SHA256SUMS`, VSCode `.vsix` 미포함 정책을 확인한다. GitHub Wiki
  mirror는 `tools/sync-github-wiki.sh`로 수행한다.
- Progress output contract는 `docs/progress-output.md`와 `wiki/reference/progress-output.md`에
  둔다. QStar 0.5 UI line은 `[ 75%] Linking CXX executable app` 같은 CMake-style
  action description을 일반 출력으로 사용하고, legacy scheduler stage wording, `Status: ...`,
  `schedule_action`, `build_action` 같은 내부 trace는 `--verbose`나 `--schedule-trace`로
  제한한다. Warning은 `warning:` prefix를 orange/yellow, error는 bold red로 표시한다.
  `qstar action-log`, `qstar replay`, `qstar last-failure`도 같은 action description을
  `description=` metadata로 보존한다.
- 0.5 readiness 판단과 `0.5.1-beta.1` beta patch release-prep line은
  `docs/qstar-v0.5-readiness.md`에 둔다. 이 문서는 self-host,
  Stella/Ninja benchmark, Ninja parity, Linux/Windows status, docs/CLI drift, medium
  project readiness, version policy, deferred surface를 요약한다.
- Linux host 지원은 Round Q113 기준 validation-backed source build path에
  `linux-x86_64` release-candidate tarball dry-run이 더해진 상태다.
  `make qstar-linux-validation-tests`는 portable path/process, Linux depfile 후보,
  generated_dir, install layout, docs/manpage smoke를 묶는다. macOS에서는 제한된 path
  smoke이고, Linux release asset은 깨끗한 Linux host 또는 CI에서 이 gate와 package
  dry-run이 통과한 뒤에만 추가한다.
- Round Q109의 Linux validation workflow는 `.github/workflows/linux-validation.yml`이다.
  `ubuntu-latest`에서 gcc/clang matrix를 돌리고 `QSTAR_LINUX_VALIDATION_CC`로 실제 depfile
  compiler lane을 고정한다. 각 lane은 Ninja를 설치하고 `make all`, `make check`,
  `make qstar-linux-validation-tests`, install docs/man smoke를 실행한다. gcc lane은
  `QSTAR_RELEASE_PLATFORM=linux-x86_64 tools/package-public-beta.sh`로 ELF x86-64,
  `ldd`, installed docs/wiki/manpages, `SHA256SUMS`를 확인하는 publish 없는 dry-run을
  수행한다.
- Windows host 지원은 아직 official support가 아니다. Round Q114 기준
  `make qstar-windows-prep-tests`가 path/process/MSVC response-file 준비 규칙을 묶고,
  `.github/workflows/windows-validation.yml`은 `workflow_dispatch` 전용 native validation
  candidate다. QStar DSL path는 Windows에서도 `/`로 정규화된 package-relative path이며,
  backslash path와 drive-letter package path는 금지된다. `.exe`는 `artifact_name`으로
  명시할 수 있고, 외부 system library는 MSVC-like target에서 `.lib`로 렌더링한다.
  QStar가 직접 만드는 static `.lib`, `.dll`, import library, PDB, Windows install layout은
  native Windows 검증 전까지 official contract가 아니다.
- `make qstar-medium-project-readiness-tests`는 Stella executor와 Ninja backend의 clean,
  no-op, incremental build 시간을 `medium_project_gate ...` line protocol로 기록한다.
  Round Q92 기준 timing threshold는 report-only가 기본이며,
  `QSTAR_MEDIUM_PERF_REPORT_ONLY=0`이면 hard gate로 승격된다.
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
- `qstar.status`
- `qstar.input`
- `qstar.output`
- `qstar.target_file`
- `qstar.stage_file`
- `qstar.files`
- `qstar.subdir`
- `qstar.import_file`
- `qstar.import_module`
- `qstar.join`
- `qstar.copy`
- `qstar.append`
- `qstar.merge`
- `qstar.extend`
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
- `qstar doctor`는 resolved tool, 누락된 PATH/package tool, sysroot/resource_dir 상태,
  response policy, depfile behavior를 보고한다.

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
- `wiki/reference/language-providers.md`: Cale/Stella/Ninja/HCL backend boundary

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

지원되지 않는 언어가 object file을 만들 수 있다면 object artifact bridge를 쓴다.

```lua
qstar.custom_target "foreign_object" {
  inputs = {"src/foreign_source.ext"},
  outputs = {qstar.output("generated/foreign.o", {format = "object"})},
  command = qstar.cli {"tools/compile-foreign.sh", qstar.input(0), qstar.output(0)},
}

qstar.executable "app" {
  sources = {"src/main.c", qstar.output("generated/foreign.o")},
}
```

이 pattern은 새 language provider가 아니라 artifact bridge다. External compiler는 package-local
wrapper나 profile `path_tools`로 명시적으로 허용한다.

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
- QStar builtin authoring helpers: `qstar.join`, `qstar.copy`, `qstar.append`, `qstar.merge`,
  `qstar.extend`

금지:

- global assignment
- `io.open`
- `os.execute`
- `require`
- `load`, `loadfile`, `dofile`
- `debug`, `package`
- process/network/time/random API

Helper module은 Lua `require`가 아니라 `qstar.import_module`로 읽는다.
문자열 `$VAR` 치환은 없으므로 path와 option은 Lua local 값으로 조립한다.

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
2. `reference/backends.md`
3. `reference/modules.md`
4. `reference/configs.md`
5. `reference/target-rules.md`
6. `reference/profiles.md`
7. `reference/custom-target.md`
8. `reference/object-artifacts.md`
9. `reference/run-target.md`
10. `reference/performance-gates.md`
11. `reference/diagnostics.md`

Low-level/bootloader-style project:

1. `tutorials/freestanding-image.md`
2. `cookbook/objcopy.md`
3. `cookbook/staging.md`
4. `cookbook/qemu-smoke.md`
5. `cookbook/response-files.md`

## 9. Useful CLI

```sh
qstar docs
qstar docs --path
qstar docs --ai-index
qstar docs --show reference/qstar-lua.md
qstar --version
qstar version
qstar help
qstar help build
qstar init c-app hello
qstar --file qstar.lua check //...
qstar --file qstar.lua lint //...
qstar --file qstar.lua fmt --check qstar.lua
qstar --file qstar.lua list-targets --format json
qstar --file qstar.lua query //:target
qstar --file qstar.lua doctor
qstar --file qstar.lua explain //:target
qstar --file qstar.lua dry-run //:target
qstar --file qstar.lua emit-ninja //:target
qstar --file qstar.lua build //:target --explain-cache
qstar --file qstar.lua build //:target --progress plain
qstar --file qstar.lua build //:target --verbose --progress plain
qstar --file qstar.lua build //:target --schedule-trace --progress plain
qstar --file qstar.lua build //:target --progress off --color never
qstar docs --show reference/progress-output.md
qstar --file qstar.lua -B out/qstar -G stella build //:target
qstar --file qstar.lua -G ninja build //:smoke
qstar --file qstar.lua test //...
qstar --file qstar.lua stage //:bundle --dry-run
qstar --file qstar.lua install //:target --prefix /tmp/qstar-install --dry-run
make qstar-linux-validation-tests
make qstar-windows-prep-tests
qstar --file qstar.lua why-rebuild //:target
qstar --file qstar.lua clean --target //:target
qstar --file qstar.lua log //:target
qstar --file qstar.lua last-failure
qstar --file qstar.lua action-log //:target:compile:0
qstar --file qstar.lua replay //:target:action:0
qstar lsp --stdio
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
- `qstar.target_file("//:group")`
- board-specific target/rule builtin
