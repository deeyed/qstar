# QStar 개발자 바이너리

QStar는 Cale package/build graph 방향을 실험하는 독립 개발자용 바이너리다. 현재는 `qstar.lua`를 읽어 query, explain, dry-run, check, 제한적 local build executor를 제공한다. `cale build`의 public 기본 경로로 통합하지 않고, `qstar/` 안의 독립 `Makefile`로 빌드한다.

## 현재 역할

QStar의 목적은 Cale package graph를 deterministic하게 평가하는 것이다.

- target graph dump
- target query
- dependency closure 설명
- authoring check
- dry-run build plan
- 제한적 local executor
- profile/toolchain resolver 실험

## v0.2 authoring surface

Round 47부터 QStar v0.2 authoring surface는 hard cut이다. 새 project는 다음 API만
정본으로 사용한다.

```lua
qstar.executable "app" { ... }
qstar.staticlib "core" { ... }
qstar.sharedlib "plugin" { ... }
qstar.test "unit" { ... }
qstar.custom_target "generated" { ... }
qstar.run_target "smoke" { ... }
qstar.configure_file "cfg" { ... }
```

`qstar.exe`, `qstar.genrule`, `qstar.config_header`, `qstar.write_config_header`는
제거됐다. Target top-level의 `include_dirs`, `public_include_dirs`,
`private_include_dirs`, `system_include_dirs`, `cflags`, `cxxflags`, `cxx_standard`도
제거됐다. Include/compile option은 항상 `lang.*` 아래에 둔다.

```lua
qstar.staticlib "core" {
  sources = {"lib/src/core.c"},
  public_headers = {"lib/include/core.h"},
  lang = {
    c = {
      public_include_dirs = {"lib/include"},
      compile_options = {"-ffreestanding"},
      defines = {"CORE_BUILD=1"},
    },
  },
}
```

상세한 한국어 사용 문서는 `qstar/wiki/README.md`에서 시작한다.

## Lua evaluator

QStar evaluator는 `qstar/vendor/lua`에 있는 Lua submodule을 사용한다. tag는 `v5.4.8`에 고정되어 있으며, license text는 `LICENSE/lua.txt`에 보존한다. vendored source의 원출처 정보는 license/notice 정책을 따른다.

## 주요 명령

