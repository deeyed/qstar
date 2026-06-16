# Language Providers

QStar는 특정 언어에 종속되지 않는 빌드시스템이다. 현재 runtime은 built-in `c`, `cxx`,
`asm` provider namespace를 preloaded registry로 다룬다. Public syntax의 `lang.c`,
`lang.cxx`, `lang.asm`은 초보자 친화 표면으로 계속 유지되지만, 내부 source
classification과 tool role은 `c.compiler`, `cxx.compiler`, `asm.compiler` 같은 provider
role로 내려간다.

외부 provider는 `qstar.use_language(...)`로 명시적으로 활성화한다. Activation은 provider
manifest를 읽고 `lang.<namespace>`를 허용하는 registry를 갱신하며, manifest의 `options`
schema로 `lang.<namespace>` table을 검증한다. Manifest의 `units` schema는 provider helper가
`qstar.source(...)` token을 만들 수 있게 하고, backend는 provider implementation의 lowering
function이 반환한 action template을 consuming target 소유 object artifact로 낮춘다.
외부 compiler 호출을 직접 손으로 제어해야 하는 경우에는 object artifact bridge를 계속 쓴다.

## Provider Activation

QStar는 표준 Zig/Rust provider를 설치물에 함께 포함한다. 따라서 일반 사용자는 provider
package를 직접 작성하지 않아도 다음처럼 바로 활성화할 수 있다.

```lua
local zig = qstar.use_language("zig")
```

Short id form은 먼저 project-local provider를 찾고, 없으면 installed standard provider bundle
`share/qstar/languages/<id>`를 찾는다. 같은 ID를 project-local layout에 vendoring하면 그
manifest가 우선한다.

Project-local provider는 다음 layout을 쓴다.

```txt
qstar/
  languages/
    zig/
      zig.qsm
      provider.lua
```

`qstar.use_language("zig")`는 project-local `qstar/languages/zig/zig.qsm`을 먼저 읽고,
없으면 installed standard provider bundle의 `zig` provider를 읽는다. 명시적 folder form은 project-relative
manifest로만 해석된다.

```lua
local zig = qstar.use_language("zig")
-- 또는:
-- local zig = qstar.use_language("qstar/languages/zig")
```

같은 provider를 두 번 활성화하면 duplicate diagnostic이 난다. Provider끼리 서로를 다시
활성화하는 circular chain도 error다.

`zig.qsm`은 일반 helper `.qsm`이 아니라 provider manifest다. 반드시
`qstar.language_provider { ... }`를 반환해야 하며, QStar는 `api`, `id`, `version`,
`namespace`, `implementation`, `tools`, `units`, `options`, `exports`, `scaffold` schema를
검증한다.
`provider.lua`는 별도의 제한 sandbox에서 로드되는 implementation이다. Provider 작성자 API와
사용자 API는 `exports` table로 분리된다.

```lua
return qstar.language_provider {
  api = "qstar.lang/1",
  id = "zig",
  version = "0.1",
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
      default = "native",
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
  scaffold = {
    api = "qstar.scaffold/1",
    tools = {
      compiler = {"zig"},
    },
    options = {
      optimize = "Debug",
      target = "native",
    },
    shapes = {
      app = {
        files = {
          {
            path = "src/main.zig",
            body = "pub fn main() void {}\n",
          },
        },
        targets = {
          {
            kind = "executable",
            name = "app",
            sources = {
              {
                helper = "object",
                path = "src/main.zig",
              },
            },
          },
        },
      },
    },
  },
}
```

## Provider Init Scaffold Metadata

`scaffold`는 선택 field다. 있으면 QStar는 `api = "qstar.scaffold/1"`, `tools`,
`options`, `shapes`를 manifest load 시점에 검증한다. Q212부터 `qstar init`은 primary
provider의 shape plan을 읽어 provider별 folder layout, sample source, root `qstar.lua`,
workspace fragment를 materialize한다. Provider가 요청한 shape를 제공하지 않으면 C fallback
scaffold와 warning을 사용한다.
Workspace scaffold에서는 root와 각 fragment가 같은 provider manifest를 다시 활성화할 수
있다. 같은 manifest의 `qstar.use_language("zig")`는 기존 export table을 반환하고, 다른
manifest가 같은 namespace를 차지하면 duplicate namespace error를 유지한다.

검증 규칙:

- `scaffold.tools.<role>`은 provider manifest의 `tools.<role>`에 선언되어 있어야 한다.
- `scaffold.options.<name>`은 provider manifest의 `options.<name>` schema와 타입이 맞아야 한다.
- `scaffold.shapes` key는 `app`, `lib`, `tool`, `empty`, `workspace` 중 하나여야 한다.
- `files`, `directories`, source helper path, fragment path는 package-relative path만 허용한다.
- Fragment path는 `.qst`로 끝나야 한다.
- Template variable은 `${project_name}`, `${project_ident}`, `${shape}`, `${namespace}`,
  `${target_name}`, `${source_ext}`만 허용한다.
- `command`, `script`, `fetch`, URL field 같은 실행/네트워크 surface는 schema에 없다.
- Absolute path, parent directory path, shell command substitution은 rejected diagnostic이다.

`provider.lua`는 graph declaration API를 볼 수 없다. 아래처럼 provider helper만 작성한다.

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

