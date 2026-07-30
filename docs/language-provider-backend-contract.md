# QStar Language Provider Backend Contract

Status: Q280 `qstar.lang/2` Stella/Ninja backend and provider parity seal.

QStar runtime은 built-in `c`, `cxx`, `asm` provider namespace를 preloaded registry로
갖고 있다. Public syntax의 `lang.c`, `lang.cxx`, `lang.asm`은 그대로 유지되지만, 내부
source classification과 tool role은 `c.compiler`, `cxx.compiler`, `asm.compiler` 같은
provider role로 내려간다. 외부 provider는 `qstar.use_language(...)`로 활성화하고,
provider source unit suffix는 graph-level source registry에 등록된다. Raw string source와
explicit provider helper token은 provider implementation의 lowering function이 반환한 action
template으로 Graph IR에 저장된다. Provider manifest가 `finals` schema를 선언하면
provider-owned final action template으로 낮아진다. `qstar.lang/1`은 기존 pure-provider
final 규칙을 유지하고, `qstar.lang/2`는 mixed-provider target, 명시적 input ownership,
file/tree multi-output artifact descriptor를 추가한다.

## Q257 stability decision

GLP에는 두 층이 있다.

| Layer | Q257 status | Meaning |
| --- | --- | --- |
| Consumer surface | v1 stable candidate | `qstar.use_language`, `lang.<namespace>`, provider helper export, raw provider source classification, provider final artifact selection, `qstar init --use-language` vendoring은 사용자-facing surface다. 이 층은 `docs/qstar-compatibility-policy.md`의 stable-at-v1 후보에 속한다. |
| Provider-author API | versioned beta | `qstar.language_provider`, `qstar.provider_tools`, `qstar.language_options`, `qstar.source`, `qstar.argv`, provider sandbox, manifest schema, scaffold schema, and lowering result schema는 provider 작성자가 의존하는 author API다. QStar는 `qstar.lang/1`과 `qstar.lang/2`를 구분해서 load하며 둘 다 아직 provider-author beta다. |

따라서 QStar v1 후보에서 사용자는 bundled/project-local provider를 안정 표면처럼 사용할 수
있지만, provider package를 직접 작성하는 사람에게는 `qstar.lang/1`과 `qstar.lang/2`
provider-author contract가 아직 beta다. 이 API를 stable로 올리려면 별도 라운드에서 sandbox
capability list, lowering result schema, scaffold schema, diagnostic wording, and standard
provider update policy를 freeze해야 한다.

Future provider manifest API versions are rejected instead of guessed. A manifest
with `api = "qstar.lang/999"` must fail with:

```txt
qstar: unsupported language provider api 'qstar.lang/999'; supported APIs: qstar.lang/1, qstar.lang/2
```

That diagnostic is part of the beta version boundary. QStar accepts exactly
`qstar.lang/1` and `qstar.lang/2`; later versions must be added deliberately with
schema validation, docs, and simultaneous-version tests.

## 결정

- 활성화된 외부 provider의 source suffix는 raw string `sources` classification에 참여한다.
- provider helper는 source-local option이나 suffix 충돌 해소를 위한 explicit
  `qstar.source(path, {language = "...", unit = "..."})` token을 만든다.
- raw string이 여러 provider source unit과 match되거나 built-in source suffix와 provider unit
  모두에 match되면 QStar는 explicit provider helper를 요구한다.
- source unit은 `ctx.tool`, `ctx.input`, `ctx.output`, `ctx.cache`, `ctx.option`으로 lowering된다.
- `qstar.lang/1` final artifact는 `ctx.input("sources")`, `ctx.output("artifact")`,
  `ctx.kind()`로 lowering된다. 모든 compile source가 같은 v1 provider에 속하고 native
  deps/link input이 없을 때만 native archive/link action을 대체한다.
- `qstar.lang/2` final은 manifest가 선언한 `sources`, `objects`, `link_interfaces`,
  `link_inputs`, `link_options` input class만 읽는다. 같은 provider source를 직접 소유하면서
  built-in 또는 다른 provider source의 object를 함께 소비할 수 있다.
