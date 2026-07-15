# QStar Init And GLP Scaffold Design

이 문서는 `qstar init`을 generic project scaffolder로 개편하고, Generic
Language Provider(GLP)가 언어별 project layout과 sample source를 선언하는 방향을
정리한다. 현재 public init surface는 `qstar init app|lib|tool|empty|workspace`
project shape이며, `--use-language` language selection, external provider vendoring,
provider manifest의 optional `scaffold` schema validation, provider별 `qstar.lua`,
source file, workspace fragment materialization까지 지원한다.

## 배경

QStar는 C/C++/ASM을 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이어야 한다.
이 목표는 C/C++/ASM 지원을 제거하자는 뜻이 아니다. C/C++/ASM은 builtin으로 유지할
수 있다. 중요한 경계는 C/C++/ASM만 top-level 문법을 독점하지 않고, Zig/Rust/CUDA
같은 외부 언어도 GLP를 통해 같은 authoring 구조를 얻는 것이다.

현재 GLP는 다음을 이미 제공한다.

- built-in `c`, `cxx`, `asm` provider namespace를 preloaded registry로 다룬다.
- external provider는 `qstar.use_language("zig")` 같은 entrypoint로 활성화한다.
- provider manifest는 `qstar.language_provider { api = "qstar.lang/1", ... }` 또는
  `api = "qstar.lang/2"`를
  반환한다.
- provider는 `tools`, `units`, `options`, `exports`를 선언한다.
- provider implementation인 `provider.lua`는 제한 sandbox에서 로드된다.
- 사용자에게는 `zig.tools`, `zig.options`, `zig.object` 같은 exported helper가
  노출된다.
- `lang.<namespace>` table은 provider option schema로 검증된다.
- provider source unit suffix는 graph-level source registry에 등록되며, raw string
  source와 explicit `qstar.source(...)` token 모두 object-producing action으로 낮아진다.
- Stella와 Ninja는 같은 provider action contract를 실행한다.

이전 `qstar init`은 GLP 철학을 충분히 반영하지 못했다. `c-app`, `c-lib` 같은
template 이름은 언어가 top-level init surface에 노출되는 구조였고, 언어별 source
layout도 QStar core 내부 C 문자열 template로 고정되어 있었다. 새 설계는 이 구조를
"project shape + language provider" 조합으로 바꾼다.

## 목표

- `qstar init`의 첫 번째 축을 언어가 아니라 project shape로 만든다.
- C/C++/ASM은 builtin, always-active provider로 유지한다.
- external GLP는 init 시점에 project-local provider로 vendoring할 수 있게 한다.
- 언어별 folder layout, sample source, default tool, default option은 provider가
  선언한다.
- QStar core는 generic shape와 파일 생성만 책임진다.
- generated `qstar.lua`는 모든 provider 사용을 명시적으로 보여준다.
- init은 network fetch, package manager 실행, compiler probing, cross policy 추론을
  하지 않는다.

## 비목표

다음은 이 init 개편의 책임이 아니다.

- 원격 provider registry와 version solver.
- provider package manager.
- compiler/toolchain 자동 설치.
- target triple, sysroot, resource dir, stdlib 정책 자동 추론.
- host-specific hidden build context 생성.
- provider가 arbitrary Lua나 shell script를 실행해 파일을 생성하는 구조.

위 항목들은 QStar가 다시 domain-specific build policy를 품게 만들 수 있으므로 초기
설계에서 제외한다.

## C/C++/ASM 정책

C/C++/ASM은 계속 builtin으로 둔다.

- `local c = qstar.use_language("c")`는 필요 없다.
- `lang.c`, `lang.cxx`, `lang.asm`은 초보자 친화 surface로 유지한다.
- `tools.c.compiler`, `tools.cxx.compiler`, `tools.asm.compiler`를 사용한다.
- 과거 direct compiler slot인 `tools.c = qstar.cli {...}`는 계속 금지한다.

