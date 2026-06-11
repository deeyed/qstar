# QStar 개발자 바이너리

QStar는 Cale package/build graph 방향을 실험하는 독립 개발자용 바이너리다. 현재는 `qstar.lua`를 읽어 query, explain, dry-run, check, 제한적 local build executor를 제공한다. `cale build`의 public 기본 경로로 통합하지 않고, `qstar/` 안의 독립 `Makefile`로 빌드한다.

## 현재 역할

QStar의 목적은 Cale package graph를 deterministic하게 평가하는 것이다.

- target graph dump
- target query
- dependency closure 설명
- authoring check
- dry-run build plan
- 제한적 local executor: C, C++, compiler-driver 기반 `.s`/`.S` assembler,
  generated/config action, test/install
- profile/toolchain resolver 실험

## v0.2 authoring surface

Round 47부터 QStar v0.2 authoring surface는 hard cut이다. 새 project는 다음 API만
정본으로 사용한다. Round 48부터 generated/external command는 shell string이 아니라
`qstar.cli { ... }` argv-vector로 표현한다.

```lua
qstar.executable "app" { ... }
qstar.staticlib "core" { ... }
qstar.sharedlib "plugin" { ... }
qstar.test "unit" { ... }
qstar.custom_target "generated" { ... }
qstar.run_target "smoke" { ... }
qstar.group "aggregate" { ... }
qstar.configure_file "cfg" { ... }
qstar.stage "esp" { ... }
qstar.import_file("qstar/policies/common.qst")
local helper = qstar.import_module("qstar/modules/common")
```

`qstar.exe`, `qstar.genrule`, `qstar.config_header`, `qstar.write_config_header`는
제거됐다. Target top-level의 `include_dirs`, `public_include_dirs`,
`private_include_dirs`, `system_include_dirs`, `public_headers`, `private_headers`,
`modules`, `cflags`, `cxxflags`, `cxx_standard`도 제거됐다. Header/include/compile
option은 항상 `lang.*` 아래에 둔다.

Round 63부터 QStar authoring file은 CMake식 기본 상수와 제한적 Lua helper를 공식
지원한다. `local function`, local 변수, table literal, `ipairs`, `pairs`,
`table.insert`, `string.*`는 허용된다. Global assignment는 error다. 상수는
`QSTAR_VERSION`, `QSTAR_HOST_OS`, `QSTAR_HOST_ARCH`, `QSTAR_PACKAGE_ROOT`,
`QSTAR_PROJECT_ROOT`, `QSTAR_PROFILE`, `QSTAR_TARGET`와 `qstar.version`,
`qstar.host.os`, `qstar.host.arch`, `qstar.project.root`를 제공한다.

Round 74부터 명시적 authoring import가 있다. `qstar.import_file("path.qst")`는
package-root 기준 `.qst` graph fragment를 once-only로 평가한다.
`qstar.import_module("folder/path")`는 folder만 받고 `folder/path/path.qsm`을 읽어
반드시 table을 반환해야 한다. `.qsm` module 안에서는 target/profile/project 같은 graph
declaration이 금지되며 helper 함수, 상수, table export만 둔다.

```lua
qstar.staticlib "core" {
  sources = {"lib/src/core.c"},
  lang = {
    c = {
      public_headers = {"lib/include/core.h"},
      public_include_dirs = {"lib/include"},
      compile_options = {"-ffreestanding"},
      defines = {"CORE_BUILD=1"},
    },
  },
}
```

```lua
qstar.custom_target "generated_value" {
  inputs = {"tools/value.txt"},
  outputs = {qstar.output("generated/value.c")},
  command = qstar.cli {
    "tools/gen-value.sh",
    qstar.input(0),
    qstar.output(0),
  },
}

qstar.run_target "smoke" {
  deps = {"//:app"},
  command = qstar.cli {qstar.target_file("//:app")},
  timeout = 5,
  marker = "OK",
  marker_log = "serial.log",
}
```

상세한 한국어 사용 문서는 `wiki/README.md`에서 시작한다.

## Lua evaluator

QStar evaluator는 `vendor/lua`에 있는 Lua submodule을 사용한다. tag는 `v5.4.8`에
고정되어 있으며, license text는 `LICENSE/lua.txt`에 보존한다. vendored source의
원출처 정보는 license/notice 정책을 따른다.

## 주요 명령

