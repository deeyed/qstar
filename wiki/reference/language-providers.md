# Language Providers

QStar는 특정 언어에 종속되지 않는 빌드시스템이다. 현재 runtime은 built-in `c`, `cxx`,
`asm` provider namespace를 preloaded registry로 다룬다. Public syntax의 `lang.c`,
`lang.cxx`, `lang.asm`은 초보자 친화 표면으로 계속 유지되지만, 내부 source
classification과 tool role은 `c.compiler`, `cxx.compiler`, `asm.compiler` 같은 provider
role로 내려간다.

외부 provider는 `qstar.use_language(...)`로 명시적으로 활성화한다. Activation은 provider
manifest를 읽고 `lang.<namespace>`를 허용하는 registry를 갱신한다. 다만 외부 source suffix
classification과 backend lowering은 아직 후속 GLP 작업이다. Provider backend가 없는 언어는
외부 compiler가 object artifact를 만들게 한 뒤 그 object를 consuming target에 연결한다. 이
현재 경계를 object artifact bridge라고 부른다.

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

`zig.qsm`은 일반 `.qsm`처럼 table을 반환하지만, `qstar.import_module(...)`로 조용히 graph
semantics를 바꾸지 않는다. 반드시 `qstar.use_language(...)`를 통해 활성화해야 한다.
일반 helper `.qsm` 평가 중 `qstar.use_language(...)`를 호출하는 것도 금지된다. Provider
manifest 안에서 다른 provider dependency를 활성화하는 경우만 허용된다.

```lua
local M = {
  name = "zig",
  version = "0.1",
  namespace = "zig",
}

function M.tools(t)
  return t
end

function M.options(t)
  return t or {}
end

return M
```

현재 manifest에서 runtime이 읽는 built-in metadata는 `name` 또는 `id`, `version`,
`namespace` 또는 단일 항목 `namespaces = {...}`다. `tools`, `options`, `object` 같은 helper는
provider table이 사용자 code에 돌려주는 Lua helper이며, QStar core가 fixed keyword로
해석하지 않는다.

## 현재 source 경로: object artifact bridge

Provider backend가 외부 source lowering까지 확장되기 전까지 built-in `c`/`cxx`/`asm` 외
언어 source를 직접 `sources`에 넣지 않는다.
외부 compiler 호출은 `qstar.custom_target`으로 작성하고, 결과 object를
`qstar.output(path, {format = "object"})`로 표시한다.

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

다음 GLP 작업은 활성화된 provider가 source suffix, source helper, option schema, backend
lowering을 등록하는 경로다. Provider package는 `<id>.qsm` manifest와 `provider.lua`
implementation을 가진다.

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
      optimize = "Debug",
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

이 문법 중 provider activation과 `lang.zig` namespace gate는 구현되어 있다. `zig.object`
같은 source helper와 외부 source lowering은 후속 GLP backend 작업이므로, 아직 stable
runtime에서는 object artifact bridge를 사용한다.

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
- `qstar: duplicate language provider 'qstar/languages/zig/zig.qsm'`
- `qstar: circular language provider activation`

## 관련 문서

- `glp_roadmap.md`
- `wiki/reference/object-artifacts.md`
- `wiki/reference/custom-target.md`