- `qstar.lang/2` artifact descriptor는 file/tree, primary/secondary, runtime,
  link-interface 역할을 명시한다. QStar는 suffix, platform, linker policy를 추론하지 않는다.
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
| `api` | yes | Must be exactly `"qstar.lang/1"` or `"qstar.lang/2"`. | beta version discriminator |
| `id` | yes | Tool-role-safe provider id such as `zig`, `rust`, or `cuda`. | beta author field; consumer short-id behavior is stable candidate |
| `version` | yes | String stored in graph/listing output. | beta author field; not a semver resolver |
| `namespace` | yes | Tool-role-safe namespace used for `lang.<namespace>` and tool bundles. | beta author field; activated namespace behavior is stable candidate |
| `implementation` | yes | Package-relative `.lua` path loaded in provider sandbox. | beta |
| `tools` | no | Map of provider tool names to `{role, required}` entries. | beta author schema; consumer `tools.<namespace>` use is stable candidate |
| `units` | no | Map of source unit names to suffix/emits/lower/deps metadata. | beta author schema; raw source classification behavior is stable candidate |
| `finals` | no | Map of `executable`, `staticlib`, `sharedlib` to lowering functions. v2 entries also declare `inputs` and `artifacts`. | beta author schema; consumer final selection behavior is stable candidate |
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
| `outputs` | no | v1에서는 primary output을 포함한 action output list다. v2 final에서는 모든 manifest-owned artifact path를 정확히 포함해야 한다. |
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

위 예제는 `qstar.lang/1` compatibility contract다. QStar는 bundled Zig/Rust/CUDA의
기존 manifest와 project-local v1 provider를 계속 이 규칙으로 읽는다.

## qstar.lang/2 artifact contract

`qstar.lang/2`는 provider가 object 외의 최종 artifact graph를 명시적으로 소유할 수 있게
한다. Core가 이해하는 것은 언어 이름이나 compiler 관행이 아니라 다음 두 종류의 구조화된
metadata뿐이다.

1. final action이 소유하는 input class
2. final action이 생산하는 artifact descriptor

```lua
return qstar.language_provider {
  api = "qstar.lang/2",
  id = "pack",
  version = "0.2",
  namespace = "pack",
  implementation = "provider.lua",
  tools = {
    compiler = {role = "pack.compiler", required = true},
  },
  units = {
    object = {
      suffixes = {".p2"},
      emits = "object",
      lower = "compile_object",
    },
  },
  finals = {
    executable = {
      lower = "link_executable",
      inputs = {
        "sources",
        "objects",
        "link_interfaces",
        "link_inputs",
        "link_options",
      },
      artifacts = {
        runtime = {
          type = "file",
          roles = {"primary", "runtime"},
        },
        metadata = {
          type = "file",
          roles = {"secondary"},
          suffix = ".metadata",
        },
        resources = {
          type = "tree",
          roles = {"secondary", "runtime"},
          suffix = ".resources",
        },
        link = {
          type = "file",
          roles = {"secondary", "link-interface"},
          suffix = ".link",
        },
      },
    },
  },
  exports = {
    tools = "tools",
    options = "options",
    object = "object",
  },
}
```

### Final input ownership

| Input id | Lua value | 의미 |
| --- | --- | --- |
| `sources` | `ctx.input("sources")` string list | Final owner와 같은 namespace의 source 중 provider가 직접 처리할 source. 이 class를 선언하지 않으면 같은 provider source도 먼저 object로 낮아진다. |
| `objects` | `ctx.input("objects")` string list | Built-in C/C++/ASM, 다른 GLP provider, raw object source, `qstar.objectlib`가 생산한 object path. |
| `link_interfaces` | `ctx.input("link_interfaces")` string list | `deps`와 `private_deps`가 노출하는 명시적 link-interface artifact path. Interface-only target은 그 dependency artifact를 전달한다. |
| `link_inputs` | `ctx.input("link_inputs")` string list | Target의 명시적 `link_inputs`. |
| `link_options` | `ctx.input("link_options")` string list | Target의 명시적 argv option. File dependency가 아니므로 provider가 command argv에 넣을 때만 실행 인자가 된다. |

`ctx.input(name)`은 manifest `inputs`에 선언된 class만 반환한다. Target composition이
object, dependency, link input 또는 link option을 필요로 하는데 manifest가 해당 class를
선언하지 않으면 graph evaluation 단계에서 error가 난다. `libs`, `lib_dirs`, `frameworks`는
v2 provider가 암묵적으로 번역할 수 없다. 해당 policy는 provider가 별도 explicit authoring
surface로 받거나 project가 native final action을 사용해야 한다.