```txt
qstar --file qstar.lua --dump-graph
qstar --version
qstar version
qstar docs
qstar docs --ai-index
qstar --file qstar.lua list-targets
qstar --file qstar.lua list-targets --format json
qstar --file qstar.lua query //:app
qstar --file qstar.lua doctor
qstar --file qstar.lua check //:app
qstar --file qstar.lua lint //...
qstar --file qstar.lua lint //:app --format json
qstar fmt qstar.lua
qstar fmt --check qstar.lua
qstar --file qstar.lua explain //:app
qstar --file qstar.lua dry-run //:app
qstar --file qstar.lua build //:app
qstar --file qstar.lua build //:app --jobs 2 --schedule-trace
qstar --file qstar.lua -B out/qstar -G qstar_graph build //:app
qstar --file qstar.lua --generator ninja list-targets --format json
qstar --file qstar.lua test //:unit
qstar --file qstar.lua test //...
qstar --file qstar.lua install //:app --prefix /tmp/qstar-install --dry-run
qstar --file qstar.lua install //:app --prefix /tmp/qstar-install
qstar --file qstar.lua stage //:esp --dry-run
qstar --file qstar.lua stage //:esp
qstar --file qstar.lua build //:app --explain-cache
qstar --file qstar.lua why-rebuild //:app
qstar --file qstar.lua log //:app
qstar --file qstar.lua last-failure
qstar --file qstar.lua action-log //:app:compile:0
qstar --file qstar.lua replay //:app:compile:0
qstar --file qstar.lua clean --target //:app
qstar --file qstar.lua clean
qstar lsp --stdio
qstar init c-app my-app
qstar init c-lib my-lib
qstar init generated my-generated-app
qstar init mixed-cale my-mixed-app
qstar --file qstar.lua --diagnostics json check //:app
qstar --file qstar.lua --package-alias @core=/path/to/core explain //:app
qstar --file qstar.lua --profile debug --target arm64-apple-macos explain //:app
```

`make install PREFIX=/path`는 `qstar` binary와 함께 manpage, `wiki/`, AI index를
`share/doc/qstar` 아래에 설치한다. `qstar docs`는 설치된 wiki/AI index 위치를 찾기
위한 얇은 entrypoint다.

## 명령 의미

`--dump-graph`는 canonical Graph IR을 출력한다. `explain`은 선택한 target closure를 검증하고 dependency-first order와 action key 재료를 출력한다. `dry-run`은 실행하지 않는 deterministic step record를 만든다. `check`는 package-root 기준 source/header/generated input 존재 여부를 확인한다. `lint`는 LSP가 그대로 소비할 수 있는 `qstar-lint-v1` diagnostic stream을 출력한다. `--format text|json`을 받으며, error diagnostic이 있으면 실패하고 warning만 있으면 성공한다.

`list-targets --format json`은 editor/query tool이 그대로 읽는 `qstar-targets-v1`
JSON object를 출력한다. Target, generated action, test target, installable artifact
목록을 deterministic order로 담고, 각 record에는 label, kind, origin, source/header,
dependency, toolchain, install/test 여부를 포함한다.

`fmt`는 Round 45의 conservative formatter다. `qstar fmt --check qstar.lua`는
rewrite 없이 canonical style drift를 검사하고, `qstar fmt qstar.lua`는 simple
QStar block을 다음 스타일로 정리한다. `.qst` fragment와 `.qsm` helper module도 같은
formatter entrypoint를 쓴다. VSCode extension은 `qstar fmt --stdout`을
사용해 document formatting edit을 만든다.

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