예:

```lua
qstar.toolset "host" {
  tools = {
    archive = qstar.cli {"ar"},
    link = qstar.cli {"cc"},
    c = {
      compiler = qstar.cli {"cc"},
    },
    cxx = {
      compiler = qstar.cli {"c++"},
    },
    asm = {
      compiler = qstar.cli {"cc"},
    },
  },
}
```

이 정책의 의미는 C/C++/ASM이 "특화되어 있으나 독점하지 않는" builtin이라는 것이다.
외부 언어도 `tools.zig`, `lang.zig`, raw string `sources = {"src/main.zig"}`, explicit
`zig.object(...)` 같은 같은 급의 구조를 얻는다.

## GLP 정책

External language는 명시적으로 활성화한다.

```lua
local zig = qstar.use_language("zig")
```

활성화 후 사용자는 provider가 export한 helper를 사용한다.

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

qstar.config "debug" {
  toolset = "//:host",
  lang = {
    zig = zig.options {
      optimize = "Debug",
      target = "native",
      macos_min_version = "11.0",
    },
  },
}

qstar.executable "app" {
  configs = {"//:debug"},
  sources = {
    "src/main.zig",
  },
}
```

Provider는 다음을 담당한다.

- provider namespace: `zig`, `rust`, `cuda`.
- tool role: `zig.compiler`, `rust.compiler`, `cuda.compiler`.
- option schema: `optimize`, `edition`, `target`, `arch`, `compile_options`.
- source unit: `object`.
- source suffix registry: `.zig`, `.rs`, `.cu` 같은 raw source classification.
- lowering function: source unit을 object-producing action으로 변환.
- 사용자 helper export: `zig.tools`, `rust.options`, `cuda.object`.

## Provider 배치

QStar 설치물에는 표준 provider bundle이 들어갈 수 있다.

```txt
<prefix>/
  share/
    qstar/
      languages/
        zig/
          zig.qsm
          provider.lua
        rust/
          rust.qsm
          provider.lua
        cuda/
          cuda.qsm
          provider.lua
```

Project-local provider는 다음 layout을 쓴다.

```txt
project/
  qstar/
    languages/
      zig/
        zig.qsm
        provider.lua