한 target의 source에 final hook을 제공하는 v2 provider가 정확히 하나면 그 provider가 final
owner가 된다. Built-in source와 final hook이 없는 다른 provider source는 object로 합성된다.
서로 다른 v2 provider 둘 이상이 같은 target kind의 final hook을 제공하면 QStar는 source 순서로
임의 선택하지 않고 multiple-final-owner collision을 진단한다.

### Artifact descriptor

| Field | Required | 의미 |
| --- | --- | --- |
| descriptor key | yes | `qstar.target_file(label, {artifact = "..."})` selector가 되는 id. |
| `type` | yes | `file` 또는 `tree`. Tree는 action이 소유하는 directory artifact다. |
| `roles` | yes | `primary`, `secondary`, `runtime`, `link-interface`의 중복 없는 list. |
| `suffix` | secondary only | Primary artifact path 뒤에 붙는 filename suffix. 생략하면 `.<descriptor-id>`. Slash, backslash, `..`는 금지한다. |

Final마다 정확히 하나의 `primary`가 있어야 하고 각 descriptor는 `primary` 또는 `secondary`
중 정확히 하나를 가진다. `runtime`은 실행/배치에 의미 있는 산출물 metadata이고,
`link-interface`는 dependency consumer에게 전달할 artifact다. Final 하나에 link-interface는
최대 하나이며 tree artifact는 link-interface가 될 수 없다. Primary path는 target의 명시적
`artifact_name`과 generic target output policy를 따르지만, secondary suffix와 모든 역할은
provider manifest가 직접 선언한다.

```lua
function P.link_executable(ctx)
  local argv = qstar.argv()
  local inputs = {}

  argv:add(ctx.tool("compiler"))
  argv:add("final")
  argv:add(ctx.output("runtime"))
  argv:add(ctx.output("metadata"))
  argv:add(ctx.output("resources"))
  argv:add(ctx.output("link"))

  for _, value in ipairs(ctx.input("sources")) do
    argv:add("source=" .. value)
    table.insert(inputs, value)
  end
  for _, value in ipairs(ctx.input("objects")) do
    argv:add("object=" .. value)
    table.insert(inputs, value)
  end
  for _, value in ipairs(ctx.input("link_interfaces")) do
    argv:add("interface=" .. value)
    table.insert(inputs, value)
  end

  return {
    command = argv,
    inputs = inputs,
    outputs = {
      ctx.output("runtime"),
      ctx.output("metadata"),
      ctx.output("resources"),
      ctx.output("link"),
    },
  }
end
```

Provider lowering result의 `outputs`는 모든 descriptor path를 정확히 포함해야 한다. 선언한
artifact가 빠지거나 descriptor가 소유하지 않는 output을 추가하면 error다. 이 규칙으로
file/tree multi-output의 producer ownership, cache key, action-log, replay, Stella edge,
Ninja edge가 같은 계약을 공유한다. `ctx.output("artifact")`는 v1 호환과 migration을 위한
primary alias로 남지만 v2 provider는 descriptor id를 쓰는 것이 권장된다.

`qstar.lang/2`는 platform, target triple, sysroot, runtime, linker flag를 자동으로 만들지
않는다. Manifest metadata와 lowering argv가 명시한 것만 실행한다. 따라서 v2가 QStar core에
새 언어나 특정 toolchain 의미론을 추가하는 통로가 되어서는 안 된다.

전용 회귀 gate는 다음과 같다.

```sh
make qstar-glp-v2-artifact-contract-tests
```

이 gate는 v1/v2 동시 activation, mixed C/provider composition, imported link interface,
file/tree multi-output, selector, Stella/Ninja, action-log/replay, version collision, malformed
descriptor, missing ownership, missing/unowned output을 fake tool로 검증한다.

## Q280 backend parity seal

Q280은 v2 manifest를 load할 수 있다는 사실과 v2 action이 두 backend에서 같은 의미를
갖는다는 사실을 분리해서 검증한다. 정식 gate는 다음과 같다.

```sh
make qstar-glp-v2-backend-parity-tests
```

