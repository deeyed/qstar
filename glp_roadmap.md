# QStar GLP Roadmap

이 문서는 QStar의 다음 장기 설계인 GLP(Generic Language Provider)를 정리한다.
현재 QStar는 `qstar.toolset`, `qstar.config`, target, generated action, stage를 중심으로
generic build system 표면을 갖는다. 다만 언어 option 표면은 아직 `lang.c`,
`lang.cxx`, `lang.asm`처럼 beginner-friendly builtin namespace로 남아 있다.
Q200부터 toolset compiler role은 `tools.c.compiler`처럼 provider namespace table로
표현하고, `tools.c = qstar.cli {...}` 같은 직접 compiler slot은 제거한다.
Q201부터 C/C++/ASM source classification은 preloaded built-in provider registry의
`c`, `cxx`, `asm` namespace 위에서 동작한다.

GLP의 목표는 기존 초보자 친화적인 C/C++/ASM 문법을 유지하면서도, QStar core가 특정
언어를 직접 알지 않고 새 언어를 provider package로 추가할 수 있게 만드는 것이다.

## 현재 상태

현재 public DSL의 핵심은 다음과 같다.

- `qstar.project`: project metadata와 output policy.
- `qstar.toolset`: compiler, archiver, linker, response-file, external tool policy.
- `qstar.config`: 여러 target이 공유하는 option bundle.
- artifact targets: `qstar.executable`, `qstar.staticlib`, `qstar.sharedlib`, `qstar.test`.
- generated actions: `qstar.custom_target`, `qstar.configure_file`.
- utility rules: `qstar.run_target`, `qstar.group`, `qstar.stage`, `qstar.target_family`.
- imports: `qstar.import_file`, `qstar.import_module`, `qstar.subdir`.
- helpers: `qstar.cli`, `qstar.status`, `qstar.input`, `qstar.output`,
  `qstar.target_file`, `qstar.stage_file`, `qstar.files`, `qstar.join`,
  `qstar.copy`, `qstar.append`, `qstar.merge`, `qstar.extend`.

현재 `qstar.toolset`의 direct core role은 `archive`, `link`다. Compiler tool은
provider namespace table 아래에 둔다.

```lua
qstar.toolset "host" {
  tools = {
    c = { compiler = qstar.cli {"cc"} },
    cxx = { compiler = qstar.cli {"c++"} },
    asm = { compiler = qstar.cli {"cc"} },
    archive = qstar.cli {"ar"},
    link = qstar.cli {"cc"},
  },
  response_files = "auto",
  response_style = "posix",
  path_tools = {"python3"},
}
```

현재 `lang` namespace도 고정 allowlist다.

```lua
qstar.config "common_c" {
  lang = {
    c = {
      public_include_dirs = {"include"},
      compile_options = {"-Wall", "-Wextra"},
    },
    cxx = {
      standard = "c++20",
      compile_options = {"-Wall"},
    },
    asm = {
      preprocess = true,
      compile_options = {"-x", "assembler-with-cpp"},
    },
  },
}
```

이 문법은 유지한다. 초보자는 계속 이 표면으로 시작할 수 있어야 한다. GLP는 이 표면을
깨는 프로젝트가 아니라, 이 표면을 내부적으로 provider model 위에 올리고 동적 언어를
추가하는 프로젝트다.

## GLP란 무엇인가

GLP는 언어를 QStar core builtin syntax가 아니라 build graph를 확장하는 provider package로
취급하는 설계다.

QStar core가 아는 것은 다음 범위로 제한한다.

- source unit
- generated output
- object artifact
- archive artifact
- linked artifact
- dependency edge
- tool role argv
- response-file materialization
- depfile/discovered input
- action key, log, replay
- backend lowering contract

언어 provider가 아는 것은 다음 범위다.

- source suffix와 source unit 종류
- provider 전용 option schema
- provider 전용 tool role
- compile argv 구성
- depfile 형식
- emitted artifact format
- provider helper API

즉 QStar core는 Rust, Zig, Swift, Cale, shader language의 문법이나 package manager를 알지
않는다. Core는 "이 provider가 이 source unit을 object artifact로 낮춘다"는 계약만 안다.

## 왜 generic language 설계로 가야 하는가