`build`는 제한적 local executor v10이다. package-local generated tool, profile-allowlisted external generated tool, `qstar.configure_file`, C/Cale source compile argv, static archive, exe/test link, deps-only `qstar.group`을 다루며 산출물은 `build/qstar/out`, 로그는 `build/qstar/logs` 아래에 둔다. Round 14/15부터 `build/qstar/state/actions.json` action manifest, `compile_commands.json`, cache-hit skip, `why-rebuild`, `log`, `last-failure`, `clean`, JSON diagnostic skeleton을 제공한다. Round 16/17부터 Cale source는 frontend/backend 내부 API가 아니라 `cale -c ... -o ...` process invocation으로만 다룬다. Round 18/19부터 static library dependency link order, public/private include propagation, system library flag rendering, test runner, install skeleton을 제공한다. Round 29부터 executor는 dependency-first closure를 action DAG로 실행하고, 실패는 stop-on-first-failure로 전파한다. Build action timeout은 기본 30초이며 timeout 시 process를 kill하고 replay file을 남긴다. Round 32부터 `--jobs N`과 `--schedule-trace`를 받는다. `--jobs`를 생략하면 host CPU 수 기반 auto jobs가 적용되고, `jobs > 1`은 같은 target 안의 independent compile action을 process-level parallel batch로 실행한다. Generated action과 final archive/link는 deterministic order를 유지한다. Round 35부터 긴 compile/link command는 profile capability가 허용할 때 실제 `build/qstar/rsp/*.rsp` response file로 내려가며, POSIX/Windows/MSVC style과 digest를 plan/build/replay에 기록한다. Round 36부터 parallel compile은 `process-v2` event stream을 출력한다. Queue order, slot assignment, start/finish/fail/timeout/cancel state, `retry=next-build`, active child cancel propagation을 deterministic하게 기록해 실패 로그를 사람이 읽기 쉽게 한다. Round 37부터 `build/qstar/state/graph.json` graph snapshot과 `build/qstar/state/last-summary.json` 마지막 build summary를 저장하고, `qstar action-log <action-id>`와 `qstar replay <action-id>`로 action 단위 로그/재현 명령을 조회한다. Round 56부터 `run_target`은 stdout/stderr/`marker_log` marker check를 지원하고, QEMU wrapper 실패를 `marker-missing`, `timeout`, `exit-code`로 분리해 `last-failure`와 `replay`에 남긴다. Round 59부터 실패 action은 `qstar-action-diagnostic-v1` JSON line도 출력하며, link, objcopy transform, package/stage, QEMU timeout을 각각 `link-failure`, `objcopy-failure`, `package-failure`, `qemu-timeout`으로 분리한다.

## 아직 하지 않는 일

- remote package fetch
- Ninja generator lowering/execution
- full `.cale` semantic integration
- arbitrary external generator execution without profile allowlist
- full recursive package resolver
- shared library local build
- remote/package install metadata

QStar는 build graph와 command planning을 먼저 안정화하는 단계다. 실제 compiler semantics는 Cale compiler가 맡고, QStar는 source suffix와 toolchain/profile에 따른 command plan을 만든다. `.h`/generated header는 build input으로 추적하지만 QStar가 C/HCL 내용을 해석하지 않는다.

## fragment naming과 lint

Round 61부터 authoring entry 이름은 LSP 준비와 Q# extension 충돌 회피를 위해
hard cut으로 고정된다.

- workspace/package root entry는 `qstar.lua`다.
- `qstar.subdir("foo")`는 `foo/foo.qst`를 canonical fragment로 요구한다.
- `qstar.import_file("path.qst")`는 package-root 기준 `.qst`를 명시적으로 읽는다.
- `qstar.import_module("foo/bar")`는 `foo/bar/bar.qsm`을 helper module로 읽는다.
- `foo/qstar.qst` fallback은 없다. fragment는 항상 `<folder>.qst`다.
- `qstar.project { name = "...", version = "...", root = ".", build_dir =
  "build/qstar", compile_commands = "build" }`가 root metadata와 output policy를
  선언한다. v1에서 `root`는 `"."`만 허용한다.
- `build_dir` 기본값은 `build/qstar`다. state, logs, response files, outputs,
  install/stage manifests, 기본 compile database가 이 directory 아래에 모인다.
- CLI `-B path`는 `qstar.project.build_dir`보다 우선한다. Path는 package-relative여야
  하며 absolute path, `..`, `.`은 거부된다.
- CLI `-G qstar_graph|ninja|auto` 또는 `--generator`는 effective generator를 선택한다.
  현재 action 실행 backend는 `qstar_graph`이며, `auto`도 `qstar_graph`로 resolve된다.
  `ninja`는 selector/metadata surface만 먼저 열렸고 실제 lowering/execution은 deferred다.
- `compile_commands`는 `"build"`(기본, `build/qstar/compile_commands.json`),
  `"root"`(project root의 `compile_commands.json`), `"off"` 중 하나다.
- missing fragment는 `QSTAR002`, root entry 이름 drift는 `QSTAR001`이다.
- unknown/bad label은 `QSTAR010`, package 밖 path는 `QSTAR020`, private include leakage는 `QSTAR030`으로 lint에 통합된다.

