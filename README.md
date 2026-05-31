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

## Lua evaluator

QStar evaluator는 `qstar/vendor/lua`에 있는 Lua submodule을 사용한다. tag는 `v5.4.8`에 고정되어 있으며, license text는 `LICENSE/lua.txt`에 보존한다. vendored source의 원출처 정보는 license/notice 정책을 따른다.

## 주요 명령

```txt
qstar --file qstar.lua --dump-graph
qstar --file qstar.lua list-targets
qstar --file qstar.lua query //:app
qstar --file qstar.lua doctor
qstar --file qstar.lua check //:app
qstar --file qstar.lua explain //:app
qstar --file qstar.lua dry-run //:app
qstar --file qstar.lua build //:app
qstar --file qstar.lua test //:unit
qstar --file qstar.lua test //...
qstar --file qstar.lua install //:app --prefix /tmp/qstar-install --dry-run
qstar --file qstar.lua install //:app --prefix /tmp/qstar-install
qstar --file qstar.lua build //:app --explain-cache
qstar --file qstar.lua why-rebuild //:app
qstar --file qstar.lua log //:app
qstar --file qstar.lua last-failure
qstar --file qstar.lua clean --target //:app
qstar --file qstar.lua clean
qstar init c-app my-app
qstar init c-lib my-lib
qstar init generated my-generated-app
qstar init mixed-cale my-mixed-app
qstar --file qstar.lua --diagnostics json check //:app
qstar --file qstar.lua --package-alias @core=/path/to/core explain //:app
qstar --file qstar.lua --profile debug --target arm64-apple-macos explain //:app
```

## 명령 의미

`--dump-graph`는 canonical Graph IR을 출력한다. `explain`은 선택한 target closure를 검증하고 dependency-first order와 action key 재료를 출력한다. `dry-run`은 실행하지 않는 deterministic step record를 만든다. `check`는 package-root 기준 source/header/generated input 존재 여부를 확인한다.

`build`는 제한적 local executor v4다. package-local generated tool, `qstar.config_header`, C/Cale source compile argv, static archive, exe/test link를 다루며 산출물은 `.qstar/out`, 로그는 `.qstar/logs` 아래에 둔다. Round 14/15부터 `.qstar/state/actions.json` action manifest, `compile_commands.json`, cache-hit skip, `why-rebuild`, `log`, `last-failure`, `clean`, JSON diagnostic skeleton을 제공한다. Round 16/17부터 Cale source는 frontend/backend 내부 API가 아니라 `cale -c ... -o ...` process invocation으로만 다룬다. Round 18/19부터 static library dependency link order, public/private include propagation, system library flag rendering, test runner, install skeleton을 제공한다.

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

## source와 generated file

Round 16/17 기준 source policy:

- `.c`는 `host`/`clang`/`cale` toolchain profile에 따라 C compiler invocation으로 낮춘다.
- `.cale`은 `toolchain = "cale"` 또는 `cale-sol`에서만 object-producing compile action으로 낮춘다.
- Cale source는 `cale` process를 호출할 뿐 Cale frontend/backend 내부 API와 연결하지 않는다.
- `.h`는 source kind로 인식하지만 compile source가 아니라 `public_headers`/`private_headers`에 둬야 한다.
- `qstar.genrule` output은 target `sources` 또는 header list에서 소비될 수 있다.
- `qstar.config_header`는 package root 아래 `generated/` output만 만들 수 있고, generated header 변경은 dependent compile action cache key에 반영된다.
- `deps`/`public_deps`는 public/interface include directory를 소비자에게 전파한다.
- `private_deps`는 build/link에는 참여하지만 include directory를 소비자에게 전파하지 않는다.
- `libs`, `lib_dirs`, `frameworks`는 target profile별 link flag로 렌더링된다.

예:

```lua
qstar.config_header "cfg" {
  output = qstar.output("generated/config.h"),
  defines = {"APP_VALUE=42", "HAVE_FEATURE"},
}

qstar.exe "app" {
  sources = {"src/main.c"},
  private_headers = {qstar.output("generated/config.h")},
  include_dirs = {"generated"},
}
```

## test와 install

`qstar.test`는 test executable target이다. `qstar test //:unit`은 먼저 해당 target을
build한 뒤 `.qstar/logs/<target>.test.stdout`/`.stderr`에 출력을 저장하고 실행
결과를 보고한다. `qstar test //...`는 package 안의 모든 test target을 순서대로
실행한다. 현재 timeout은 5초 고정이다.

`qstar install`은 v1 skeleton이다. 실제 package fetch나 registry metadata 없이,
이미 build된 local artifact와 public header만 prefix 아래 복사한다.

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
make -C qstar qstar-v0-release-tests
```

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

Round 22부터 source kind와 target kind는 registry 기반 rule model로 분리한다.
자세한 경계는 `docs/qstar/rule-model.md`에 둔다.

Round 23/24부터 QStar는 C/C++ compiler depfile을 읽어 header 변경을 compile action
key에 반영하고, `.cc/.cpp/.cxx/.hpp`를 build-system source/header kind로 인식한다.
C++ source가 있는 target은 `c++` 또는 `clang++` linker path를 사용한다. QStar는 C++
문법을 해석하지 않으며 C++ modules는 아직 stable gate다.

Round 25/26부터 QStar는 `qstar.workspace` marker를 workspace root discovery 기준으로
사용한다. Marker가 없으면 기존처럼 `--file qstar.lua`의 directory가 package root다.
Marker가 있으면 그 directory가 workspace root이고, 하위 `qstar.lua`에서 선언한
`:name` target은 `//sub/path:name` label을 얻는다. Source/header/output path는
계속 workspace-root 상대 path이며, `../`나 absolute path는 package 밖 참조로 reject된다.

Target은 선언 fragment와 label package가 일치해야 한다. 예를 들어
`pkg/qstar.lua` 안의 `qstar.exe "app"`은 `//pkg:app`을 만들고, 같은 파일에서
`qstar.exe "//other:app"`처럼 다른 package 소유 label을 선언하는 것은 금지된다.

`visibility = {"//...", "//pkg:..."}`는 v1 skeleton으로 들어왔다. Visibility를
명시하지 않은 target은 v0 compatibility를 위해 workspace-local target에서 볼 수
있지만, 명시한 target은 같은 package 또는 visibility pattern에 맞는 consumer만
의존할 수 있다. QStar는 dependency target의 private include directory를 consumer가
직접 include path로 끌어오는 accidental leakage도 authoring diagnostic으로 막는다.

Round 27/28부터 profile schema는 v2로 확장된다. `Cale.toml` 또는
`.cale/profiles/<name>.toml`은 `cc`, `cxx`, `cale`, `ar`, `linker`, `sysroot`,
`resource_dir`, `include_dirs`, `lib_dirs`를 줄 수 있다. `qstar doctor`는 이 값을
resolver 결과에 포함해 보여주며, compile/link argv plan에도 sysroot/resource/include/lib
설정이 반영된다.

Command rendering은 shell string이 아니라 argv-vector가 canonical이다. Explain/dry-run
dump는 argv item을 quoting하고 deterministic `digest=`를 붙인다. 긴 command에는
`response=skeleton response_file=.qstar/rsp/...`를 표시하지만, 실제 response-file
executor는 아직 열지 않는다. Action log와 `compile_commands.json`, failure replay는
shell-safe quoting을 사용한다.
