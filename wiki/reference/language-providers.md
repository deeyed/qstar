# Language Providers

QStar는 특정 언어에 종속되지 않는 빌드시스템이다. 현재 runtime은 built-in `c`, `cxx`,
`asm` provider namespace를 preloaded registry로 다룬다. Public syntax의 `lang.c`,
`lang.cxx`, `lang.asm`은 초보자 친화 표면으로 계속 유지되지만, 내부 source
classification과 tool role은 `c.compiler`, `cxx.compiler`, `asm.compiler` 같은 provider
role로 내려간다.

그 밖의 언어는 아직 source suffix나 언어별 namespace를 QStar DSL에 추가하지 않는다. 외부
compiler가 object artifact를 만들게 한 뒤 그 object를 consuming target에 연결한다. 이
현재 경계를 object artifact bridge라고 부른다.

Generic Language Provider(GLP)는 이 문서의 다음 정식 provider 경로다. GLP가 구현되면
`qstar.use_language("zig")`로 provider를 활성화하고, provider가 등록한 `lang.zig` 같은
동적 namespace와 `zig.object(...)` helper를 사용할 수 있다. GLP 설계와 최종 문법은
root의 `glp_roadmap.md`에 둔다.

## 현재 경로: object artifact bridge

GLP가 외부 provider까지 확장되기 전까지 built-in `c`/`cxx`/`asm` 외 언어 source를 직접
`sources`에 넣지 않는다.
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

## 다음 경로: GLP

GLP는 language provider를 project-local 또는 standard provider package로 활성화하는
방식이다. Provider package는 `<id>.qsm` manifest와 `provider.lua` implementation을 가진다.

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

이 문법은 external provider roadmap surface다. 현재 stable runtime에서는 built-in
`c`/`cxx`/`asm` provider 외 언어를 object artifact bridge로 표현한다.

## 관련 CLI

```sh
qstar --file qstar.lua dry-run //:app
qstar --file qstar.lua build //:app
qstar --file qstar.lua -G ninja build //:app
```

## 관련 diagnostic

- `this language is not a QStar compile provider`
- `qstar.output(..., {format = "object"})`

## 관련 문서

- `glp_roadmap.md`
- `wiki/reference/object-artifacts.md`
- `wiki/reference/custom-target.md`