이 gate의 project-local reference provider는 `zig`, `rust`, `cuda` namespace를 사용한다.
따라서 consumer 문법은 bundled provider와 동일한 `lang.zig`, `lang.rust`, `lang.cuda`,
`tools.zig`, `tools.rust`, `tools.cuda`, raw `.zig`/`.rs`/`.cu` source다. 다만 fixture manifest만
`qstar.lang/2`이며, bundled standard provider manifest는 기존 `qstar.lang/1` compatibility
corpus와 optional real compiler corpus를 위해 그대로 유지한다.

| Contract | Stella | Ninja | Observability |
| --- | --- | --- | --- |
| Source action | Provider argv/env/input/output/depfile로 실행 | 같은 Graph IR을 Ninja edge로 생성 | `provider_action`, `provider_actions`, `provider_action_contract` |
| Final action | Mixed-provider object와 explicit link input을 소비 | 같은 input closure와 command를 사용 | `query`, `explain`, `dry-run` |
| Response file | 공통 logical argv의 tail로 real `.rsp` 생성 | 같은 materializer가 동일 argv tail로 `.rsp` 생성 | logical argc/digest, response path/style/digest, exec argc |
| Depfile | Source/final action 모두 discovered input을 action key에 반영 | `depfile`과 `deps = gcc` edge로 전달 | `why-rebuild`의 `depfile-changed` |
| Environment | Process overlay 및 env digest에 반영 | edge-local env overlay 적용 | 이름만 Graph IR에 노출하고 log/replay 값은 redacted |
| Multi-output | file/tree 전체 존재 여부와 cache hit 검사 | 한 edge의 ordered outputs | action-log와 replay의 `output_count`, `output[N]` |
| Compile database | 실제 실행되는 provider object action 기록 | 동일 source/object/command record 기록 | provider final이 직접 소유한 source는 compile DB에서 제외 |
| Runtime/link interface | Runtime은 `target_file` primary, link-interface는 consumer dependency input | 동일 | Windows `.exe`/`.dll` runtime과 provider-owned import artifact 선택 fixture |

Provider final depfile은 compile action과 동일하게 `wants_depfile` 계약을 사용한다. 실행 후
depfile을 읽어 discovered input digest를 state에 반영하며, key를 재계산할 때 action의 실제
kind(`archive`, `link`, `link-shared`)를 보존한다. 따라서 첫 build 직후 두 번째 Stella build가
불필요하게 다시 실행되지 않고, depfile input 변경은 정확히 `depfile-changed`가 된다.

Graph/query/explain은 provider env의 값은 출력하지 않는다. Graph IR에는 `env_names`만
나타나며 action-log/replay는 기존처럼 `NAME=<redacted>`를 기록한다. Provider source action의
`argv_template`, input/output, depfile과 final action의 ownership도 additive query field로
노출한다.

### Q287 wide final materialization

Provider final도 built-in final과 같은 `qstar_lowered_action` 및 response materializer를
사용한다. Provider가 반환한 command는 full logical argv이며, mixed-provider object,
file/tree artifact, dependency link interface, explicit input과 option의 순서를 보존한다.
Stella와 Ninja는 이 값을 따로 재조립하지 않는다.

Response file을 쓰더라도 action key, local CAS, `why-rebuild`, state identity, action log,
replay는 `@rsp` path가 아니라 full logical argv를 사용한다. `dry-run`과 `explain`은
filesystem mutation 없이 같은 `logical_argc`, `logical_argv_digest`, `response_digest`,
`exec_argc`를 예측한다. `make qstar-glp-v2-backend-parity-tests`는 GLP v2 final에서 plan과
두 backend action log의 digest가 일치하는지 검증한다.

Windows 연동은 provider가 MSVC, import library 이름 또는 linker option을 추론한다는 뜻이
아니다. QStar의 generic target output policy가 runtime primary path를 정하고, manifest가
선언한 `runtime`/`link-interface` 역할에 따라 `qstar.target_file`과 dependency consumer가
서로 다른 artifact를 선택한다. Suffix와 command argv는 계속 provider 소유다.

실제 compiler 검증은 별도 optional gate다.

```sh
make qstar-real-glp-compiler-corpus-tests
```

이 optional corpus는 bundled lang/1 Rust/Zig provider의 real compiler 호환성을 보호한다.
Q280 fake-tool v2 gate를 real compiler 지원 주장으로 확대 해석해서는 안 된다.

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