`qstar lint --format json`은 단일 JSON object를 출력한다. Schema는
`qstar-lint-v1`이며, 각 diagnostic은 `code`, `severity`, `file`, `line`,
`field`, `label`, `message`를 가진다.

Round 43부터 lint는 build 전 authoring mistake를 더 적극적으로 잡는다.

- `sources`에 header를 넣은 경우 `QSTAR040` warning
- generated public header가 `include/` install surface 밖에 있는 경우 `QSTAR041` warning
- public header를 가진 target이 public include surface를 가진 target을 `private_deps`로만 잡은 경우 `QSTAR042` warning
- 같은 source file이 여러 target에 들어간 경우 `QSTAR043` warning
- C++ source가 있는데 `lang.cxx.standard`가 없는 경우 `QSTAR044` info
- Cale source가 있는데 `toolchain = "cale"` 계열이 아닌 경우 `QSTAR045` warning
- visibility typo는 `QSTAR050` error
- generated output collision은 `QSTAR060` error
- `qstar.subdir()`로 도달하지 않는 orphan `.qst`는 `QSTAR070`/`QSTAR071` warning

## LSP v1

Round 40부터 `qstar lsp --stdio`가 개발용 Language Server Protocol 서버를 연다.
지원 surface는 `initialize`, `shutdown`, `exit`, `textDocument/didOpen`,
`textDocument/didChange`, `textDocument/didSave`, `textDocument/publishDiagnostics`,
`textDocument/hover`, `textDocument/completion`, `textDocument/definition`,
`textDocument/references`, `textDocument/documentSymbol`, `workspace/symbol`이다.

Diagnostics는 현재 파일 경로에서 workspace root와 root `qstar.lua`를 찾고,
기존 lint core와 같은 `QSTAR###` diagnostic을 LSP diagnostic으로 변환한다.
v1은 현재 파일에 해당하는 diagnostic만 publish한다. Hover는 `qstar.executable`,
`qstar.staticlib`, `qstar.test`, `qstar.custom_target`, `qstar.run_target`, `qstar.group`,
`qstar.configure_file`, `lang.*` 주요 field,
그리고 `//pkg:target` label의 kind/origin/source summary를 제공한다.
Completion은 top-level `qstar.*` API와 target field 이름을 제공한다.
Round 44부터 label navigation도 제공한다. `deps = {"//lib:core"}` 같은 canonical
target label에서 definition은 target/genrule 선언 origin으로 이동하고, references는
target이 `deps`/`private_deps`에서 쓰인 위치를 반환한다. Document symbols는 현재
fragment의 target/genrule/config_header를 나열하고, workspace symbols는 graph 전체의
`//pkg:target`/generated action label을 검색한다. `workspace/didChangeWatchedFiles`는
다음 라운드 후보로 남긴다.

## VSCode extension v1

Round 41부터 개발용 VSCode extension skeleton은 `editors/vscode/qstar/`에 둔다.
이 확장은 `qstar.lua`, `*.qst`, `*.qsm`을 `qstar` language id로 연결하고,
`files.associations` 기본값으로 `.qst`와 `.qsm`이 QStar로 잡히게 하며,
syntax highlighting, snippets, LSP client, QStar terminal commands를 제공한다.
LSP client는 `qstar lsp --stdio`만 시작하며, build/test를 자동 실행하지 않는다.
`QStar: Build Target` 같은 명령은 사용자가 명시적으로 실행하는 terminal invocation이다.
다른 확장이나 사용자 설정이 `.qst`/`.qsm`을 계속 선점하면 workspace setting에서
`"files.associations": {"*.qst": "qstar", "*.qsm": "qstar"}`를 명시한다.

Round 42부터 extension은 `list-targets --format json`을 사용해 Explorer 안에
`QStar` tree view를 만든다. Tree는 targets, generated actions, tests,
installable artifacts를 분리해서 보여주고, `build/qstar/state/last-summary.json`이 있으면
마지막 build status를 함께 표시한다. Tree item의 explain, dry-run, build, test는
명시적 command palette/terminal action으로만 실행된다.

QStar extension은 빌드시스템 authoring 파일을 다룬다. C, Cale, HCL 언어 의미론은
각 언어 서버/컴파일러가 담당하며, QStar는 CMake처럼 source path, target graph,
profile, command plan을 편집하고 진단하는 역할로 제한한다.

## source와 generated file

Round 16/17 기준 source policy:

- `.c`는 `host`/`clang`/`cale` toolchain profile에 따라 C compiler invocation으로 낮춘다.
- `.cl`/`.cale`은 `toolchain = "cale"` 또는 `cale-sol`에서만 object-producing compile action으로 낮춘다.
- Cale source는 `cale` process를 호출할 뿐 Cale frontend/backend 내부 API와 연결하지 않는다.
- `.h`는 source kind로 인식하지만 compile source가 아니라 `public_headers`/`private_headers`에 둬야 한다.
- `qstar.custom_target` output은 target `sources` 또는 header list에서 소비될 수 있다.
- `qstar.configure_file`는 package root 아래 `generated/` output만 만들 수 있고, generated header 변경은 dependent compile action cache key에 반영된다.
- `deps`/`public_deps`는 public/interface include directory를 소비자에게 전파한다.
- `private_deps`는 build/link에는 참여하지만 include directory를 소비자에게 전파하지 않는다.
- `libs`, `lib_dirs`, `frameworks`는 target profile별 link flag로 렌더링된다.

예:

```lua
qstar.configure_file "cfg" {
  output = qstar.output("generated/config.h"),
  defines = {"APP_VALUE=42", "HAVE_FEATURE"},
}

qstar.executable "app" {
  sources = {"src/main.c"},
  lang = {
    c = {
      private_headers = {qstar.output("generated/config.h")},
      include_dirs = {"generated"},
    },
  },
}
```

## test, install, stage

`qstar.test`는 test executable target이다. `qstar test //:unit`은 먼저 해당 target을
build한 뒤 `build/qstar/logs/<target>.test.stdout`/`.stderr`에 출력을 저장하고 실행
결과를 보고한다. `qstar test //...`는 package 안의 모든 test target을 순서대로
실행한다. 현재 timeout은 5초 고정이다.

`qstar install`은 v2 skeleton이다. 실제 package fetch나 registry metadata 없이,
이미 build된 local artifact와 public header만 prefix 아래 복사한다. install 실행은
package-local `build/qstar/install/manifest.json`을 남긴다. Manifest에는 exe/staticlib/header
entry, dry-run/copy mode, destination path가 기록된다. Generated public header도
생성 action output이면 install 대상이 될 수 있다. CMake config 같은 export file은
아직 만들지 않고 manifest에 deferred skeleton으로만 표시한다.

Round 55부터 `qstar.stage`는 install과 별개인 copy-only staging/package rule이다.
`install`은 prefix 아래 개발 산출물과 public header를 놓는 흐름이고, `stage`는 boot
image나 firmware package처럼 정해진 directory layout을 만드는 흐름이다. `qstar stage`
는 `build/qstar/stage/<label>/manifest.json`을 남기며, `--dry-run`은 copy하지 않고
would-create/would-update/unchanged diff만 출력한다.

