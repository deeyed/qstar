# QStar v0 Seal

> Current note: Round 47의 현재 authoring 정본은
> `docs/qstar-v0.2-authoring-spec.md`다. 이 문서는 Round 20 v0 contract의
> 역사 기록으로 유지한다.

QStar v0는 Cale repo 안에서 독립적으로 빌드되는 개발용 build system이다. 이 문서는
Round 20 기준으로 당분간 유지할 authoring surface와 아직 안정화하지 않는 범위를
고정한다.

```txt
status: v0 developer build system
binary: qstar/build/bin/qstar
build: make -C qstar
install: make -C qstar install PREFIX=/path
root Makefile integration: none
cale build integration: Round 21 이후 검토
```

QStar는 Cale frontend/backend 내부 API를 호출하지 않는다. C와 Cale source는 process
invocation을 위한 build input으로만 다룬다. `.h`, `.hcl`, generated header는 path와
dependency graph의 일부지만, QStar가 header 내용을 해석하지 않는다.

## Compatibility Contract

v0에서 유지할 surface는 다음이다. 세부 출력 형식은 developer diagnostic text이며
stable public protocol은 아니다.

- `qstar.lua`와 subdir `.qst` fragment 평가.
- `qstar.target`, `qstar.executable`, `qstar.staticlib`, `qstar.test`.
- `qstar.sharedlib`는 local executor에서 stable unsupported 또는 plan-only surface로만 유지.
- `qstar.custom_target`, `qstar.run_target`, `qstar.configure_file`, `qstar.output`,
  `qstar.cli`.
- `qstar init c-app|c-lib|generated|mixed-cale`.
- `qstar.modules`, `qstar.files`, `qstar.join`, `qstar.select`, `qstar.incompatible`,
  `qstar.subdir`.
- labels: `:local`, `//:name`, `//path:name`, `@pkg//path:name`.
- fields: `sources`, `lang`, `deps`, `public_deps`, `private_deps`,
  `toolchain`, `stdlib`, `libs`, `lib_dirs`, `frameworks`. Header/include
  surface는 `lang.c`, `lang.cxx`, `lang.cale` 아래에 둔다.
- commands: `list-targets`, `query`, `doctor`, `check`, `explain`, `dry-run`, `build`,
  `test`, `install`, `why-rebuild`, `log`, `last-failure`, `clean`, `--dump-graph`.
- diagnostics: default text and `--diagnostics json` skeleton.

이 contract는 v0 authoring을 깨뜨리지 않기 위한 최소 약속이다. 출력 text의 공백,
action key hash, 내부 `.qstar/state/actions.json` schema는 아직 public cache protocol이
아니다.

## Sample Corpus

Round 20 manual corpus는 QStar를 손으로 써볼 수 있게 하는 작은 project set이다.

| sample | 목적 |
| --- | --- |
| `qstar/tests/manual/c-only` | C static library, executable, test target, install flow |
| `qstar/tests/manual/generated` | generated config header와 generated C source chaining |
| `qstar/tests/manual/mixed-cale` | C/Cale mixed target의 dry-run command plan |

권장 manual loop:

```txt
make -C qstar
tmp=$(mktemp -d /tmp/qstar-sample.XXXXXX)
cp -R qstar/tests/manual/c-only "$tmp/c-only"
cd "$tmp/c-only"
/Users/gungye/workspace/Cale/qstar/build/bin/qstar --file qstar.lua build //:app
/Users/gungye/workspace/Cale/qstar/build/bin/qstar --file qstar.lua test //:unit
/Users/gungye/workspace/Cale/qstar/build/bin/qstar --file qstar.lua install //:core --prefix "$tmp/install"
```

Generated sample:

```txt
tmp=$(mktemp -d /tmp/qstar-generated.XXXXXX)
cp -R qstar/tests/manual/generated "$tmp/generated"
cd "$tmp/generated"
rm -rf .qstar generated compile_commands.json
/Users/gungye/workspace/Cale/qstar/build/bin/qstar --file qstar.lua build //:app
```

Mixed C/Cale sample은 현재 dry-run 중심이다.

```txt
tmp=$(mktemp -d /tmp/qstar-mixed.XXXXXX)
cp -R qstar/tests/manual/mixed-cale "$tmp/mixed-cale"
cd "$tmp/mixed-cale"
/Users/gungye/workspace/Cale/qstar/build/bin/qstar --file qstar.lua dry-run //:mixed
```

## qstar init

Round 21부터 `qstar init`은 manual sample corpus와 같은 authoring skeleton을 만든다.
현재 template은 `c-app`, `c-lib`, `generated`, `mixed-cale`이다. 기존 파일은 덮어쓰지
않는다.

```txt
qstar init c-app my-app
qstar init c-lib my-lib
qstar init generated my-generated-app
qstar init mixed-cale my-mixed-app
```

`qstar init`은 package registry, remote dependency, workspace policy를 만들지
않는다. Project/profile metadata는 `qstar.lua` 안의 QStar DSL로만 선언한다.

## Release Gate

v0 release gate는 `qstar/Makefile` 안에서만 제공한다.

```txt
make -C qstar qstar-v0-release-tests
```

이 target은 QStar 자체 build, smoke, manual sample copy/build/test/install,
`compile_commands.json` validation, clean rebuild, docs/examples drift check를 묶는다.
Root `make qstar-tests`와 `cale build` integration은 의도적으로 제공하지 않는다.

## v0.1 Hardening Seal

Round 38 기준 현재 hardening contract는
`docs/qstar-v0.1-hardening-seal.md`가 canonical이다. v0.1은 v0 authoring
surface를 유지하면서 executor/profile/install/test/compile database, response file,
parallel executor, persistent graph snapshot, action replay UX, sample corpus release
gate를 하나로 묶는다.

```txt
make -C qstar qstar-v0.1-release-tests
make -C qstar qstar-standalone-integration-tests
```

이 seal은 `cale build` 통합 전에도 QStar를 독립 개발용 빌드시스템으로 사용할 수
있다는 기준을 고정한다.

## v0.2 Release Candidate Seal

Round 60 기준 현재 release-candidate contract는
`docs/qstar-v0.2-release-candidate-seal.md`가 canonical이다. v0.2 RC는
hard-cut authoring surface, local executor, diagnostics/replay, editor UX, project
corpus를 하나의 QStar-local gate로 묶는다.

```txt
make -C qstar qstar-v0.2-rc-tests
make -C qstar qstar-release-candidate-tests
```

## Deferred

- `cale build` 내부 QStar 사용.
- root `Makefile` integration.
- package registry/fetch/cache.
- Ninja generator.
- full shared library executor.
- remote cache protocol.
- HCL parsing/import/export.
- Cale compiler frontend/backend internal API integration.