Meson, CMake, Xmake는 모두 언어 이름과 compiler role을 public surface에 직접 노출한다.
이것은 실용적이고 표준적인 방식이지만, 언어가 늘어날수록 core가 계속 커진다.

QStar가 다음 단계에서 앞서가려면 언어별 하드코딩을 늘리지 않아야 한다. Rust를 넣기 위해
`lang.rust`, Zig를 넣기 위해 `lang.zig`, Cale을 넣기 위해 `lang.cale`을 core에 계속
추가하면, 결국 QStar core가 "범용 build kernel"이 아니라 "많은 언어를 직접 아는 큰
build system"이 된다.

GLP는 이 경계를 바꾼다.

- 기존 `lang.c`는 계속 쓴다.
- 그러나 내부적으로는 built-in C provider가 제공하는 namespace처럼 다룬다.
- 새 언어는 core patch 없이 provider package로 추가한다.
- 사용자는 provider string을 직접 다루지 않고, language module helper를 사용한다.
- backend는 provider가 만든 source/action contract를 공통 방식으로 실행한다.

## 기본 원칙

- 기존 `lang.c`, `lang.cxx`, `lang.asm` 문법은 유지한다.
- `tools.archive`, `tools.link`는 core direct role로 유지한다.
- `tools.c = qstar.cli {...}`, `tools.cxx = qstar.cli {...}`,
  `tools.asm = qstar.cli {...}` 직접 compiler role은 제거한다.
- Compiler tool은 `tools.c.compiler`, `tools.cxx.compiler`, `tools.asm.compiler`처럼
  provider namespace table로 표현한다.
- `.qsm` helper module의 기존 철학은 유지한다.
- 일반 `.qsm`은 target/config/toolset을 선언하지 않는 helper table이다.
- language provider는 일반 helper module과 구분되는 provider package다.
- provider 사용은 `qstar.use_language(...)`로 명시한다.
- `qstar.import_module(...)`이 조용히 graph semantics를 바꾸게 하지 않는다.
- provider 구현은 shell string이 아니라 QStar argv/context API를 통해 action을 만든다.
- provider option은 schema 검증을 받는다.
- 알 수 없는 `lang.<namespace>`와 알 수 없는 provider option은 diagnostic이어야 한다.

## 권장 프로젝트 구조

Standard provider를 쓰는 일반 프로젝트:

```txt
qstar.lua
src/
  main.c
  math.zig
```

Project-local provider를 vendoring하는 프로젝트:

```txt
qstar.lua
qstar/
  languages/
    zig/
      zig.qsm
      provider.lua
  policies/
    common.qst
  modules/
    paths/
      paths.qsm
src/
  main.c
  math.zig
```

`qstar/modules`는 기존 helper module 공간으로 유지한다. `qstar/languages`는 language
provider package 공간으로 둔다. 두 공간을 섞지 않는다.

## Provider Package 구조

GLP provider package는 다음 두 파일을 기본 단위로 한다.

```txt
qstar/languages/zig/
  zig.qsm
  provider.lua
```

`zig.qsm`은 provider manifest다. 이 파일은 QStar가 읽고 검증할 수 있는 선언적 schema를
담는다. `provider.lua`는 실제 provider 구현이다. 이 파일은 일반 Lua `require` 대상이
아니라 QStar provider loader가 제한된 sandbox에서 읽는 provider implementation이다.

## zig.qsm 예시

```lua
return qstar.language_provider {
  api = "qstar.lang/1",
  id = "zig",
  version = "0.1.0",

  namespace = "zig",
  implementation = "provider.lua",

  tools = {
    compiler = {
      role = "zig.compiler",
      required = true,
    },
  },

  units = {
    object = {
      suffixes = {".zig"},
      emits = "object",
      lower = "compile_object",
      deps = "none",
    },
  },

  options = {
    target = {
      type = "string",
    },
    optimize = {
      type = "enum",
      values = {"Debug", "ReleaseSafe", "ReleaseFast", "ReleaseSmall"},
      default = "Debug",
    },
    compile_options = {
      type = "list",
      default = {},
    },
  },

  exports = {
    tools = "tools",
    options = "options",
    object = "object",
  },
}
```

Manifest의 고정 key와 provider-defined key를 구분한다.