```lua
qstar.test "unit" {
  sources = {"tests/unit.c"},
}

qstar.staticlib "core" {
  sources = {"src/core.c"},
  lang = {
    c = {
      public_headers = {"include/core.h"},
      public_include_dirs = {"include"},
    },
  },
}

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

## manual smoke

```txt
make -C qstar
qstar/build/bin/qstar --file qstar/tests/manual/hello/qstar.lua --dump-graph
qstar/build/bin/qstar --file qstar/tests/manual/hello/qstar.lua list-targets
qstar/build/bin/qstar --file qstar/tests/manual/hello/qstar.lua query //:app
qstar/build/bin/qstar --file qstar/tests/manual/hello/qstar.lua doctor
qstar/build/bin/qstar --file qstar/tests/manual/hello/qstar.lua check //:app
qstar/build/bin/qstar --file qstar/tests/manual/hello/qstar.lua explain //:app
qstar/build/bin/qstar --file qstar/tests/manual/hello/qstar.lua dry-run //:app
qstar/build/bin/qstar --file qstar/tests/manual/generated/qstar.lua build //:app
```

QStar 자체 regression은 다음으로 실행한다.

```txt
make -C qstar check
make -C qstar vscode-extension-tests
make -C qstar qstar-v0-release-tests
make -C qstar qstar-v0.1-release-tests
make -C qstar qstar-v0.2-rc-tests
make -C qstar qstar-release-candidate-tests
make -C qstar qstar-standalone-integration-tests
```

`vscode-extension-tests` checks the QStar VSCode extension package surface,
sample workspace, JSON package drift, and the policy that `node_modules/` and
`.vsix` artifacts are not committed. The extension can be packaged manually
from `qstar/editors/vscode/qstar` with `npm run package:vsix`; generated VSIX
files are release artifacts only.

## v0 seal

Round 20 기준 QStar는 개발용 v0 build system으로 봉인되어 있다. Round 21부터
`qstar init`은 sample corpus 기반 project skeleton을 생성한다. 유지할
`qstar.lua` surface, manual sample corpus, deferred integration 항목은
`docs/qstar-v0-seal.md`에 적는다. Root `Makefile`과 `cale build` integration은
아직 열지 않으며, QStar는 `qstar/Makefile` 안에서 독립적으로 build/check/install된다.

Manual sample:

```txt
qstar/tests/manual/c-only
qstar/tests/manual/generated
qstar/tests/manual/mixed-cale
```

Project corpus:

```txt
qstar/tests/projects/c-app-lib-test
qstar/tests/projects/cxx-mixed
qstar/tests/projects/generated-config
qstar/tests/projects/binary-blob-embed
qstar/tests/projects/multipkg
```

`tests/projects`는 `/tmp` ad-hoc smoke가 아니라 repository 안에 고정된 QStar
real-project corpus다. C app/lib/test, C++ mixed target, generated config/source,
binary blob embed, multi-package dependency, install, rebuild, and
`compile_commands.json` coverage를 한 번에 묶는다.

Round 22부터 source kind와 target kind는 registry 기반 rule model로 분리한다.
자세한 경계는 `docs/rule-model.md`에 둔다.

Round 23/24부터 QStar는 C/C++ compiler depfile을 읽어 header 변경을 compile action
key에 반영하고, `.cc/.cpp/.cxx/.hpp`를 build-system source/header kind로 인식한다.
C++ source가 있는 target은 `c++` 또는 `clang++` linker path를 사용한다. QStar는 C++
문법을 해석하지 않으며 C++ modules는 아직 stable gate다.

Round 61부터 QStar는 `qstar.workspace` marker를 사용하지 않는다. Root discovery는
현재 authoring file에서 위로 올라가며 가장 가까운 `qstar.lua`를 찾는다. 그 directory가
package root이고, 하위 `<folder>.qst`에서 선언한 `:name` target은 `//sub/path:name`
label을 얻는다. Source/header/output path는 계속 package-root 상대 path이며, `../`나
absolute path는 package 밖 참조로 reject된다.

Target은 선언 fragment와 label package가 일치해야 한다. 예를 들어
`pkg/pkg.qst` 안의 `qstar.executable "app"`은 `//pkg:app`을 만들고, 같은 파일에서
`qstar.executable "//other:app"`처럼 다른 package 소유 label을 선언하는 것은 금지된다.

`visibility = {"//...", "//pkg:..."}`는 v1 skeleton으로 들어왔다. Visibility를
명시하지 않은 target은 v0 compatibility를 위해 workspace-local target에서 볼 수
있지만, 명시한 target은 같은 package 또는 visibility pattern에 맞는 consumer만
의존할 수 있다. QStar는 dependency target의 private include directory를 consumer가
직접 include path로 끌어오는 accidental leakage도 authoring diagnostic으로 막는다.

Round 66부터 profile schema는 `qstar.lua` 안의 `qstar.profile` DSL로만 선언된다.
QStar는 별도 mandatory profile config file을 읽지 않는다.
`qstar.profile`은 `cc`, `cxx`, `cale`, `ar`, `linker`, `sysroot`,
`resource_dir`, `include_dirs`, `lib_dirs`, `response_files`, `response_style`을 줄 수
있다. `response_files`는 `auto/on/off` 계열 값을 받고, `response_style`은
`posix/windows/msvc`를 받는다. 기본 style은 target triple에서 추론한다. `qstar doctor`는
이 값을 resolver 결과에 포함해 보여주며, compile/link argv plan에도
sysroot/resource/include/lib/response 설정이 반영된다.

