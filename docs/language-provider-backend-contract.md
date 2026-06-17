# QStar Language Provider Backend Contract

QStar runtime은 built-in `c`, `cxx`, `asm` provider namespace를 preloaded registry로
갖고 있다. Public syntax의 `lang.c`, `lang.cxx`, `lang.asm`은 그대로 유지되지만, 내부
source classification과 tool role은 `c.compiler`, `cxx.compiler`, `asm.compiler` 같은
provider role로 내려간다. 외부 provider는 `qstar.use_language(...)`로 활성화하고,
provider source unit suffix는 graph-level source registry에 등록된다. Raw string source와
explicit provider helper token은 provider implementation의 lowering function이 반환한 action
template으로 Graph IR에 저장된다.

## 결정

- 활성화된 외부 provider의 source suffix는 raw string `sources` classification에 참여한다.
- provider helper는 source-local option이나 suffix 충돌 해소를 위한 explicit
  `qstar.source(path, {language = "...", unit = "..."})` token을 만든다.
- raw string이 여러 provider source unit과 match되거나 built-in source suffix와 provider unit
  모두에 match되면 QStar는 explicit provider helper를 요구한다.
- source unit은 `ctx.tool`, `ctx.input`, `ctx.output`, `ctx.cache`, `ctx.option`으로 lowering된다.
- lowered action의 argv, env, inputs, outputs, depfile은 Stella와 Ninja가 같은 contract로 실행한다.
- provider action `env`는 `"NAME=value"` string list다. 실행 backend는 값을 실제 process에 넘기지만 action-log/replay에는 `NAME=<redacted>`로만 기록한다.
- object artifact bridge도 계속 지원된다.
- QStar는 외부 compiler의 AST, module system, semantic import/export, package manager,
  internal API를 해석하거나 호출하지 않는다.

## 현재 backend별 동작

| Source kind | Stella | Ninja |
| --- | --- | --- |
| C | compile action | lowered |
| C++ | compile action | lowered |
| ASM | compile action | lowered |
| Provider source unit | lowered provider action | lowered provider action |
| Generated object | archive/link input | archive/link input |
| Other source suffix | unsupported source diagnostic | unsupported source diagnostic |

## Provider lowering contract

Provider implementation의 `lower` 함수는 action table을 반환한다.

```lua
function P.compile_object(ctx)
  local argv = qstar.argv()
  argv:add(ctx.tool("compiler"))
  argv:add("-c")
  argv:add(ctx.input("source"))
  argv:add("-o")
  argv:add(ctx.output("object"))

  return {
    command = argv,
    env = {
      "ZIG_GLOBAL_CACHE_DIR=" .. ctx.cache("zig-global"),
      "ZIG_LOCAL_CACHE_DIR=" .. ctx.cache("zig-local"),
    },
    inputs = {ctx.input("source")},
    outputs = {ctx.output("object")},
    depfile = ctx.output("depfile"),
  }
end
```

`ctx.tool("compiler")`는 provider namespace를 붙여 `zig.compiler` 같은 toolset role로
해석된다. `ctx.tool("zig.compiler")`처럼 완전한 role 이름을 직접 쓸 수도 있다.
`ctx.cache(name)`은 현재 provider source action의 object directory 아래 package-relative
cache path를 반환한다. `name`은 package-relative path여야 하며 absolute path나 `..`는
허용하지 않는다.
`ctx.option(name)`은 source-local option, target/config `lang.<namespace>` option, schema
default 순서로 값을 반환한다. `depfile`이 있거나 unit schema의 `deps = "make"`가 설정되면
backend는 depfile-discovered input을 action key와 incremental state에 반영한다.
