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
qstar --file qstar.lua build //:app --explain-cache
qstar --file qstar.lua why-rebuild //:app
qstar --file qstar.lua log //:app
qstar --file qstar.lua last-failure
qstar --file qstar.lua clean --target //:app
qstar --file qstar.lua clean
qstar --file qstar.lua --diagnostics json check //:app
qstar --file qstar.lua --package-alias @core=/path/to/core explain //:app
qstar --file qstar.lua --profile debug --target arm64-apple-macos explain //:app
```

## 명령 의미

`--dump-graph`는 canonical Graph IR을 출력한다. `explain`은 선택한 target closure를 검증하고 dependency-first order와 action key 재료를 출력한다. `dry-run`은 실행하지 않는 deterministic step record를 만든다. `check`는 package-root 기준 source/header/generated input 존재 여부를 확인한다.

`build`는 제한적 local executor v3다. package-local generated tool, `qstar.config_header`, C/Cale source compile argv, static archive, exe link를 다루며 산출물은 `.qstar/out`, 로그는 `.qstar/logs` 아래에 둔다. Round 14/15부터 `.qstar/state/actions.json` action manifest, `compile_commands.json`, cache-hit skip, `why-rebuild`, `log`, `last-failure`, `clean`, JSON diagnostic skeleton을 제공한다. Round 16/17부터 Cale source는 frontend/backend 내부 API가 아니라 `cale -c ... -o ...` process invocation으로만 다룬다.

## 아직 하지 않는 일

- remote package fetch
- Ninja generator
- full `.cale` semantic integration
- assembly source build
- arbitrary external generator execution
- full recursive package resolver

QStar는 build graph와 command planning을 먼저 안정화하는 단계다. 실제 compiler semantics는 Cale compiler가 맡고, QStar는 source suffix와 toolchain/profile에 따른 command plan을 만든다. `.h`/generated header는 build input으로 추적하지만 QStar가 C/HCL 내용을 해석하지 않는다.

## source와 generated file

Round 16/17 기준 source policy:

- `.c`는 `host`/`clang`/`cale` toolchain profile에 따라 C compiler invocation으로 낮춘다.
- `.cale`은 `toolchain = "cale"` 또는 `cale-sol`에서만 object-producing compile action으로 낮춘다.
- Cale source는 `cale` process를 호출할 뿐 Cale frontend/backend 내부 API와 연결하지 않는다.
- `.h`는 source kind로 인식하지만 compile source가 아니라 `public_headers`/`private_headers`에 둬야 한다.
- `qstar.genrule` output은 target `sources` 또는 header list에서 소비될 수 있다.
- `qstar.config_header`는 package root 아래 `generated/` output만 만들 수 있고, generated header 변경은 dependent compile action cache key에 반영된다.

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
```
