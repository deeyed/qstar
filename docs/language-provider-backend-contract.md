# QStar Language Provider Backend Contract

Status: Q257 provider-author API stability boundary.

QStar runtime은 built-in `c`, `cxx`, `asm` provider namespace를 preloaded registry로
갖고 있다. Public syntax의 `lang.c`, `lang.cxx`, `lang.asm`은 그대로 유지되지만, 내부
source classification과 tool role은 `c.compiler`, `cxx.compiler`, `asm.compiler` 같은
provider role로 내려간다. 외부 provider는 `qstar.use_language(...)`로 활성화하고,
provider source unit suffix는 graph-level source registry에 등록된다. Raw string source와
explicit provider helper token은 provider implementation의 lowering function이 반환한 action
template으로 Graph IR에 저장된다. Provider manifest가 `finals` schema를 선언하면 pure-provider
artifact target은 provider-owned final action template으로 낮아진다.

## Q257 stability decision

GLP에는 두 층이 있다.

| Layer | Q257 status | Meaning |
| --- | --- | --- |
| Consumer surface | v1 stable candidate | `qstar.use_language`, `lang.<namespace>`, provider helper export, raw provider source classification, provider final artifact selection, `qstar init --use-language` vendoring은 사용자-facing surface다. 이 층은 `docs/qstar-compatibility-policy.md`의 stable-at-v1 후보에 속한다. |
| Provider-author API | versioned beta | `qstar.language_provider`, `qstar.provider_tools`, `qstar.language_options`, `qstar.source`, `qstar.argv`, provider sandbox, manifest schema, scaffold schema, and lowering result schema는 provider 작성자가 의존하는 author API다. Q257은 이 API를 stable로 승격하지 않는다. 대신 `api = "qstar.lang/1"`라는 versioned beta contract로 문서화한다. |

따라서 QStar v1 후보에서 사용자는 bundled/project-local provider를 안정 표면처럼 사용할 수
있지만, provider package를 직접 작성하는 사람에게는 `qstar.lang/1` provider-author contract가
아직 beta다. 이 API를 stable로 올리려면 별도 라운드에서 manifest version negotiation, sandbox
capability list, lowering result schema, scaffold schema, diagnostic wording, and standard
provider update policy를 freeze해야 한다.

Future provider manifest API versions are rejected instead of guessed. A manifest
with `api = "qstar.lang/999"` must fail with:

```txt
qstar: language provider api must be "qstar.lang/1"
```

That diagnostic is part of the beta version boundary: current QStar accepts only
`qstar.lang/1`; future versions must be added deliberately with docs and tests.

## 결정

- 활성화된 외부 provider의 source suffix는 raw string `sources` classification에 참여한다.
- provider helper는 source-local option이나 suffix 충돌 해소를 위한 explicit
  `qstar.source(path, {language = "...", unit = "..."})` token을 만든다.
- raw string이 여러 provider source unit과 match되거나 built-in source suffix와 provider unit
  모두에 match되면 QStar는 explicit provider helper를 요구한다.
- source unit은 `ctx.tool`, `ctx.input`, `ctx.output`, `ctx.cache`, `ctx.option`으로 lowering된다.
- final artifact는 `ctx.input("sources")`, `ctx.output("artifact")`, `ctx.kind()`로 lowering된다.
- provider final action은 모든 compile source가 같은 외부 provider에 속하고 target이 native
  deps/link input을 갖지 않을 때 native archive/link action을 대체한다.
- lowered action의 argv, env, inputs, outputs, depfile은 Stella와 Ninja가 같은 contract로 실행한다.
- provider action `env`는 `"NAME=value"` string list다. 실행 backend는 값을 실제 process에 넘기지만 action-log/replay에는 `NAME=<redacted>`로만 기록한다.
- object artifact bridge도 계속 지원된다.
- QStar는 외부 compiler의 AST, module system, semantic import/export, package manager,
  internal API를 해석하거나 호출하지 않는다.

## Manifest schema boundary

Provider manifests must return `qstar.language_provider { ... }`. The currently
accepted provider-author schema is:

| Field | Required | Current validation | Q257 stability |
| --- | --- | --- | --- |
| `api` | yes | Must be the exact string `"qstar.lang/1"`. | beta version discriminator |
| `id` | yes | Tool-role-safe provider id such as `zig`, `rust`, or `cuda`. | beta author field; consumer short-id behavior is stable candidate |
| `version` | yes | String stored in graph/listing output. | beta author field; not a semver resolver |
| `namespace` | yes | Tool-role-safe namespace used for `lang.<namespace>` and tool bundles. | beta author field; activated namespace behavior is stable candidate |
| `implementation` | yes | Package-relative `.lua` path loaded in provider sandbox. | beta |
| `tools` | no | Map of provider tool names to `{role, required}` entries. | beta author schema; consumer `tools.<namespace>` use is stable candidate |
| `units` | no | Map of source unit names to suffix/emits/lower/deps metadata. | beta author schema; raw source classification behavior is stable candidate |
| `finals` | no | Map of `executable`, `staticlib`, `sharedlib` to lowering functions. | beta author schema; consumer final selection behavior is stable candidate |
| `options` | no | Map of option names to `string`, `bool`/`boolean`, `list`, or `enum` schemas plus defaults. | beta author schema; validated `lang.<namespace>` consumer behavior is stable candidate |
| `exports` | yes | Map of user-visible helper names to implementation table fields. Must not be empty. | beta author schema; returned helper table behavior is stable candidate |
| `scaffold` | no | Optional `api = "qstar.scaffold/1"` metadata used by `qstar init`. | beta author schema; `qstar init --use-language` vendoring behavior is stable candidate |

Manifest validation is deliberately allowlist-based. Unknown fields are rejected
so provider packages cannot accidentally depend on unimplemented or future
surface.

## Provider implementation sandbox boundary

`provider.lua` is not a graph authoring file. It is loaded in a restricted
provider sandbox and returns an implementation table. The implementation can
construct helper values and lowering action templates; it cannot declare project
targets, configs, stages, commands, subdirs, or ordinary helper modules.

Provider-author helper APIs in the sandbox:

| API | Purpose | Q257 stability |
| --- | --- | --- |
| `qstar.provider_tools(namespace, table)` | Return a provider namespace tool bundle such as `zig.tools { compiler = qstar.cli {"zig"} }`. | beta author API |
| `qstar.language_options(namespace, table)` | Return a `lang.<namespace>` option table that will be validated against the manifest option schema. | beta author API |
| `qstar.source(path, metadata)` | Return an explicit typed source unit token such as `zig.object("src/main.zig", {...})`. | beta author API; consumer helper use is stable candidate |
| `qstar.argv()` | Build a lowered argv vector returned by source/final lowering functions. | beta author API |
| `qstar.host.os`, `qstar.host.arch` | Read-only host facts for argv construction. | stable authoring constants, available in provider sandbox |

The sandbox capability list itself is beta. Provider authors must not assume
additional Lua globals, filesystem access, network access, shell execution, or
graph declaration APIs.

## 현재 backend별 동작

| Source kind | Stella | Ninja |
| --- | --- | --- |
| C | compile action | lowered |
| C++ | compile action | lowered |
| ASM | compile action | lowered |
| Provider source unit | lowered provider action | lowered provider action |
| Provider final artifact | lowered provider action | lowered provider action |
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

Lowered action result schema:

| Field | Required | Meaning |
| --- | --- | --- |
| `command` | yes | `qstar.argv()` result. Shell strings are not accepted. |
| `env` | no | List of `"NAME=value"` strings. Runtime receives real values; action-log/replay records `NAME=<redacted>`. |
| `inputs` | no | Additional package-relative paths or resolved context paths that participate in the action key and backend dependency edge. |
| `outputs` | no | Additional output paths beyond the primary object/final artifact. |
| `depfile` | no | Make-style depfile path. If present, Stella/Ninja consume discovered inputs through the same backend contract. |

The result schema is implemented and backend-parity tested, but Q257 keeps it in
the provider-author beta bucket. Consumer-facing behavior produced by bundled
providers is the stable candidate; provider authors should expect schema
versioning or a diagnostic compatibility pass before v1 promotion.

`ctx.tool("compiler")`는 provider namespace를 붙여 `zig.compiler` 같은 toolset role로
해석된다. `ctx.tool("zig.compiler")`처럼 완전한 role 이름을 직접 쓸 수도 있다.
`ctx.cache(name)`은 현재 provider source action의 object directory 아래 package-relative
cache path를 반환한다. `name`은 package-relative path여야 하며 absolute path나 `..`는
허용하지 않는다.
`ctx.option(name)`은 source-local option, target/config `lang.<namespace>` option, schema
default 순서로 값을 반환한다. `depfile`이 있거나 unit schema의 `deps = "make"`가 설정되면
backend는 depfile-discovered input을 action key와 incremental state에 반영한다.
Provider sandbox는 graph entrypoint를 호출할 수 없지만, read-only `qstar.host.os`와
`qstar.host.arch`는 읽을 수 있다. Provider는 이 값을 사용해 host-specific argv를 만들 수
있지만, target이나 dependency graph를 새로 선언할 수는 없다.

Provider final lowering도 같은 action table을 반환한다.