| 구분 | 예시 | 소유자 |
| --- | --- | --- |
| Manifest envelope | `api`, `id`, `version`, `namespace`, `implementation` | QStar core |
| Tool schema root | `tools` | QStar core |
| Unit schema root | `units` | QStar core |
| Option schema root | `options` | QStar core |
| Export schema root | `exports` | QStar core |
| Provider tool name | `compiler` | provider |
| Provider role | `zig.compiler` | provider가 선언, QStar가 저장/조회 |
| Provider unit name | `object` | provider |
| Provider option | `target`, `optimize`, `compile_options` | provider |
| Provider export | `tools`, `options`, `object` | provider |

## provider.lua 예시

```lua
local P = {}

function P.tools(t)
  return qstar.provider_tools("zig", {
    compiler = t.compiler,
  })
end

function P.options(t)
  return qstar.language_options("zig", t or {})
end

function P.object(path, opts)
  return qstar.source(path, qstar.merge({
    language = "zig",
    unit = "object",
  }, opts or {}))
end

function P.compile_object(ctx)
  local argv = qstar.argv()

  argv:add(ctx.tool("zig.compiler"))
  argv:add("build-obj")
  argv:add(ctx.input("source"))
  argv:add("-femit-bin=" .. ctx.output("object"))

  if ctx.option("target") then
    argv:add("-target")
    argv:add(ctx.option("target"))
  end

  if ctx.option("optimize") then
    argv:add("-O")
    argv:add(ctx.option("optimize"))
  end

  argv:add_all(ctx.option("compile_options"))

  return {
    command = argv,
    inputs = {ctx.input("source")},
    outputs = {ctx.output("object")},
  }
end

return P
```

`provider.lua`는 provider author용 API다. 일반 사용자가 Zig를 쓰기 위해 이 파일을 매번
작성하게 해서는 안 된다. 일반 사용자는 QStar standard provider를 쓰거나, 프로젝트에
vendored provider를 넣어두고 `qstar.use_language(...)`만 호출한다.

## 사용자 코드 예시

Project-local Zig provider를 쓰는 예시:

```lua
local zig = qstar.use_language("qstar/languages/zig")

qstar.project {
  name = "mixed-zig-c",
  root = ".",
  build_dir = "build/qstar",
  generated_dir = "build/qstar/generated",
}

qstar.toolset "host" {
  tools = qstar.merge({
    c = { compiler = qstar.cli {"cc"} },
    archive = qstar.cli {"ar"},
    link = qstar.cli {"cc"},
  }, zig.tools {
    compiler = qstar.cli {"zig"},
  }),
}

qstar.config "debug" {
  toolset = "//:host",
  lang = {
    c = {
      public_include_dirs = {"include"},
      compile_options = {"-Wall", "-Wextra"},
    },
    zig = zig.options {
      optimize = "Debug",
      compile_options = {"-fPIC"},
    },
  },
}

qstar.executable "app" {
  configs = {"//:debug"},
  sources = {
    "src/main.c",
    zig.object("src/math.zig"),
  },
}
```

Standard provider bundle을 쓰는 예시:

```lua
local zig = qstar.use_language("zig")
```

이 경우 QStar는 설치된 standard provider path에서 `zig/zig.qsm`을 찾는다. Project-local
provider와 standard provider resolution은 같은 manifest contract를 사용한다.

## 장기 Toolset 문법

장기적으로는 다음 문법이 가능해야 한다.

```lua
qstar.toolset "host" {
  tools = {
    archive = qstar.cli {"ar"},
    link = qstar.cli {"cc"},
    zig = zig.tools {
      compiler = qstar.cli {"zig"},
    },
  },
}
```

이 문법은 `zig` provider namespace 아래 tool role들을 묶어준다. 내부적으로는 다음 role map과
같은 의미를 가진다.

```lua
qstar.toolset "host" {
  tools = {
    archive = qstar.cli {"ar"},
    link = qstar.cli {"cc"},
    ["zig.compiler"] = qstar.cli {"zig"},
  },
}
```

초기 GLP 단계에서는 다음 문법을 먼저 허용해도 된다.

```lua
qstar.toolset "host" {
  tools = qstar.merge({
    archive = qstar.cli {"ar"},
    link = qstar.cli {"cc"},
  }, zig.tools {
    compiler = qstar.cli {"zig"},
  }),
}
```

`tools.rust = qstar.cli {"rustc"}` 같은 단일 field는 권장하지 않는다. Rust나 Zig provider가
언제나 compiler 하나만 필요하다고 가정할 수 없기 때문이다. Provider namespace 아래에 여러
tool을 둘 수 있게 해야 한다.