```txt
qstar --file qstar.lua --dump-graph
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
qstar --file qstar.lua test //:unit
qstar --file qstar.lua test //...
qstar --file qstar.lua install //:app --prefix /tmp/qstar-install --dry-run
qstar --file qstar.lua install //:app --prefix /tmp/qstar-install
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

## 명령 의미

`--dump-graph`는 canonical Graph IR을 출력한다. `explain`은 선택한 target closure를 검증하고 dependency-first order와 action key 재료를 출력한다. `dry-run`은 실행하지 않는 deterministic step record를 만든다. `check`는 package-root 기준 source/header/generated input 존재 여부를 확인한다. `lint`는 LSP가 그대로 소비할 수 있는 `qstar-lint-v1` diagnostic stream을 출력한다. `--format text|json`을 받으며, error diagnostic이 있으면 실패하고 warning만 있으면 성공한다.

`list-targets --format json`은 editor/query tool이 그대로 읽는 `qstar-targets-v1`
JSON object를 출력한다. Target, generated action, test target, installable artifact
목록을 deterministic order로 담고, 각 record에는 label, kind, origin, source/header,
dependency, toolchain, install/test 여부를 포함한다.

`fmt`는 Round 45의 conservative formatter다. `qstar fmt --check qstar.lua`는
rewrite 없이 canonical style drift를 검사하고, `qstar fmt qstar.lua`는 simple
QStar block을 다음 스타일로 정리한다. VSCode extension은 `qstar fmt --stdout`을
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

`build`는 제한적 local executor v9이다. package-local generated tool, `qstar.configure_file`, C/Cale source compile argv, static archive, exe/test link를 다루며 산출물은 `.qstar/out`, 로그는 `.qstar/logs` 아래에 둔다. Round 14/15부터 `.qstar/state/actions.json` action manifest, `compile_commands.json`, cache-hit skip, `why-rebuild`, `log`, `last-failure`, `clean`, JSON diagnostic skeleton을 제공한다. Round 16/17부터 Cale source는 frontend/backend 내부 API가 아니라 `cale -c ... -o ...` process invocation으로만 다룬다. Round 18/19부터 static library dependency link order, public/private include propagation, system library flag rendering, test runner, install skeleton을 제공한다. Round 29부터 executor는 dependency-first closure를 action DAG로 실행하고, 실패는 stop-on-first-failure로 전파한다. Build action timeout은 기본 30초이며 timeout 시 process를 kill하고 replay file을 남긴다. Round 32부터 `--jobs N`과 `--schedule-trace`를 받는다. `jobs > 1`은 같은 target 안의 independent compile action을 process-level parallel batch로 실행하고, generated action과 final archive/link는 deterministic order를 유지한다. Round 35부터 긴 compile/link command는 profile capability가 허용할 때 실제 `.qstar/rsp/*.rsp` response file로 내려가며, POSIX/Windows/MSVC style과 digest를 plan/build/replay에 기록한다. Round 36부터 parallel compile은 `process-v2` event stream을 출력한다. Queue order, slot assignment, start/finish/fail/timeout/cancel state, `retry=next-build`, active child cancel propagation을 deterministic하게 기록해 실패 로그를 사람이 읽을 수 있게 한다. Round 37부터 `.qstar/state/graph.json` graph snapshot과 `.qstar/state/last-summary.json` 마지막 build summary를 저장하고, `qstar action-log <action-id>`와 `qstar replay <action-id>`로 action 단위 로그/재현 명령을 조회한다.

## 아직 하지 않는 일

- remote package fetch
- Ninja generator
- full `.cale` semantic integration
- assembly source build
- arbitrary external generator execution
- full recursive package resolver
- shared library local build
- remote/package install metadata

QStar는 build graph와 command planning을 먼저 안정화하는 단계다. 실제 compiler semantics는 Cale compiler가 맡고, QStar는 source suffix와 toolchain/profile에 따른 command plan을 만든다. `.h`/generated header는 build input으로 추적하지만 QStar가 C/HCL 내용을 해석하지 않는다.

## fragment naming과 lint

Round 39부터 authoring entry 이름은 LSP 준비를 위해 고정된다.

- workspace/package root entry는 `qstar.lua`다.
- `qstar.subdir("foo")`는 `foo/foo.qs`를 canonical fragment로 요구한다.
- `foo/qstar.qs`는 v0 compatibility fallback으로 평가하지만 `QSTAR003` warning을 낸다.
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
- `qstar.subdir()`로 도달하지 않는 orphan `.qs`는 `QSTAR070`/`QSTAR071` warning

## LSP v1

Round 40부터 `qstar lsp --stdio`가 개발용 Language Server Protocol 서버를 연다.
지원 surface는 `initialize`, `shutdown`, `exit`, `textDocument/didOpen`,
`textDocument/didChange`, `textDocument/didSave`, `textDocument/publishDiagnostics`,
`textDocument/hover`, `textDocument/completion`, `textDocument/definition`,
`textDocument/references`, `textDocument/documentSymbol`, `workspace/symbol`이다.

Diagnostics는 현재 파일 경로에서 workspace root와 root `qstar.lua`를 찾고,
기존 lint core와 같은 `QSTAR###` diagnostic을 LSP diagnostic으로 변환한다.
v1은 현재 파일에 해당하는 diagnostic만 publish한다. Hover는 `qstar.executable`,
`qstar.staticlib`, `qstar.test`, `qstar.custom_target`, `qstar.configure_file`, `lang.*` 주요 field,
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
이 확장은 `qstar.lua`, `*.qs`, `qstar.workspace`를 `qstar` language id로 연결하고,
syntax highlighting, snippets, LSP client, QStar terminal commands를 제공한다.
LSP client는 `qstar lsp --stdio`만 시작하며, build/test를 자동 실행하지 않는다.
`QStar: Build Target` 같은 명령은 사용자가 명시적으로 실행하는 terminal invocation이다.

Round 42부터 extension은 `list-targets --format json`을 사용해 Explorer 안에
`QStar` tree view를 만든다. Tree는 targets, generated actions, tests,
installable artifacts를 분리해서 보여주고, `.qstar/state/last-summary.json`이 있으면
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
  private_headers = {qstar.output("generated/config.h")},
  lang = {
    c = {
      include_dirs = {"generated"},
    },
  },
}
```

## test와 install

`qstar.test`는 test executable target이다. `qstar test //:unit`은 먼저 해당 target을
build한 뒤 `.qstar/logs/<target>.test.stdout`/`.stderr`에 출력을 저장하고 실행
결과를 보고한다. `qstar test //...`는 package 안의 모든 test target을 순서대로
실행한다. 현재 timeout은 5초 고정이다.

`qstar install`은 v2 skeleton이다. 실제 package fetch나 registry metadata 없이,
이미 build된 local artifact와 public header만 prefix 아래 복사한다. install 실행은
package-local `.qstar/install/manifest.json`을 남긴다. Manifest에는 exe/staticlib/header
entry, dry-run/copy mode, destination path가 기록된다. Generated public header도
생성 action output이면 install 대상이 될 수 있다. CMake config 같은 export file은
아직 만들지 않고 manifest에 deferred skeleton으로만 표시한다.

```lua
qstar.test "unit" {
  sources = {"tests/unit.c"},
}

qstar.staticlib "core" {
  sources = {"src/core.c"},
  public_headers = {"include/core.h"},
  public_include_dirs = {"include"},
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
`docs/qstar/qstar-v0-seal.md`에 적는다. Root `Makefile`과 `cale build` integration은
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
qstar/tests/projects/multipkg
```

`tests/projects`는 `/tmp` ad-hoc smoke가 아니라 repository 안에 고정된 QStar
real-project corpus다. C app/lib/test, C++ mixed target, generated config/source,
multi-package dependency, install, rebuild, and `compile_commands.json` coverage를
한 번에 묶는다.

Round 22부터 source kind와 target kind는 registry 기반 rule model로 분리한다.
자세한 경계는 `docs/qstar/rule-model.md`에 둔다.

Round 23/24부터 QStar는 C/C++ compiler depfile을 읽어 header 변경을 compile action
key에 반영하고, `.cc/.cpp/.cxx/.hpp`를 build-system source/header kind로 인식한다.
C++ source가 있는 target은 `c++` 또는 `clang++` linker path를 사용한다. QStar는 C++
문법을 해석하지 않으며 C++ modules는 아직 stable gate다.

Round 25/26부터 QStar는 `qstar.workspace` marker를 workspace root discovery 기준으로
사용한다. Marker가 없으면 기존처럼 `--file qstar.lua`의 directory가 package root다.
Marker가 있으면 그 directory가 workspace root이고, 하위 `<folder>.qs`에서 선언한
`:name` target은 `//sub/path:name` label을 얻는다. Source/header/output path는
계속 workspace-root 상대 path이며, `../`나 absolute path는 package 밖 참조로 reject된다.

Target은 선언 fragment와 label package가 일치해야 한다. 예를 들어
`pkg/pkg.qs` 안의 `qstar.executable "app"`은 `//pkg:app`을 만들고, 같은 파일에서
`qstar.executable "//other:app"`처럼 다른 package 소유 label을 선언하는 것은 금지된다.

`visibility = {"//...", "//pkg:..."}`는 v1 skeleton으로 들어왔다. Visibility를
명시하지 않은 target은 v0 compatibility를 위해 workspace-local target에서 볼 수
있지만, 명시한 target은 같은 package 또는 visibility pattern에 맞는 consumer만
의존할 수 있다. QStar는 dependency target의 private include directory를 consumer가
직접 include path로 끌어오는 accidental leakage도 authoring diagnostic으로 막는다.

Round 27/28부터 profile schema는 v2로 확장된다. `Cale.toml` 또는
`.cale/profiles/<name>.toml`은 `cc`, `cxx`, `cale`, `ar`, `linker`, `sysroot`,
`resource_dir`, `include_dirs`, `lib_dirs`, `response_files`, `response_style`을 줄 수
있다. `response_files`는 `auto/on/off` 계열 값을 받고, `response_style`은
`posix/windows/msvc`를 받는다. 기본 style은 target triple에서 추론한다. `qstar doctor`는
이 값을 resolver 결과에 포함해 보여주며, compile/link argv plan에도
sysroot/resource/include/lib/response 설정이 반영된다.

Command rendering은 shell string이 아니라 argv-vector가 canonical이다. Explain/dry-run
dump는 argv item을 quoting하고 deterministic `digest=`를 붙인다. 긴 command에는
`response=skeleton response_file=.qstar/rsp/... response_style=... response_digest=...`를
표시한다. 실제 executor는 같은 policy로 response file을 만들며, failure replay에는
`argv_digest`와 response file path/style/digest가 함께 남는다. Action log와
`compile_commands.json`, failure replay는 shell-safe quoting을 사용한다. Action state에는
`argv_key`, `env_key`, `input_key`, `depfile_key`, `profile_key`가 함께 저장된다.
`why-rebuild`와 `build --explain-cache`는 이를 비교해 `no-previous-state`,
`output-missing`, `argv-changed`, `env-changed`, `depfile-changed`, `input-changed`,
`profile-changed`, `key-changed` reason을 출력한다.

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
`docs/qstar/qstar-v0.1-hardening-seal.md`에 둔다. 현재 seal target은 다음이다.

```txt
make -C qstar qstar-v0.1-release-tests
make -C qstar qstar-v0.1-hardening-tests
make -C qstar qstar-standalone-integration-tests
```

이 target들은 QStar-local `check` harness를 통해 manual sample, real project corpus,
executor/profile/install/test/compile database, graph snapshot, action replay, response
file, parallel executor smoke를 함께 검증한다.