```

`qstar.use_language("zig")`, `qstar.use_language("rust")`, `qstar.use_language("cuda")`는
project-local provider를 먼저 찾고, 없으면 installed standard provider를 찾는다. Init에서
`--use-language=zig`, `--use-language=rust`, `--use-language=cuda`를 주면 installed standard provider를
project-local `qstar/languages/<id>`로 복사한다. 생성된 `qstar.lua`에는 그래도
`local zig = qstar.use_language("zig")` 같은 activation이 남는다.

이 방식의 장점:

- 프로젝트가 어떤 provider에 의존하는지 source tree에 명확히 남는다.
- QStar binary 업그레이드가 기존 프로젝트 provider 동작을 몰래 바꾸지 않는다.
- 사용자가 provider 파일을 직접 작성하지 않아도 된다.
- project-local provider가 installed provider보다 우선하므로 vendoring이 자연스럽다.

## Init CLI

기존의 언어별 template 이름은 제거되었고 compatibility diagnostic으로 돌린다.

이전에 존재하던 `c-app`, `c-lib`, `generated` 형태는 제거되었다.

현재 지원 구조:

```sh
qstar init <shape> [directory]
qstar init app hello
qstar init empty sandbox
qstar init workspace demo
qstar init app hello --name hello_app
qstar init app hello --use-language=c --dry-run
qstar init app hello --use-language=zig
qstar init workspace demo --use-language=c,zig
qstar init --list-shapes
qstar init --list-languages
```

`--use-language`는 comma-separated list를 받는다.

```sh
qstar init app mixed --use-language=zig,rust,cuda
```

반복 option은 아직 지원하지 않는다. 장기적으로는 아래 형태도 허용할 수 있다.

```sh
qstar init app mixed --use-language zig --use-language rust
```

언어를 생략하면 기본값은 `c`다. C는 builtin이고 provider vendoring이 필요 없으며,
바로 build 가능한 skeleton을 만들 수 있다. `cxx`와 `asm`도 builtin language로 판별되며
generated `qstar.lua`의 `tools.cxx.compiler`, `tools.asm.compiler` entry를 만들 수 있다.
External provider가 요청한 shape의 scaffold를 제공하면 `qstar init`은 그 provider plan을
materialize한다. Provider가 해당 shape를 제공하지 않을 때만 C fallback scaffold를 만들고
warning을 출력한다.

```sh
qstar init app hello --use-language=zig
```

위 명령은 현재 다음을 수행한다.

- installed standard provider bundle 또는 dev checkout에서 `zig` provider를 찾는다.
- `qstar/languages/zig`에 provider package를 복사한다.
- `qstar.lua` 상단에 `local zig = qstar.use_language("zig")`를 생성한다.
- `qstar.toolset`에 `zig = zig.tools { compiler = qstar.cli {"zig"} }` entry를 생성한다.
- `qstar.config`에 `zig = zig.options { ... }` default option entry를 생성한다.
- provider scaffold plan에 따라 `src/main.zig`와 raw string `sources = {"src/main.zig"}`
  target을 만든다.

`--use-language=rust`와 `--use-language=cuda`도 같은 흐름을 사용한다. 표준 Rust provider는
`qstar/languages/rust`, `rust.tools`, `rust.options`, raw string `"src/main.rs"`를
생성하고, 표준 CUDA provider는 `qstar/languages/cuda`, `cuda.tools`, `cuda.options`,
raw string `"src/main.cu"`를 생성한다.

여러 언어가 들어오면 첫 번째 언어가 primary scaffold language다. Primary provider가
shape-specific scaffold를 제공하면 그 layout이 `src/main.zig`, `src/crates/...`,
`kernels/...` 같은 언어별 layout을 책임진다. 나머지 언어는 provider vendoring, activation,
toolset/config entry까지만 생성한다.

`qstar init app hello`에서 `hello`는 생성 directory다. 기본 project name은 directory
basename인 `hello`가 된다.

```lua
qstar.project {
  name = "hello",
  version = "0.1.0",
  root = ".",
}
```

Directory basename과 project name을 다르게 하고 싶으면 `--name`을 쓴다.

```sh
qstar init app ./examples/hello-world --name hello
```

## 공통 생성물

모든 shape는 최소한 다음을 만든다.

```txt
hello/
  qstar.lua
  .gitignore
```

External language를 선택하면 provider를 vendoring한다.

```txt
hello/
  qstar/
    languages/
      zig/
        zig.qsm
        provider.lua
```

C/C++/ASM은 builtin이므로 provider 파일을 복사하지 않는다.

`qstar.lua`는 다음 원칙을 따른다.

- project metadata를 명시한다.
- toolset을 명시한다.
- config를 명시한다.
- external provider는 `qstar.use_language("<id>")`로 명시 활성화한다.
- provider helper를 사용해 source unit과 options를 작성한다.
- hidden cross policy를 생성하지 않는다.

## Shape: app

`app`은 실행 파일 하나를 만드는 shape다.

C fallback 구조:

```txt
hello/
  qstar.lua
  .gitignore
  src/
    main.c
```

Provider-defined scaffold 완성 후 Zig provider scaffold 예:

```txt
hello/
  qstar.lua
  .gitignore
  qstar/
    languages/
      zig/
        zig.qsm
        provider.lua
  src/
    main.zig
```

C fallback `qstar.lua` 예:

```lua
qstar.project {
  name = "hello",
  version = "0.1.0",
  root = ".",
}

qstar.toolset "host" {
  tools = {
    archive = qstar.cli {"ar"},
    link = qstar.cli {"cc"},
    c = {
      compiler = qstar.cli {"cc"},
    },
  },
}