```lua
qstar.toolset "rust_host" {
  tools = {
    archive = qstar.cli {"ar"},
    link = qstar.cli {"cc"},
    rust = rust.tools {
      compiler = qstar.cli {"rustc"},
      doc = qstar.cli {"rustdoc"},
      bindgen = qstar.cli {"bindgen"},
    },
  },
}
```

## 기존 Toolset 전환 계획

Q200 hard cut 기준으로 direct compiler role은 제거됐다.

| 이전 문법 | 현재 role | 상태 |
| --- | --- | --- |
| `tools.c = qstar.cli {...}` | `tools.c.compiler` | 제거, diagnostic |
| `tools.cxx = qstar.cli {...}` | `tools.cxx.compiler` | 제거, diagnostic |
| `tools.asm = qstar.cli {...}` | `tools.asm.compiler` | 제거, diagnostic |
| `tools.archive = qstar.cli {...}` | `archive` | 유지 |
| `tools.link = qstar.cli {...}` | `link` | 유지 |

전환 단계:

1. 내부 Graph IR에 dynamic tool role map을 둔다.
2. direct compiler role은 diagnostic으로 거절한다.
3. built-in C/C++/ASM compiler는 provider namespace role로 저장한다.
4. `qstar.use_language(...)`가 provider role을 등록한다.
5. `tools.zig = zig.tools { ... }`를 정식 provider bundle 문법으로 확장한다.
6. explain/doctor/JSON 출력은 provider role name을 보여준다.

## GLP Builtin과 Provider Flexibility

| 영역 | QStar core builtin | Provider가 유연하게 정의 |
| --- | --- | --- |
| Provider loading | `qstar.use_language(path_or_id)` | provider id와 배포 위치 |
| Manifest schema | `api`, `id`, `version`, `namespace`, `implementation` | 각 field의 값 |
| Tool map | argv-vector 저장, role lookup, response file policy | `zig.compiler`, `rust.compiler`, `cale.compiler` |
| Toolset policy | `response_files`, `response_style`, `path_tools`, `allow_absolute_tools` | provider-specific tool helper |
| Lang namespace | `lang.<namespace>` 동적 등록과 merge rule | `zig`, `rust`, `cale` namespace |
| Option validation | type, default, enum, list validation | `edition`, `cfg`, `externs`, `target`, `optimize` |
| Source token | `qstar.source(...)` token, path validation | `zig.object`, `rust.crate`, `cale.module` |
| Artifact contract | `object`, generated file, linked artifact | 어떤 unit이 어떤 artifact를 emit하는지 |
| Lowering context | `ctx.tool`, `ctx.input`, `ctx.output`, `ctx.option` | 실제 argv 구성 |
| Backend contract | action inputs/outputs, depfile, action key, replay/log | provider별 depfile와 discovered input policy |
| Diagnostics | missing provider, missing tool, unknown namespace, unknown option | provider-specific option message |
| Distribution | standard provider lookup, project-local provider lookup | provider package content |

## 금지 설계

다음 설계는 피한다.

- `qstar.import_module(...)`만 호출했는데 language provider가 자동 등록되는 방식.
- 일반 `.qsm` helper module과 language provider package를 같은 개념으로 섞는 방식.
- `qstar/modules/language/zig.lua`처럼 기존 module 규칙을 우회하는 flat Lua file 특별취급.
- provider가 shell string을 만들어 반환하는 방식.
- provider option schema 없이 `lang.zig`의 임의 key를 모두 허용하는 방식.
- QStar core에 `lang.rust`, `lang.zig`, `lang.swift`, `lang.cale`을 계속 하드코딩하는 방식.
- `tools.rust = qstar.cli {"rustc"}`만 표준으로 삼아 provider의 여러 tool을 막는 방식.
- provider가 외부 package manager나 compiler internal API를 QStar core 책임으로 끌어들이는 방식.
- 원격 provider fetch를 초기 GLP에 넣는 방식.

## 권장 설계

다음 설계를 권장한다.

