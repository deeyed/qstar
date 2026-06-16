# Language Providers

QStar는 특정 언어에 종속되지 않는 빌드시스템이다. 현재 runtime은 built-in `c`, `cxx`,
`asm` provider namespace를 preloaded registry로 다룬다. Public syntax의 `lang.c`,
`lang.cxx`, `lang.asm`은 초보자 친화 표면으로 계속 유지되지만, 내부 source
classification과 tool role은 `c.compiler`, `cxx.compiler`, `asm.compiler` 같은 provider
role로 내려간다.

외부 provider는 `qstar.use_language(...)`로 명시적으로 활성화한다. Activation은 provider
manifest를 읽고 `lang.<namespace>`를 허용하는 registry를 갱신하며, manifest의 `options`
schema로 `lang.<namespace>` table을 검증한다. Manifest의 `units` schema는 provider helper가
`qstar.source(...)` token을 만들 수 있게 하고, 현재 backend는 이 token을 consuming target이
소유하는 object artifact로 낮춘다. Provider별 복잡한 argv lowering은 아직 후속 GLP 작업이므로,
그 경우에는 외부 compiler가 object artifact를 만들게 한 뒤 그 object를 consuming target에
연결하는 object artifact bridge를 계속 쓴다.

## Provider Activation

Project-local provider는 다음 layout을 쓴다.

```txt
qstar/
  languages/
    zig/
      zig.qsm
      provider.lua
```

`qstar.use_language("zig")`는 `qstar/languages/zig/zig.qsm`을 읽는다. 명시적 folder form도
같은 manifest로 해석된다.

```lua
local zig = qstar.use_language("zig")
-- 또는:
-- local zig = qstar.use_language("qstar/languages/zig")
```

같은 provider를 두 번 활성화하면 duplicate diagnostic이 난다. Provider끼리 서로를 다시
활성화하는 circular chain도 error다.

`zig.qsm`은 일반 helper `.qsm`이 아니라 provider manifest다. 반드시
`qstar.language_provider { ... }`를 반환해야 하며, QStar는 `api`, `id`, `version`,
`namespace`, `implementation`, `tools`, `units`, `options`, `exports` schema를 검증한다.
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
      values = {"Debug", "ReleaseFast"},
      default = "Debug",
    },
    emit_docs = {
      type = "bool",
      default = false,
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
그 token을 object-producing source unit으로 Graph IR에 낮춘다. 예를 들어
`zig.object("src/main.zig")`는 consuming target이 소유하는 deterministic object output을
만든다.

기존 object artifact bridge도 계속 유효하다. 외부 compiler 호출을 더 세밀하게 제어해야 하면
`qstar.custom_target`으로 작성하고, 결과 object를 `qstar.output(path, {format = "object"})`로
표시한 뒤 consuming target의 `sources`에 넣는다.

현재 provider source unit lowering은 provider compiler role을 사용해
`compiler -c <source> -o <object>` 형태의 generic object contract로 실행된다. provider별
정교한 argv lowering 함수는 후속 backend API에서 확장한다.

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
- 외부 compiler 호출은 `qstar.custom_target`과 `qstar.cli` argv-vector로 표현한다.
- 생성된 object는 `qstar.output(path, {format = "object"})`로 표시한다.
- Stella와 Ninja backend는 generated object artifact를 link/archive input으로 소비한다.
- 언어별 package manager, semantic import/export, compiler internal API 호출은 QStar 책임이 아니다.

## 다음 경로: GLP Lowering

다음 GLP 작업은 활성화된 provider가 source suffix, source helper, backend lowering을 실제
build action으로 연결하는 경로다. Provider package는 이미 `<id>.qsm` manifest와
`provider.lua` implementation을 가진다.

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
      emit_docs = false,
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
token, object output allocation은 구현되어 있다. Provider별 정교한 argv lowering은 아직
후속 GLP backend 작업이다.

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
- `qstar: duplicate language provider 'qstar/languages/zig/zig.qsm'`
- `qstar: circular language provider activation`

## 관련 문서

- `glp_roadmap.md`
- `wiki/reference/object-artifacts.md`
- `wiki/reference/custom-target.md`