qstar.config "debug" {
  toolset = "//:host",
}

qstar.executable "app" {
  configs = {"//:debug"},
  sources = {"src/main.c"},
}
```

Provider-defined scaffold 완성 후 Zig `qstar.lua` 예:

```lua
local zig = qstar.use_language("zig")

qstar.project {
  name = "hello",
  version = "0.1.0",
  root = ".",
}

qstar.toolset "host" {
  tools = {
    archive = qstar.cli {"ar"},
    link = qstar.cli {"cc"},
    zig = zig.tools {
      compiler = qstar.cli {"zig"},
    },
  },
}

qstar.config "debug" {
  toolset = "//:host",
  lang = {
    zig = zig.options {
      optimize = "Debug",
    },
  },
}

qstar.executable "app" {
  configs = {"//:debug"},
  sources = {
    "src/main.zig",
  },
}
```

## Shape: lib

`lib`은 재사용 가능한 library와 간단한 test를 만든다.

C fallback 구조:

```txt
hello/
  qstar.lua
  .gitignore
  include/
    hello.h
  src/
    hello.c
  tests/
    unit.c
```

C fallback target 예:

```lua
qstar.staticlib "core" {
  configs = {"//:debug"},
  sources = {"src/hello.c"},
  lang = {
    c = {
      public_headers = {"include/hello.h"},
      public_include_dirs = {"include"},
    },
  },
}

qstar.test "unit" {
  configs = {"//:debug"},
  sources = {"tests/unit.c"},
  deps = {"//:core"},
}
```

Rust/Zig/CUDA 같은 external provider는 자기 언어 관습에 맞는 layout을 선언한다.
예를 들어 Rust provider는 다음처럼 정의할 수 있다.

```txt
hello/
  src/
    crates/
      hello/
        lib.rs
  tests/
    unit.rs
```

QStar core는 Rust가 `src/crates`를 쓰는지, Zig가 `src`를 쓰는지 알지 않는다.
Provider scaffold가 그 layout을 선언한다.

## Shape: tool

`tool`은 repository-local 개발 도구를 만드는 shape다. Code generator, asset compiler,
repo helper command 같은 것을 만드는 데 쓴다.

C fallback 구조:

```txt
hello/
  qstar.lua
  .gitignore
  tools/
    hello/
      main.c
```

C fallback target 예:

```lua
qstar.executable "tool" {
  configs = {"//:debug"},
  sources = {"tools/hello/main.c"},
}

qstar.run_target "run" {
  command = qstar.cli {qstar.target_file("//:tool")},
  deps = {"//:tool"},
}
```

External provider는 같은 shape를 자기 관습에 맞게 바꿀 수 있다.

```txt
hello/
  src/
    tools/
      hello/
        main.rs
```

혹은 provider가 `tools/hello/main.zig`처럼 C fallback과 비슷한 layout을 선택할 수도 있다.

## Shape: empty

`empty`는 최소 QStar project다.

```txt
hello/
  qstar.lua
  .gitignore
```

기본 `qstar.lua`:

```lua
qstar.project {
  name = "hello",
  version = "0.1.0",
  root = ".",
}

qstar.toolset "host" {
  tools = {
    archive = qstar.cli {"ar"},
    link = qstar.cli {"cc"},
    c = {
      compiler = qstar.cli {"cc"},
    },
  },
}

qstar.config "debug" {
  toolset = "//:host",
}
```

`--use-language=zig`를 같이 주면 provider vendoring, activation, provider tool/default
option entry를 생성하고, target/source는 만들지 않는다.

```lua
local zig = qstar.use_language("zig")

qstar.project {
  name = "hello",
  version = "0.1.0",
  root = ".",
}

qstar.toolset "host" {
  tools = {
    archive = qstar.cli {"ar"},
    link = qstar.cli {"cc"},
    zig = zig.tools {
      compiler = qstar.cli {"zig"},
    },
  },
}