- `qstar.use_language("zig")`로 standard provider를 명시적으로 활성화한다.
- `qstar.use_language("qstar/languages/zig")`로 project-local provider를 활성화한다.
- provider package는 `<dir>/<id>.qsm` manifest와 `provider.lua` implementation을 가진다.
- 기존 `.qsm` helper module 규칙은 유지한다.
- provider manifest는 QStar가 schema 검증한다.
- provider implementation은 restricted provider sandbox에서 실행한다.
- provider helper는 사용자에게 `zig.options`, `zig.object`, `zig.tools`처럼 의미 있는 API를
  제공한다.
- 사용자는 provider string이 아니라 provider module value를 다룬다.
- toolset은 provider namespace nested tools를 받는다.
- C/C++/ASM은 built-in provider namespace로 preloaded하고 direct compiler slot alias는
  유지하지 않는다.

## 현재 Object Bridge와의 관계

GLP가 들어오기 전까지 외부 언어의 정본 경로는 여전히 object artifact bridge다.

```lua
qstar.custom_target "zig_object" {
  inputs = {"src/math.zig"},
  outputs = {
    qstar.output("build/qstar/generated/zig/math.o", {
      format = "object",
    }),
  },
  command = qstar.cli {
    "zig",
    "build-obj",
    qstar.input(0),
    "-femit-bin=" .. qstar.output(0),
  },
}

qstar.executable "app" {
  sources = {
    "src/main.c",
    qstar.output("build/qstar/generated/zig/math.o"),
  },
}
```

GLP는 이 boilerplate를 provider가 안전하게 흡수하는 것이다.

```lua
local zig = qstar.use_language("zig")

qstar.executable "app" {
  sources = {
    "src/main.c",
    zig.object("src/math.zig"),
  },
}
```

## 최종 목표 문법

최종적으로 사용자가 보게 될 문법은 다음 정도가 되어야 한다.

```lua
local zig = qstar.use_language("zig")

qstar.project {
  name = "app",
  root = ".",
  build_dir = "build/qstar",
  generated_dir = "build/qstar/generated",
}

qstar.toolset "host" {
  tools = {
    archive = qstar.cli {"ar"},
    link = qstar.cli {"cc"},
    zig = zig.tools {
      compiler = qstar.cli {"zig"},
    },
  },
  response_files = "auto",
  response_style = "posix",
}

qstar.config "debug" {
  toolset = "//:host",
  lang = {
    zig = zig.options {
      optimize = "Debug",
      compile_options = {"-fPIC"},
    },
  },
}

qstar.executable "app" {
  configs = {"//:debug"},
  sources = {
    zig.object("src/main.zig"),
  },
}
```

이 문법에서 `zig`는 문자열 provider id가 아니라 `qstar.use_language("zig")`가 반환한
provider module value다. 일반 사용자는 provider lowering이나 internal role을 몰라도 된다.

## 배포 전략

초기 GLP에는 원격 fetch나 QStar package manager를 넣지 않는다.

권장 순서:

1. Built-in C/C++/ASM provider를 내부 모델로 분리한다.
2. Project-local provider loading을 구현한다.
3. QStar 설치물에 standard provider bundle을 포함한다.
4. Zig나 Rust처럼 object emission 경계가 비교적 명확한 provider를 reference provider로 둔다.
5. CI에서 provider manifest validation, doctor, explain, Stella/Ninja parity를 확인한다.
6. 나중에 provider registry, lockfile, integrity hash, offline cache를 설계한다.

원격 fetch는 마지막 단계다. 초기부터 fetch를 넣으면 build reproducibility, trust, offline CI,
cache invalidation, version pinning 문제가 GLP 자체보다 커진다.

## 성공 조건

- 기존 C/C++/ASM 프로젝트는 `tools.<provider>.compiler` toolset 문법으로 동작한다.
- 새 언어 provider가 QStar core patch 없이 추가된다.
- `lang.<namespace>`는 provider activation 이후에만 허용된다.
- provider option typo는 diagnostic으로 잡힌다.
- `qstar explain`과 `qstar doctor`가 provider tool role과 lowering 결과를 보여준다.
- Stella와 Ninja가 같은 provider action contract를 실행한다.
- generated object, archive, link input cache key가 deterministic하다.
- provider가 QStar core에 package manager나 언어 semantic analyzer 책임을 밀어 넣지 않는다.

이 방향이면 QStar는 현재의 generic build system 표면을 유지하면서, 장기적으로는
language-neutral build kernel로 이동할 수 있다.
