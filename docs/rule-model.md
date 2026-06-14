# QStar Rule Model

QStar Round 22부터 build graph 정책과 언어/toolchain 세부 사항을 분리한다.
목표는 QStar를 C, C++, Cale 의미론을 직접 아는 도구가 아니라, 언어에 독립적인
build system으로 유지하는 것이다.

```txt
QStar core:
  label graph
  target closure
  action plan
  cache key
  executor/log/install/test

Rule/provider layer:
  source suffix registry
  target kind registry
  output group
  command rendering hook
  local executor support gate
```

QStar는 `.c`가 C compile input이고 `.cale`이 Cale compile input이라는 정도는
알 수 있다. 하지만 두 언어를 직접 파싱하지 않는다. AST, semantic checking,
module/header 해석, target-specific code generation은 compiler가 소유한다.

## Source Kind Registry

Round 22의 source kind registry는 다음 형태다.

| suffix | language | provider | tool role | output group | local executor |
| --- | --- | --- | --- | --- | --- |
| `.c` | `c` | `c` | `c-compiler` | `objects` | yes |
| `.cc`/`.cpp`/`.cxx` | `cxx` | `cxx` | `cxx-compiler` | `objects` | yes |
| `.cppm`/`.ixx` | `cxx-module` | `cxx` | `cxx-module-scanner` | `modules` | no, stable gate |
| `.h` | `header` | `c` | `header-input` | `headers` | metadata only |
| `.hpp`/`.hh` | `cxx-header` | `cxx` | `header-input` | `headers` | metadata only |
| `.cl`/`.cale` | `cale` | `cale` | `cale-compiler` | `objects` | yes with `toolchain=cale`; Ninja wrapper deferred |
| `.s` | `asm` | `asm` | `assembler` | `objects` | yes with host/clang compiler driver |
| `.S` | `asm-cpp` | `asm` | `preprocessed-assembler` | `objects` | yes with host/clang compiler driver |
| `.o`/`.obj` | `object` | `native` | `link-object` | `objects` | consumed by final archive/link |

이 registry는 사용자가 build rule을 적고 QStar가 plan을 만들기 위한 표면이다.
아직 local executor가 지원하지 않는 조합은 엉뚱한 compiler를 조용히 고르지
않고, 명확한 diagnostic으로 멈춘다.

## Object Artifact Bridge

QStar가 직접 모르는 언어도 외부 compiler가 `.o` 또는 `.obj`를 만들 수 있으면 link graph에
참여할 수 있다. 이때 새 language provider를 추가하지 않고 다음 contract를 쓴다.

```lua
qstar.custom_target "foreign_object" {
  inputs = {"src/foreign_source.ext"},
  outputs = {
    qstar.output("generated/foreign.o", {
      format = "object",
    }),
  },
  command = qstar.cli {"tools/compile-foreign.sh", qstar.input(0), qstar.output(0)},
}

qstar.executable "app" {
  sources = {"src/main.c", qstar.output("generated/foreign.o")},
}
```

`format = "object"`는 generated output의 artifact identity를 object로 표시한다. 기본
output group은 `objects`다. Consuming target의 `sources`에 들어간 `.o`/`.obj`는 compile
input이 아니며, final `archive`, `link`, `link-shared` action의 input으로 직접 들어간다.

이 bridge는 Objective-C, Rust, Zig, Swift 같은 toolchain wrapper에 사용할 수 있지만 QStar가
그 언어의 syntax, module graph, package manager, semantic rule을 이해한다는 뜻은 아니다.
QStar가 맡는 범위는 external argv-vector command, generated output ownership, cache/replay,
그리고 final artifact link edge다.

## Target Rule Registry

Round 22의 target rule registry는 다음 형태다.

| kind | final action | output group | installable | local executor |
| --- | --- | --- | --- | --- |
| `exe` | `link` | `exe` | yes | yes |
| `test` | `link` | `exe` | no | yes |
| `staticlib` | `archive` | `libs` | yes | yes |
| `sharedlib` | `link-shared` | `libs` | yes | yes on Darwin/Linux, Windows deferred |
| `objectlib` | `compile-objects` | `objects` | no | no, prepared |
| `target` | `materialize` | `generic` | no | no |

`sharedlib` dependency를 link하는 executable/test/sharedlib action은 build-tree 실행을
위해 target artifact directory 기준 상대 rpath를 자동으로 받는다. Darwin-like profile은
`@loader_path`, Linux-like profile은 `$ORIGIN` 기반이다.

이렇게 두면 C/Cale-specific 결정이 graph core 안으로 들어오지 않는다. C++를
추가할 때도 label/dependency/cache logic을 다시 쓰지 않고, registry와 command
renderer를 확장하면 된다.

## Boundary

QStar가 compiler-facing contract로 받아도 되는 정보는 다음이다.

- executable path
- argv rendering
- compiler depfile
- depfile path
- output artifact path
- compile database record
- test/install artifact role

QStar가 직접 소비하면 안 되는 언어 내부 정보는 다음이다.

- C/Cale AST
- C preprocessing token semantics
- HCL export/import semantics
- SIR/FIR/BCIR internals
- backend private lowering APIs

Q116 기준 Cale source는 Stella-only language-provider action이다. Ninja wrapper lowering은
Cale provider의 argv, depfile, response-file, replay 계약이 별도 라운드로 봉인되기 전까지
deferred다.

## Depfile Tracking

Round 23은 C/C++ compiler depfile tracking을 추가한다. QStar는 C/C++ compile
action에 `-MMD -MF`를 전달하고, 다음 build부터 생성된 depfile을 읽어
package-relative discovered input을 action key에 포함한다. 이렇게 하면 header가
`public_headers`나 `private_headers`에 직접 적혀 있지 않아도 일반적인 header
수정은 rebuild 원인으로 잡힌다.

이전에 발견된 package-local header가 사라지면 QStar는 오래된 object file을
조용히 재사용하지 않고 `depfile-discovered header` diagnostic을 낸다. System
header는 `-MMD` 정책을 쓰므로 이 v1 depfile key 밖에 둔다.

## C++ Surface v1

Round 24는 C++를 QStar의 build-system 표면으로 연다. QStar가 C++ 언어 의미론을
소유한다는 뜻은 아니다.

```lua
qstar.executable "mixed" {
  sources = {"src/main.c", "src/widget.cpp"},
  lang = {
    c = {
      include_dirs = {"include"},
      compile_options = {"-DAPP_C=1"},
    },
    cxx = {
      include_dirs = {"include"},
      compile_options = {"-DAPP_CXX=1"},
      standard = "c++11",
    },
  },
}
```

`host` profile은 `c++`를, `clang` profile은 `clang++`를 사용한다. 어떤 target이
C++ source를 포함하면 QStar는 그 target을 C++ linker로 연결한다. C++ modules는
scanner/BMI 정책과 더 엄격한 action graph가 필요하므로 아직 gate로 남긴다.