qstar.config "debug" {
  toolset = "//:host",
  lang = {
    zig = zig.options {
      optimize = "Debug",
      target = "native",
      macos_min_version = "",
    },
  },
}
```

이 shape는 사용자가 직접 구조를 설계하려는 경우를 위한 것이다.

## Shape: workspace

`workspace`는 여러 package/fragment를 갖는 root project를 만든다. QStar의 fragment 규칙은
`qstar.subdir("packages/core")`가 `packages/core/core.qst`를 읽는 구조다. 따라서 workspace
scaffold도 이 규칙을 따른다.

C fallback 구조:

```txt
hello/
  qstar.lua
  .gitignore
  packages/
    core/
      core.qst
      include/
        core.h
      src/
        core.c
    app/
      app.qst
      src/
        main.c
```

Root `qstar.lua` 예:

```lua
qstar.project {
  name = "hello",
  version = "0.1.0",
  root = ".",
}

qstar.toolset "host" {
  tools = {
    archive = qstar.cli {"ar"},
    link = qstar.cli {"cc"},
    c = {
      compiler = qstar.cli {"cc"},
    },
  },
}

qstar.config "debug" {
  toolset = "//:host",
}

qstar.subdir("packages/core")
qstar.subdir("packages/app")

qstar.group "all" {
  deps = {
    "//packages/core:core",
    "//packages/app:app",
  },
}
```

`packages/core/core.qst` 예:

```lua
qstar.staticlib "core" {
  configs = {"//:debug"},
  sources = {"packages/core/src/core.c"},
  lang = {
    c = {
      public_headers = {"packages/core/include/core.h"},
      public_include_dirs = {"packages/core/include"},
    },
  },
}
```

`packages/app/app.qst` 예:

```lua
qstar.executable "app" {
  configs = {"//:debug"},
  sources = {"packages/app/src/main.c"},
  deps = {"//packages/core:core"},
}
```

External provider workspace scaffold는 root에 provider activation과 vendored provider를 추가하고,
fragments 안에서도 같은 provider helper를 다시 얻어 사용할 수 있어야 한다. Standard Zig
provider는 workspace shape에서 `packages/core/core.qst`와 `packages/app/app.qst`를 만들고,
root `qstar.lua`는 두 fragment를 `qstar.subdir("packages/core")`,
`qstar.subdir("packages/app")`로 연결한다.

## Provider Scaffold Contract

언어별 scaffold는 provider가 선언한다. QStar core가 Rust는 `src/crates`, CUDA는 `kernels`,
Zig는 `src` 같은 지식을 하드코딩하지 않는다.

Provider manifest에 optional `scaffold` root를 추가한다.

```lua
return qstar.language_provider {
  api = "qstar.lang/1",
  id = "rust",
  version = "0.1",
  namespace = "rust",
  implementation = "provider.lua",

  tools = {
    compiler = {
      role = "rust.compiler",
      required = true,
    },
  },

  units = {
    object = {
      suffixes = {".rs"},
      emits = "object",
      lower = "compile_object",
    },
  },

  options = {
    edition = {
      type = "enum",
      values = {"2021", "2024"},
      default = "2021",
    },
    crate_type = {
      type = "enum",
      values = {"lib", "rlib", "staticlib", "cdylib", "dylib", "bin", "proc-macro"},
      default = "lib",
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

  scaffold = {
    api = "qstar.scaffold/1",
    tools = {
      compiler = {"rustc"},
    },
    options = {
      edition = "2021",
      crate_type = "lib",
      compile_options = {},
    },
    shapes = {
      app = {
        files = {
          {
            path = "src/crates/${project_ident}/main.rs",
            body = "#[no_mangle]\npub extern \"C\" fn main() -> i32 { 0 }\n",
          },
        },
        target = {
          kind = "executable",
          name = "app",
          sources = {
            "src/crates/${project_ident}/main.rs",
          },
        },
      },
    },
  },
}
```

이 `scaffold` table은 선언적 plan이다. Provider는 init 시점에 shell command를 실행하지 않고,
프로젝트 파일을 직접 쓰지도 않는다. QStar core가 scaffold plan을 검증한 뒤 파일을 생성한다.
QStar는 manifest load 시점에 이 schema를 검증하고, `qstar init`이 primary provider의
shape plan을 실제 project files로 materialize한다.

## Scaffold Schema

`scaffold` root:

| Field | Type | 의미 |
| --- | --- | --- |
| `api` | string | 현재 `"qstar.scaffold/1"` |
| `tools` | table | init이 `qstar.toolset`에 넣을 default provider tool command |
| `options` | table | init이 `lang.<namespace>`에 넣을 default provider options |
| `shapes` | table | `app`, `lib`, `tool`, `empty`, `workspace`별 scaffold |

`scaffold.tools` 예:

```lua
tools = {
  compiler = {"zig"},
}
```

이는 generated `qstar.lua`에서 다음처럼 materialize된다.

```lua
zig = zig.tools {
  compiler = qstar.cli {"zig"},
}
```

`scaffold.options` 예:

```lua
options = {
  optimize = "Debug",
  target = "native",
  macos_min_version = "11.0",
}
```

이는 generated `qstar.lua`에서 다음처럼 materialize된다.

```lua
zig = zig.options {
  optimize = "Debug",
  target = "native",
  macos_min_version = "11.0",
}
```

`scaffold.shapes.<shape>`:

| Field | Type | 의미 |
| --- | --- | --- |
| `directories` | list(string) | 명시적으로 만들 directory |
| `files` | list(table) | 생성할 file 목록 |
| `targets` | list(table) | root 또는 fragment에 생성할 target skeleton |
| `fragments` | list(table) | workspace용 subdir fragment |
| `group` | table | workspace aggregate group skeleton |

`files` item:

| Field | Type | 의미 |
| --- | --- | --- |
| `path` | string | project-relative path template |
| `body` | string | file content template |
| `executable` | bool | executable bit 설정 여부 |

`targets` item:

| Field | Type | 의미 |
| --- | --- | --- |
| `kind` | string | `executable`, `staticlib`, `sharedlib`, `test`, `run_target`, `group` |
| `name` | string | target name |
| `sources` | list | raw source string 또는 provider helper reference |
| `deps` | list(string) | target deps |
| `lang` | table | target-local lang options |

Raw source string은 generated `qstar.lua`에 그대로 출력되며, provider가 활성화되어 있고 suffix가
해당 provider unit에 등록되어 있으면 build graph에서 provider object unit으로 낮아진다.

```lua
sources = {
  "src/main.zig",
}
```

Provider helper reference는 source-local option이 필요하거나 suffix 충돌을 명시적으로 해결해야 할 때
쓴다.

```lua
{
  helper = "object",
  path = "src/main.zig",
  options = {
    optimize = "ReleaseFast",
  },
}
```

이 값은 generated `qstar.lua`에서 다음으로 변환된다.

```lua
zig.object("src/main.zig", {
  optimize = "ReleaseFast",
})
```

## Template Variables

Scaffold path와 file body는 제한된 변수만 사용할 수 있다.

| Variable | 의미 |
| --- | --- |
| `${project_name}` | `qstar.project.name`에 들어갈 이름 |
| `${project_ident}` | identifier-safe project name. 예: `hello-world` -> `hello_world` |
| `${shape}` | `app`, `lib`, `tool`, `empty`, `workspace` |
| `${namespace}` | provider namespace. 예: `zig` |
| `${target_name}` | 기본 target name. 예: `app`, `core`, `tool` |
| `${source_ext}` | provider가 선택한 기본 source extension |

Provider가 임의 환경변수, host path, command substitution을 template에서 사용할 수 있게 해서는
안 된다.

## Scaffold Validation

QStar core는 scaffold plan을 적용하기 전에 다음을 검증한다.

- 모든 path는 package-relative여야 한다.
- absolute path, `..`, empty component, trailing slash file path는 금지한다.
- root 밖으로 나가는 path는 금지한다.
- provider id, namespace, helper name은 valid identifier/token이어야 한다.
- `scaffold.api`는 지원하는 version이어야 한다.
- `scaffold.tools`의 key는 provider manifest `tools`에 존재해야 한다.
- `scaffold.options`의 key와 value는 provider manifest `options` schema에 맞아야 한다.
- source helper reference는 provider `exports`에 존재해야 한다.
- generated file이 이미 있으면 overwrite하지 않는다.
- `--dry-run`은 실제 write 없이 plan만 출력한다.

Provider scaffold는 편의를 위한 선언이지 권한 상승 API가 아니다.

## Fallback Policy

Provider가 요청한 shape를 제공하지 않으면 C fallback scaffold를 사용한다. 단, 경고를 출력한다.

예:

```txt
warning language 'rust' has no init scaffold for shape 'tool'; using builtin c scaffold
```

Fallback 의미:

- init 전체가 실패하지 않는다.
- 프로젝트는 바로 build 가능한 C scaffold를 얻는다.
- 선택한 external provider는 vendoring될 수 있다.
- 사용자는 provider scaffold가 불완전하다는 사실을 알 수 있다.

Standard provider는 가능하면 `app`, `lib`, `tool`, `empty`, `workspace` 다섯 shape를 모두
제공해야 한다. CI에서는 standard provider의 scaffold coverage를 검사하는 것이 좋다.

## Workspace And Idempotent use_language

Workspace에서는 root `qstar.lua`와 여러 fragment가 같은 provider를 참조할 수 있다.
따라서 `qstar.use_language("zig")`는 같은 manifest에 대해 idempotent해야 한다.

현재 동작:

- 같은 manifest, 같은 namespace면 기존 export table을 다시 반환한다.
- 다른 manifest가 같은 namespace를 차지하려 하면 error를 낸다.
- circular provider activation은 계속 error다.

이렇게 하면 root와 fragment 모두 안전하게 다음을 쓸 수 있다.

```lua
local zig = qstar.use_language("zig")
```

## Init Algorithm

`qstar init app hello --use-language=zig`의 conceptual algorithm:

1. CLI option을 parse한다.
2. shape를 검증한다.
3. directory와 project name을 결정한다.
4. language list를 결정한다. 생략 시 `c`.
5. 각 language가 builtin인지 external provider인지 판별한다.
6. external provider는 installed standard bundle 또는 source checkout fallback에서 찾는다.
7. external provider manifest file 존재를 확인한다.
8. external provider를 project-local `qstar/languages/<id>`로 복사할 plan을 만든다.
9. shape별 scaffold plan을 선택한다. Primary provider가 shape plan을 제공하면 그것을 쓴다.
10. provider scaffold가 없으면 C fallback plan과 warning을 만든다.
11. directory, `qstar.lua`, `.gitignore`, source file, fragment file 생성 plan을 만든다.
12. `--dry-run`이면 plan만 출력한다.
13. 실제 write 전에 overwrite conflict를 검사한다.
14. directory와 file을 생성한다.
15. 생성 결과와 warning을 출력한다.

## Init Output

예상 출력:

```txt
qstar init v2
shape app
language zig
project hello
directory hello
vendor qstar/languages/zig
activate zig
scaffold zig app
create_dir src
create qstar.lua
create .gitignore
create src/main.zig
status ok
```

Fallback이 있으면:

```txt
qstar init v2
shape tool
language rust
project helper
directory helper
warning language 'rust' has no init scaffold for shape 'tool'; using builtin c scaffold
vendor qstar/languages/rust
activate rust
create_dir tools
create_dir tools/helper
create qstar.lua
create .gitignore
create tools/helper/main.c
status ok
```

## Generated .gitignore

기본 `.gitignore`는 QStar build state와 흔한 local editor state를 피한다.

```gitignore
build/
.qstar/
compile_commands.json
```

Project-local provider는 vendored source이므로 ignore하지 않는다.

## Future Provider Commands

Init 개편의 필수 구현은 아니지만, provider vendoring을 도입하면 다음 command가 자연스럽다.

```sh
qstar provider list
qstar provider update zig
qstar provider diff zig
```

초기 설계에서는 remote fetch를 넣지 않는다. `provider update`가 생기더라도 먼저 installed
standard provider와 vendored provider의 diff/update에 집중한다.

## 금지 설계

다음 설계는 피한다.

- `c-app`, `zig-app`, `rust-lib` 같은 언어별 top-level template을 계속 늘리는 방식.
- `.zig` 파일이 있다고 provider를 자동 활성화하는 방식.
- `qstar.lua`에서 provider activation을 숨기는 방식.
- provider가 init 시점에 shell command를 실행하는 방식.
- provider가 package manager를 실행하는 방식.
- provider가 compiler probing으로 얻은 absolute path를 `qstar.lua`에 쓰는 방식.
- target triple, sysroot, resource dir, stdlib policy를 QStar가 추론하는 방식.
- provider scaffold가 project root 밖에 파일을 쓰는 방식.
- 원격 provider fetch를 init 기본 동작에 넣는 방식.

## 권장 설계

- Project shape는 QStar core가 안다.
- 언어별 layout과 sample source는 provider가 안다.
- Provider scaffold는 declarative data다.
- File write는 QStar core만 수행한다.
- External provider는 init 시점에 project-local로 vendoring한다.
- Generated `qstar.lua`는 provider activation과 toolset/config/target을 명시적으로 보여준다.
- C/C++/ASM은 builtin fallback으로 유지한다.
- Standard provider는 다섯 shape scaffold를 제공하도록 관리한다.

## 구현 영향 범위

예상 변경 범위:

- `src/main.c`: `qstar init` CLI parsing 확장.
- `src/init.c`: fixed template table에서 shape/provider scaffold engine으로 전환.
- `src/internal.h`: init option 구조와 provider scaffold 읽기 API 추가.
- `src/lua_runtime.c`: provider manifest allowlist에 `scaffold` 추가, scaffold schema validation.
- `qstar/languages/*/*.qsm`: standard provider scaffold metadata 추가.
- `docs/README.md`: 이 문서 링크.
- `docs/syntax.md`: init/GLP scaffold syntax drift guard 추가 가능.
- `wiki/reference/language-providers.md`: user-facing provider scaffold 설명 추가.
- `wiki/reference/qstar-lua.md`: `qstar init`과 provider vendoring 설명 추가.
- `man/man1/qstar.1`: init CLI 갱신.
- `tests/smoke.sh`: init shape, provider vendoring, fallback warning 테스트.
- `tests/linux-validation.sh`: installed provider scaffold/vendoring 검증.

## 최종 철학

QStar core는 다음을 안다.

- `app`, `lib`, `tool`, `empty`, `workspace`.
- `qstar.lua`.
- `.gitignore`.
- `qstar.subdir`.
- `qstar.toolset`, `qstar.config`, target skeleton.
- provider bundle 위치와 vendoring.
- scaffold plan validation과 file write.

언어 provider는 다음을 안다.

- source directory convention.
- sample source body.
- default compiler command.
- default language options.
- source helper selection.
- shape별 target/source layout.

이 경계가 유지되면 `qstar init`은 C 중심 template generator가 아니라 generic QStar project
scaffolder가 된다. C/C++/ASM은 계속 편하게 쓸 수 있고, external provider도 init 단계부터
builtin과 거의 같은 사용자 경험을 얻는다.