Round 51부터 profile schema는 freestanding/link policy까지 포함한다. `arch`, `cpu`,
`abi`, `freestanding`은 target triple만으로는 부족한 kernel/firmware compile policy를
profile로 고정한다. `freestanding=true`는 `-ffreestanding`, `-fno-builtin`,
`-fno-stack-protector`를 compile argv에 추가하고, arch hint에 따라 `aarch64`는
`-mgeneral-regs-only`, `x86_64`는 `-mno-red-zone`을 추가한다. `link_options`,
`linker_script`, `defsyms`는 profile과 target 양쪽에서 줄 수 있고, target
`linker_script`가 profile 값을 override한다. Package-relative linker script는 link
action input으로 추적되어 script 내용 변경이 link rebuild reason에 반영된다.

Round 52부터 `qstar.custom_target`의 첫 argv는 external tool policy를 따른다.
`tools/gen.sh`처럼 path separator가 있는 package-relative tool은 기본 허용된다.
`llvm-objcopy` 같은 bare PATH tool은 profile의 `path_tools = ["llvm-objcopy"]`에
명시해야 하며, absolute tool path는 `allow_absolute_tools = true`가 있어야만 허용된다.
`tool_overrides = ["llvm-objcopy=tools/fake-objcopy.sh"]`는 authoring surface를
`command = qstar.cli { "llvm-objcopy", ... }`로 유지하면서 profile별 실제 실행 tool을
바꾼다. `qstar doctor`는 allowlist tool의 PATH discovery와 override 상태를 출력한다.

Round 53부터 `qstar.output(path, metadata)`는 ELF에서 raw image를 만들거나 boot
artifact를 분류하는 generated artifact identity를 표현한다. 예를 들어
`qstar.output("generated/kernel8.img", {format = "raw-binary", address = "0x80000",
layout = "rpi5-kernel8"})`는 `images/raw-binary` 산출물로 분류되고, identity에는
path뿐 아니라 group/format/address/layout metadata가 포함된다. QStar는 이 metadata로
`llvm-objcopy` command를 자동 생성하지 않는다. 변환은 여전히
`qstar.custom_target`의 `command = qstar.cli { "llvm-objcopy", "-O", "binary", ... }`
가 수행한다. 같은 package에서 같은 group/format/address/layout을 가진 generated
artifact가 둘 이상 있으면 collision으로 거절한다. `qstar build //:image_rule`처럼
generated action label을 직접 빌드할 수 있고, `qstar.target_file("//:image_rule")`는
그 action의 첫 output path로 해석된다.

Round 57부터 binary blob input을 generated assembly 또는 object로 바꿔 이후 target
입력으로 소비하는 흐름을 지원한다. `qstar.custom_target`이
`generated/embed.S`를 만들고 target `sources`가 그 output을 참조하면 QStar는 먼저
generated action을 실행한 뒤 assembler compile action을 실행한다.
`qstar.output("generated/blob.o", {format = "object"})`는 group을 생략해도 `objects`
group으로 분류되며, target `sources`에서 compile 없이 final archive/link input으로
직접 들어간다. Depfile이 없는 binary fixture와 generated object도 action key의
`input_key`에 content hash로 들어가므로 ELF fixture가 바뀌면 generator와 downstream
compile/link action이 `input-changed`로 rebuild된다.

Round 58부터 `qstar/tests/projects/systems-firmware`가 systems-grade release
corpus다. 이 corpus는 AArch64 freestanding C/ASM/linker script, `llvm-objcopy` raw
image, RPi/ESP staging, UEFI PE/COFF profile output, QEMU wrapper smoke를 모두
QStar의 generic target/rule API로 표현한다. `qstar.target_file("//:kernel")`가
generated command argv에 들어가면 해당 artifact path도 action input으로 추적되어
kernel ELF 변경이 raw image rebuild reason으로 이어진다.

Round 54부터 PE/COFF/UEFI link surface는 dedicated `uefi_app` target kind가 아니라
generic executable + profile link policy로 표현한다. `lld-link`/`link.exe` 계열 linker는
output option을 `/out:<artifact>`로 렌더링하고, UEFI에 필요한
`/subsystem:efi_application`, `/entry:efi_main`, `/nodefaultlib`는 `link_options`에
그대로 둔다. Profile `artifact_names = ["//:boot=BOOTX64.EFI"]`는 target label/name에
따라 산출물 파일명을 바꾸며, target-local `artifact_name = "BOOTLOCAL.EFI"`가 있으면
profile mapping보다 우선한다. `artifact_name`은 파일명 basename만 허용한다. ESP layout
같은 directory staging은 Round 55 `qstar.stage`에서 처리한다.