```lua
function P.link_executable(ctx)
  local argv = qstar.argv()
  argv:add(ctx.tool("compiler"))
  argv:add("build-exe")
  argv:add_all(ctx.input("sources"))
  argv:add("-femit-bin=" .. ctx.output("artifact"))

  return {
    command = argv,
    inputs = ctx.input("sources"),
    outputs = {ctx.output("artifact")},
  }
end
```

## Standard provider compatibility promise

QStar currently ships standard `zig`, `rust`, and `cuda` providers. Q257 assigns
them the following compatibility boundary:

| Provider | Consumer-facing v1 candidate | Provider-author/internal beta |
| --- | --- | --- |
| `zig` | short id `zig`, namespace `lang.zig`, helper exports `zig.tools`, `zig.options`, `zig.object`, `.zig` raw source classification, documented options `target`, `optimize`, `macos_min_version`, `compile_options`, and provider final actions for executable/staticlib/sharedlib | exact `provider.lua` argv construction, cache directory naming, and lowering hook internals |
| `rust` | short id `rust`, namespace `lang.rust`, helper exports `rust.tools`, `rust.options`, `rust.object`, `.rs` raw source classification, documented options `edition`, `crate_type`, `cfg`, `externs`, `compile_options`, and provider final actions for executable/staticlib/sharedlib | exact `rustc` argv construction, final-action helper internals, and future Cargo/build.rs integration decisions |
| `cuda` | short id `cuda`, namespace `lang.cuda`, helper exports `cuda.tools`, `cuda.options`, `cuda.object`, `.cu` raw source classification, documented options `arch`, `compile_options`, and object unit lowering | exact `nvcc` argv construction and any future CUDA final-action design |

Installed standard provider bundle lookup and `qstar init --use-language` vendoring
are consumer-facing stable candidates. Project-local provider override behavior is
also a stable candidate: project-local `qstar/languages/<id>/<id>.qsm` wins over
the installed standard provider with the same id.

The standard provider implementation files remain part of the beta
provider-author surface. They can change implementation strategy as long as the
documented consumer-facing ids, namespaces, helper names, option schemas, source
classification behavior, and init vendoring behavior remain compatible or follow
the compatibility policy.

## Standard provider compatibility coverage

Q258 adds a dedicated fake-tool gate for the bundled standard providers:

```sh
make qstar-standard-provider-compatibility-tests
```

This gate is separate from the optional real compiler corpus. It does not require
`zig`, `rustc`, or `nvcc` to be installed. Instead, it validates the QStar-facing
contract with deterministic fake compilers so the compatibility surface can run
as part of ordinary regression testing.

The coverage is:

| Area | Zig | Rust | CUDA |
| --- | --- | --- | --- |
| Standard bundle lookup | `qstar.use_language("zig")` resolves the bundled manifest when the project does not vendor `qstar/languages/zig`. | `qstar.use_language("rust")` resolves the bundled manifest without project-local vendoring. | `qstar.use_language("cuda")` resolves the bundled manifest without project-local vendoring. |
| Tool bundle syntax | `tools.zig = zig.tools { compiler = qstar.cli {...} }` records `zig.compiler`. | `tools.rust = rust.tools { compiler = qstar.cli {...} }` records `rust.compiler`. | `tools.cuda = cuda.tools { compiler = qstar.cli {...} }` records `cuda.compiler`. |
| Option schema | `target`, `optimize`, `macos_min_version`, and `compile_options` are accepted; invalid `optimize` enum values fail. | `edition`, `crate_type`, `cfg`, `externs`, and `compile_options` are accepted; invalid `crate_type` enum values fail. | `arch` and `compile_options` are accepted; wrong string/list types fail. |
| Raw source classification | `sources = {"src/main.zig"}` lowers through the Zig source registry. | `sources = {"src/main.rs"}` lowers through the Rust source registry. | `sources = {"src/main.cu"}` lowers through the CUDA source registry. |
| Final or object lowering | `qstar.staticlib` with only Zig sources uses the provider final staticlib action. | `qstar.staticlib` with only Rust sources uses the provider final staticlib action. | CUDA source lowers to provider-owned object compile action and then normal archive consumption. |
| Backend contract | `check`, `--dump-graph`, `explain`, `dry-run`, Stella `build`, `action-log`, `replay`, Ninja `build`, Ninja `action-log`, and Ninja `replay`. | Same. | Same. |

The optional `make qstar-real-glp-compiler-corpus-tests` remains the real
compiler evidence path. It proves that the standard providers' generated argv can
drive real Rust/Zig compilers when available. The Q258 standard provider gate
proves that the QStar DSL/Graph/backend compatibility contract itself does not
regress even on machines without those compilers.