return P
```

현재 runtime은 manifest와 implementation을 모두 읽고 검증한 뒤, `exports`가 가리키는
implementation field만 `qstar.use_language(...)`의 반환 table에 노출한다. 예를 들어 위
manifest에서는 사용자 코드가 `zig.tools`, `zig.options`, `zig.object`를 볼 수 있다. `options` schema는
`string`, `bool`/`boolean`, `list`, `enum` 타입과 `default` metadata를 지원한다. 사용자가
`lang.zig`에 schema에 없는 key를 쓰거나 타입이 맞지 않는 값을 넣으면 diagnostic이 난다.

`qstar.import_module(...)`로 provider를 조용히 등록할 수 없다. 일반 helper `.qsm` 평가 중
`qstar.use_language(...)`를 호출하는 것도 금지된다. Provider manifest 안에서 다른 provider
dependency를 활성화하는 경우만 허용된다.

## 현재 source 경로

외부 언어 source는 raw string으로 `sources`에 넣지 않는다. Provider가 노출한 helper가
`qstar.source(path, {language = "...", unit = "..."})` token을 반환하고, target reader가
provider implementation의 `lower` 함수로 그 token을 object-producing action template으로
낮춘다. 예를 들어
`zig.object("src/main.zig")`는 consuming target이 소유하는 deterministic object output을
만든다.

기존 object artifact bridge도 계속 유효하다. 외부 compiler 호출을 더 세밀하게 제어해야 하면
`qstar.custom_target`으로 작성하고, 결과 object를 `qstar.output(path, {format = "object"})`로
표시한 뒤 consuming target의 `sources`에 넣는다.

Provider source unit lowering은 Stella와 Ninja가 같은 backend contract로 실행한다.
Provider implementation은 `qstar.argv()`와 `ctx.tool`, `ctx.input`, `ctx.output`,
`ctx.option`으로 action을 만든다. QStar는 그 결과의 `command`, `inputs`, `outputs`,
`depfile`을 Graph IR에 저장하고 action-log/replay, response file, depfile-discovered input,
Ninja emission에 같은 값을 사용한다.

```lua
function P.compile_object(ctx)
  local argv = qstar.argv()
  argv:add(ctx.tool("compiler"))
  argv:add("-c")
  argv:add(ctx.input("source"))
  argv:add("-o")
  argv:add(ctx.output("object"))

  if ctx.option("optimize") then
    argv:add("--optimize=" .. ctx.option("optimize"))
  end

  argv:add_all(ctx.option("compile_options"))

  return {
    command = argv,
    inputs = {ctx.input("source")},
    outputs = {ctx.output("object")},
    depfile = ctx.output("depfile"),
  }
end
```

## 최소 예제

```lua
qstar.executable "app" {
  sources = {"src/main.c"},
}
```

## 전체 예제

```lua
qstar.custom_target "foreign_obj" {
  inputs = {"src/foreign.source"},
  outputs = {qstar.output("generated/foreign.o", {format = "object"})},
  command = qstar.cli {"tools/compile-foreign.sh", qstar.input(0), qstar.output(0)},
}

qstar.executable "app" {
  sources = {
    "src/main.c",
    qstar.output("generated/foreign.o"),
  },
}
```

## 실패 예제

```lua
qstar.executable "bad" {
  sources = {"src/main.c", "src/foreign.source"},
}
```

Built-in provider가 없는 외부 언어 source를 `sources`에 직접 넣지 않는다. 외부 compiler를 호출하는
`qstar.custom_target`을 만들고 `qstar.output(path, {format = "object"})` output을 consuming
target의 `sources`에 넣는다.

## 계약

- QStar는 외부 언어의 AST, module system, header semantics를 해석하지 않는다.
- provider source unit은 provider lowering action으로 표현한다.
- object artifact bridge는 `qstar.custom_target`과 `qstar.output(path, {format = "object"})`를 쓴다.
- Stella와 Ninja backend는 provider action과 generated object artifact를 같은 action/log/replay contract로 처리한다.
- 언어별 package manager, semantic import/export, compiler internal API 호출은 QStar 책임이 아니다.

## GLP Lowering

활성화된 provider는 source suffix, source helper, backend lowering을 실제 build action으로
연결한다. Provider package는 `<id>.qsm` manifest와 `provider.lua` implementation을 가진다.

```txt
qstar/
  languages/
    zig/
      zig.qsm
      provider.lua
```

사용자 코드는 provider manifest나 lowering 구현을 직접 다루지 않는다.

```lua
local zig = qstar.use_language("zig")

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
      target = "native",
      optimize = "Debug",
      compile_options = {"-Ddemo"},
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

이 문법 중 provider activation, manifest validation, `provider.lua` sandbox loading,
`lang.zig` namespace gate, provider-defined option schema validation, `zig.object` source
token, object output allocation, provider action lowering, Stella/Ninja backend execution은
구현되어 있다.

## 관련 CLI

```sh
qstar --file qstar.lua dry-run //:app
qstar --file qstar.lua build //:app
qstar --file qstar.lua -G ninja build //:app
```

## 관련 diagnostic

- `this language is not a QStar compile provider`
- `qstar.output(..., {format = "object"})`
- `qstar: unknown language namespace lang.zig`
- `qstar: unknown field lang.zig.<option>`
- `qstar: lang.zig.<option> has unsupported enum value '...'`
- `qstar: duplicate language provider namespace lang.zig`
- `qstar: circular language provider activation`

## 관련 문서

- `glp_roadmap.md`
- `wiki/reference/object-artifacts.md`
- `wiki/reference/custom-target.md`
