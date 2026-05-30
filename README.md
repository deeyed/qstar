# QStar 개발자 바이너리

QStar는 Cale package/build graph 방향을 실험하는 독립 개발자용 바이너리다. 현재는 `qstar.lua`를 읽어 query, explain, dry-run, check, 제한적 local build executor를 제공한다. 아직 `cale build`의 public 기본 경로로 완전히 통합된 상태는 아니다.

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
qstar --file qstar.lua --diagnostic-format line check //:app
qstar --file qstar.lua --package-alias @core=/path/to/core explain //:app
qstar --file qstar.lua --profile debug --target arm64-apple-macos explain //:app
```

## 명령 의미

`--dump-graph`는 canonical Graph IR을 출력한다. `explain`은 선택한 target closure를 검증하고 dependency-first order와 action key 재료를 출력한다. `dry-run`은 실행하지 않는 deterministic step record를 만든다. `check`는 package-root 기준 source/header/generated input 존재 여부를 확인한다.

`build`는 제한적 local executor v1이다. package-local generated tool, C source compile, static archive, exe link를 다루며 산출물은 `.qstar/out`, 로그는 `.qstar/logs` 아래에 둔다.

## 아직 하지 않는 일

- remote package fetch
- cache protocol
- Ninja generator
- full `.cale` source build
- assembly source build
- arbitrary external generator execution
- full recursive package resolver

QStar는 build graph와 command planning을 먼저 안정화하는 단계다. 실제 compiler semantics는 Cale compiler가 맡고, QStar는 source suffix와 toolchain/profile에 따른 command plan을 만든다.

## manual smoke

```txt
make qstar
build/bin/qstar --file qstar/tests/manual/hello/qstar.lua --dump-graph
build/bin/qstar --file qstar/tests/manual/hello/qstar.lua list-targets
build/bin/qstar --file qstar/tests/manual/hello/qstar.lua query //:app
build/bin/qstar --file qstar/tests/manual/hello/qstar.lua doctor
build/bin/qstar --file qstar/tests/manual/hello/qstar.lua check //:app
build/bin/qstar --file qstar/tests/manual/hello/qstar.lua explain //:app
build/bin/qstar --file qstar/tests/manual/hello/qstar.lua dry-run //:app
```

QStar 자체 regression은 다음으로 실행한다.

```txt
make -C qstar check
```