Command rendering은 shell string이 아니라 argv-vector가 canonical이다. Explain/dry-run
dump는 argv item을 quoting하고 deterministic `digest=`를 붙인다. 긴 command에는
`response=skeleton response_file=build/qstar/rsp/... response_style=... response_digest=...`를
표시한다. 실제 executor는 같은 policy로 response file을 만들며, failure replay에는
`argv_digest`와 response file path/style/digest가 함께 남는다. Action log와
`compile_commands.json`, failure replay는 shell-safe quoting을 사용한다. Action state에는
`argv_key`, `env_key`, `input_key`, `depfile_key`, `profile_key`, `output_key`,
`external_tool_key`가 함께 저장된다.
`why-rebuild`와 `build --explain-cache`는 이를 비교해 `no-previous-state`,
`output-missing`, `output-changed`, `external-tool-changed`, `argv-changed`,
`env-changed`, `depfile-changed`, `input-changed`, `profile-changed`, `key-changed`
reason을 출력한다.

Round 32 scheduler surface는 `--jobs N`과 `--schedule-trace`로 켠다. Round 36 기준
parallel executor는 FIFO source order로 ready queue를 채우고, slot assignment와
action event를 `parallel_batch`, `parallel_slot`, `parallel_event` line으로 남긴다.
Compile child 하나가 fail 또는 timeout되면 아직 실행 중인 compile child는 kill하고,
아직 시작하지 않은 compile action은 queue에 남긴 채 build를 중단한다. Cancel된 action과
failure replay는 다음 build에서 재시도 가능한 상태로 기록한다. Generated action과 final
archive/link action은 아직 순차 실행한다.

## v0.1 hardening seal

Round 38 기준 QStar는 `v0.1 standalone developer build system`으로 봉인되어 있다.
즉, `cale build` 통합 전에도 QStar 자체 binary와 `qstar/Makefile`만으로 local
C/C++/Cale-by-process project를 authoring, build, test, install, rebuild 추적할 수
있어야 한다.

Compatibility contract와 release gate는
`docs/qstar-v0.1-hardening-seal.md`에 둔다. 현재 seal target은 다음이다.

```txt
make -C qstar qstar-v0.1-release-tests
make -C qstar qstar-v0.1-hardening-tests
make -C qstar qstar-standalone-integration-tests
```

이 target들은 QStar-local `check` harness를 통해 manual sample, real project corpus,
executor/profile/install/test/compile database, graph snapshot, action replay, response
file, parallel executor smoke를 함께 검증한다.

## v0.2 release candidate seal

Round 60 기준 v0.2 RC contract는
`docs/qstar-v0.2-release-candidate-seal.md`가 canonical이다. Stable surface는
v0.2 authoring API, `lang.*` language option, local executor, incremental cache,
diagnostics/replay, LSP/VSCode authoring UX, systems firmware corpus를 포함한다.
Experimental surface는 `cale build` integration, remote package resolver, Ninja
generator, full sharedlib executor, HCL semantics, Cale internal API integration으로
분리한다.

```txt
make -C qstar qstar-v0.2-rc-tests
make -C qstar qstar-release-candidate-tests
make -C qstar qstar-full-regression-tests
make -C qstar qstar-systems-corpus-tests
```

## v0.3 release candidate seal

Round 65 기준 v0.3 RC contract는 `docs/qstar-v0.3-seal.md`가 canonical이다.
Stable surface는 `qstar.lua`, `.qst`, `.qsm`, `qstar.project`, `lang.*`, generic `qstar.cli`, staged
package, systems firmware corpus, action replay, LSP/VSCode/lint/formatter UX를
포함한다. Experimental surface는 `cale build` 통합, remote package resolver,
Ninja lowering/execution, full sharedlib executor, Cale internal compiler API, C++ modules,
HCL semantic checking으로 분리한다.

```txt
qstar --version
qstar version
make -C qstar qstar-v0.3-rc-tests
make -C qstar vscode-extension-tests
```

## Submodule extraction prep

QStar는 독립 repository로 분리하고 Cale repository의 `qstar/`를 submodule pin으로
교체할 준비 단계에 있다. 실제 split은 다음 라운드로 미루며, 이번 tree는 standalone
repo가 될 때 필요한 `docs/`, `LICENSE/`, `wiki/`, `tests/`, `editors/vscode/qstar`
surface를 QStar 내부에 보존한다.

자세한 전환 체크리스트는 `docs/qstar-submodule-extraction-prep.md`에 둔다.
