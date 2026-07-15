# Language Providers

QStar는 특정 언어에 종속되지 않는 빌드시스템이다. 현재 runtime은 built-in `c`, `cxx`,
`asm` provider namespace를 preloaded registry로 다룬다. Public syntax의 `lang.c`,
`lang.cxx`, `lang.asm`은 초보자 친화 표면으로 계속 유지되지만, 내부 source
classification과 tool role은 `c.compiler`, `cxx.compiler`, `asm.compiler` 같은 provider
role로 내려간다.

외부 provider는 `qstar.use_language(...)`로 명시적으로 활성화한다. Activation은 provider
manifest를 읽고 `lang.<namespace>`를 허용하는 registry를 갱신하며, manifest의 `options`
schema로 `lang.<namespace>` table을 검증한다. Manifest의 `units` schema는 provider helper가
`qstar.source(...)` token을 만들 수 있게 하며, 같은 suffix 정보를 graph-level source registry에
등록한다. 따라서 활성화된 provider의 `.zig`, `.rs`, `.cu` 같은 source는 raw string source
classification에도 참여한다. Backend는 provider implementation의 lowering function이 반환한
action template을 consuming target 소유 object artifact 또는 provider-owned final artifact로
낮춘다. `qstar.lang/1` final은 기존 pure-provider 규칙을 유지한다. `qstar.lang/2` final은
manifest가 input ownership과 file/tree artifact descriptor를 선언하므로 built-in source와
여러 provider source가 섞인 target도 명시적 object/artifact contract로 합성할 수 있다.
외부 compiler 호출을 직접 손으로 제어해야 하는 경우에는 object artifact bridge를 계속 쓴다.

## Stability Boundary

Q257 기준 GLP는 consumer surface와 provider-author surface를 분리한다.

| Layer | 현재 상태 | 의미 |
| --- | --- | --- |
| Consumer surface | v1 stable 후보 | `qstar.use_language`, `lang.<namespace>`, provider helper export, raw provider source classification, provider final artifact selection, `qstar init --use-language` vendoring은 사용자가 쓰는 표면이다. |
| Provider-author API | versioned beta | `qstar.language_provider`, `qstar.provider_tools`, `qstar.language_options`, `qstar.source`, `qstar.argv`, provider sandbox, manifest schema, scaffold schema, lowering result schema는 provider 작성자가 의존하는 표면이며 아직 v1 stable이 아니다. QStar는 `qstar.lang/1`과 `qstar.lang/2`를 별도 schema로 협상한다. |

현재 QStar가 받는 provider manifest version은 `api = "qstar.lang/1"`과
`api = "qstar.lang/2"`다. 다른 version을 추측해서 실행하지 않으며, 예를 들어
`api = "qstar.lang/999"`는 다음
diagnostic으로 거절된다.

```txt
qstar: unsupported language provider api 'qstar.lang/999'; supported APIs: qstar.lang/1, qstar.lang/2
```

표준 `zig`, `rust`, `cuda` provider의 short id, namespace, documented helper,
documented option schema, raw source classification, init vendoring behavior는
consumer-facing stable 후보로 취급한다. 반면 각 provider의 `provider.lua` 내부 argv 구성,
cache layout, lowering hook 구현은 beta다.

## Standard Provider Compatibility Coverage

Q258부터 bundled Zig/Rust/CUDA provider는 전용 fake-tool gate로 반복 검증한다.

```sh
make qstar-standard-provider-compatibility-tests
```

이 gate는 실제 `zig`, `rustc`, `nvcc` 설치를 요구하지 않는다. 실제 compiler 검증은
optional `make qstar-real-glp-compiler-corpus-tests`가 맡고, standard provider compatibility
gate는 QStar가 약속하는 consumer-facing 표면이 Stella와 Ninja에서 깨지지 않는지를 본다.

검증 범위:

| Surface | Coverage |
| --- | --- |
| Standard bundle lookup | project-local vendoring 없이 `qstar.use_language("zig")`, `qstar.use_language("rust")`, `qstar.use_language("cuda")`가 설치/checkout standard bundle을 찾는다. |
| Tool bundle syntax | `tools.zig = zig.tools {...}`, `tools.rust = rust.tools {...}`, `tools.cuda = cuda.tools {...}`가 각각 `zig.compiler`, `rust.compiler`, `cuda.compiler` role을 등록한다. |
| Option schema | `lang.zig`, `lang.rust`, `lang.cuda`의 documented option success path와 enum/string/list failure diagnostic을 확인한다. |
| Raw source classification | `sources = {"src/main.zig"}`, `sources = {"src/main.rs"}`, `sources = {"src/main.cu"}`가 활성화된 provider source registry를 통해 낮아진다. |
| Provider lowering | Zig/Rust `qstar.staticlib`는 provider final staticlib action으로, CUDA는 provider object compile action과 normal archive consumption으로 낮아진다. |
| Backend parity | `check`, `--dump-graph`, `explain`, `dry-run`, Stella `build`, `action-log`, `replay`, Ninja `build`, Ninja `action-log`, Ninja `replay`를 확인한다. |

이 coverage가 의미하는 것은 “standard provider implementation 내부가 stable”이라는 뜻이 아니다.
Stable 후보는 short id, namespace, helper 이름, documented option schema, raw source suffix
classification, provider final/object lowering behavior 같은 사용자 표면이다. `provider.lua` 내부
argv 구성, cache layout, lowering hook 구현은 계속 provider-author beta bucket에 남는다.

## Provider Activation

QStar는 표준 Zig/Rust/CUDA provider를 설치물에 함께 포함한다. 따라서 일반 사용자는 provider
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
없으면 installed standard provider bundle의 `zig` provider를 읽는다. `rust`, `cuda`도 같은
short-id 규칙을 따른다. 명시적 folder form은 project-relative
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
`namespace`, `implementation`, `tools`, `units`, `finals`, `options`, `exports`, `scaffold` schema를
검증한다.
`provider.lua`는 별도의 제한 sandbox에서 로드되는 implementation이다. Provider 작성자 API와
사용자 API는 `exports` table로 분리된다.

Manifest field boundary:

| Field | Required | Meaning |
| --- | --- | --- |
| `api` | yes | 정확히 `"qstar.lang/1"` 또는 `"qstar.lang/2"`. |
| `id` | yes | Short id. `qstar.use_language("id")`와 provider vendoring에 쓰인다. |
| `version` | yes | Provider package version string. Resolver 의미론은 없다. |
| `namespace` | yes | `lang.<namespace>`와 `tools.<namespace>`에 쓰이는 namespace. |
| `implementation` | yes | package-relative `provider.lua` path. |
| `tools` | no | Provider tool roles such as `compiler = {role = "zig.compiler", required = true}`. |
| `units` | no | Source unit suffix, output kind, lowering function, depfile policy. |
| `finals` | no | Provider-owned `executable`, `staticlib`, `sharedlib` final action hooks. v2는 각 final에 `inputs`, `artifacts`도 요구한다. |
| `options` | no | `lang.<namespace>` option schema. |
| `exports` | yes | User-visible helper name -> implementation field map. |
| `scaffold` | no | Optional `qstar.scaffold/1` init metadata. |

Provider implementation lowering result boundary:

| Field | Required | Meaning |
| --- | --- | --- |
| `command` | yes | `qstar.argv()` result. Shell string은 허용하지 않는다. |
| `env` | no | `"NAME=value"` list. 실행에는 실제 값이 전달되고 action-log/replay에는 redacted된다. |
| `inputs` | no | Action key와 backend dependency edge에 들어갈 추가 input. |
| `outputs` | no | v1 action output list. v2 final은 모든 descriptor-owned output을 정확히 포함해야 한다. |
| `depfile` | no | Make-style depfile path. Stella/Ninja가 같은 contract로 소비한다. |

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
  finals = {
    executable = {
      lower = "link_executable",
    },
    staticlib = {
      lower = "archive_staticlib",
    },
    sharedlib = {
      lower = "link_sharedlib",
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
      macos_min_version = "11.0",
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
              "src/main.zig",
            },
          },
        },
      },
    },
  },
}
```

위 manifest는 기존 `qstar.lang/1` 예제다. Bundled Zig/Rust/CUDA와 기존 project-local
provider는 변경 없이 이 loader를 계속 사용한다.

## qstar.lang/2 Final Artifact

v2는 언어나 toolchain별 built-in을 늘리지 않고 provider가 최종 산출물 계약을 구조화해서
선언하는 API다. Core가 해석하는 built-in key는 final의 `inputs`, artifact의 `type`, `roles`,
`suffix`뿐이다. Artifact id와 provider namespace는 작성자가 자유롭게 정하지만 identifier
검증을 통과해야 한다.

```lua
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
}
```

Input class는 fixed contract다.

| 이름 | `ctx.input(...)` 결과 | 책임 |
| --- | --- | --- |
| `sources` | source path list | Final owner와 같은 provider namespace의 source를 provider가 직접 소비한다. |
| `objects` | object path list | Built-in C/C++/ASM, 다른 provider, raw object, objectlib output을 받는다. |
| `link_interfaces` | artifact path list | `deps`/`private_deps`의 명시적 link-interface artifact를 받는다. |
| `link_inputs` | path list | Target의 explicit `link_inputs`를 받는다. |
| `link_options` | argv string list | Target의 explicit `link_options`를 받는다. 자동 flag 번역은 없다. |

Manifest에 선언하지 않은 input class를 `ctx.input`으로 읽을 수 없다. Target이 object나
dependency를 필요로 하는데 해당 ownership을 빠뜨려도 error다. `libs`, `lib_dirs`,
`frameworks`에서 provider argv를 자동 추론하지 않는다.

Artifact descriptor 규칙:

- `type`은 `file` 또는 `tree`다.
- `roles`는 `primary`, `secondary`, `runtime`, `link-interface`만 허용한다.
- Final 하나에는 primary가 정확히 하나 있어야 한다.
- 각 artifact는 primary/secondary 중 정확히 하나를 가져야 한다.
- link-interface는 최대 하나이고 tree에는 붙일 수 없다.
- Secondary `suffix`는 `.`으로 시작하는 filename suffix다. 생략하면 `.<artifact-id>`다.
- Descriptor key는 `qstar.target_file(label, {artifact = "key"})` selector다.
- Provider lowering `outputs`에는 선언된 artifact가 모두 있어야 하며 미선언 output은 금지한다.

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

한 target에서 final hook을 가진 v2 provider가 하나면 그 provider가 final owner가 된다.
나머지 built-in/foreign provider source는 `objects`로 합성된다. 서로 다른 v2 provider가 같은
target kind의 final hook을 함께 제공하면 source 순서로 선택하지 않고 collision error가 난다.
Primary/secondary/runtime/link-interface metadata는 Graph IR, `query --format json`,
`qstar.target_file`, dependency selection, Stella/Ninja multi-output edge에 동일하게 반영된다.

`ctx.output("artifact")`는 v1 compatibility alias로 남지만, v2 구현은 descriptor id를
사용해야 의도가 분명하다. v2 provider도 platform, triple, sysroot, linker/runtime policy를
추론해서는 안 된다. 필요한 argv는 provider option과 target의 explicit input을 통해 직접
기술한다.

```sh
make qstar-glp-v2-artifact-contract-tests
```

이 gate는 v1/v2 동시 activation, mixed-provider composition, file/tree multi-output,
runtime/link-interface selector, malformed manifest/action ownership, Stella/Ninja parity를
검증한다.

## Provider Init Scaffold Metadata

`scaffold`는 선택 field다. 있으면 QStar는 `api = "qstar.scaffold/1"`, `tools`,
`options`, `shapes`를 manifest load 시점에 검증한다. `qstar init`은 primary
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

return P
```

현재 runtime은 manifest와 implementation을 모두 읽고 검증한 뒤, `exports`가 가리키는
implementation field만 `qstar.use_language(...)`의 반환 table에 노출한다. 예를 들어 위
manifest에서는 사용자 코드가 `zig.tools`, `zig.options`, `zig.object`를 볼 수 있다. `options` schema는
`string`, `bool`/`boolean`, `list`, `enum` 타입과 `default` metadata를 지원한다. 사용자가
`lang.zig`에 schema에 없는 key를 쓰거나 타입이 맞지 않는 값을 넣으면 diagnostic이 난다.

Provider-author API는 beta이므로, provider package를 배포하는 작성자는
`docs/language-provider-backend-contract.md`와 `docs/qstar-compatibility-policy.md`의 Q257
boundary를 함께 봐야 한다. 일반 project 사용자는 provider manifest나 `provider.lua` 구현을 직접
다루지 않고 `qstar.use_language`가 반환한 helper만 사용한다.

`qstar.import_module(...)`로 provider를 조용히 등록할 수 없다. 일반 helper `.qsm` 평가 중
`qstar.use_language(...)`를 호출하는 것도 금지된다. Provider manifest 안에서 다른 provider
dependency를 활성화하는 경우만 허용된다.

## 현재 source 경로

외부 provider source도 raw string `sources` classification에 참여한다. Provider manifest의
`units.<unit>.suffixes`가 graph-level source registry에 등록되므로, provider가 활성화된 뒤에는
`"src/main.zig"` 같은 source string이 provider object unit으로 바로 낮아진다. 예를 들어
`qstar.use_language("zig")` 이후 `sources = {"src/main.zig"}`는 `zig.object` helper를 직접
쓰지 않아도 consuming target이 소유하는 deterministic object output을 만든다.

Provider가 노출한 helper도 계속 정식 문법이다. Helper는
`qstar.source(path, {language = "...", unit = "..."})` token을 반환하고, source-local option이
필요하거나 suffix가 여러 provider와 충돌할 때 명시 경로로 쓴다.

```lua
local zig = qstar.use_language("zig")

qstar.staticlib "core" {
  sources = {
    "src/main.zig",
    zig.object("src/special.zig", {optimize = "ReleaseFast"}),
  },
}
```

Raw string이 활성화된 provider source unit 두 개 이상과 동시에 match되면 QStar는 ambiguous
source diagnostic을 낸다. Raw string이 built-in C/C++/ASM suffix와 provider source unit 모두에
match되어도 같은 이유로 거절된다. 이 경우에는 diagnostic이 제안하는 explicit provider helper를
써서 의도를 고정한다.

## Zig Provider Notes

표준 Zig provider는 real `zig build-obj` object unit과 `zig build-exe`/`zig build-lib`
provider final action을 지원한다. 기본 option은 `target = "native"`, `optimize = "Debug"`,
`macos_min_version = ""`, `compile_options = {}`이다.

```lua
local zig = qstar.use_language("zig")

qstar.toolset "host" {
  tools = {
    c = { compiler = qstar.cli {"cc"} },
    archive = qstar.cli {"ar"},
    link = qstar.cli {"cc"},
    zig = zig.tools {
      compiler = qstar.cli {"zig"},
    },
  },
}

qstar.config "native" {
  toolset = "//:host",
  lang = {
    zig = zig.options {
      target = "native",
      optimize = "Debug",
      macos_min_version = "",
    },
  },
}

qstar.staticlib "zig_core" {
  configs = {"//:native"},
  sources = {"src/zig_core.zig"},
}
```

Provider action은 `ZIG_GLOBAL_CACHE_DIR`와 `ZIG_LOCAL_CACHE_DIR`를 action-local cache로
설정한다. 실제 process에는 cache path가 전달되지만, action-log/replay에는
`NAME=<redacted>`로만 기록된다.

macOS에서 Zig object가 patch-level OS version으로 찍히고 C linker가 major.0 minimum으로
링크하면서 warning을 낼 수 있다. 이 경우에는 `target = "native"`와 `macos_min_version`을
같이 둔다. Provider는 read-only `qstar.host`를 보고 macOS host에서만 concrete Zig target을
만들며, macOS가 아닌 host에서는 `native`를 그대로 둔다.

```lua
zig = zig.options {
  target = "native",
  optimize = "Debug",
  macos_min_version = "11.0",
}
```

순수 Zig executable은 provider final action으로 `zig build-exe`가 직접 만든다. Zig package
graph나 `build.zig`를 해석하는 기능은 아니며, 필요한 link option은 `compile_options`에
명시한다. 자세한 예제와 Zig staticlib macOS consumer caveat는 `docs/zig-provider.md`에 둔다.

## Rust Provider Notes

표준 Rust provider는 real `rustc --emit=obj` object unit과 `rustc --crate-type
bin/staticlib/cdylib` provider final action을 지원한다. `crate_type` option은 object unit
lowering에서 `rustc --crate-type`으로 내려가며 기본값은 `lib`이다.

```lua
local rust = qstar.use_language("rust")

qstar.toolset "host" {
  tools = {
    c = { compiler = qstar.cli {"cc"} },
    archive = qstar.cli {"ar"},
    link = qstar.cli {"cc"},
    rust = rust.tools {
      compiler = qstar.cli {"rustc"},
    },
  },
}

qstar.config "native" {
  toolset = "//:host",
  lang = {
    rust = rust.options {
      edition = "2021",
      crate_type = "lib",
    },
  },
}

qstar.staticlib "rust_core" {
  configs = {"//:native"},
  sources = {"src/rust_core.rs"},
}
```

이 경로는 Rust provider final action으로 staticlib를 만든 뒤 다른 QStar target이 소비하는
용도에 맞춰져 있다. 순수 Rust executable도 `qstar.executable`에서 provider final action으로
빌드할 수 있다. Cargo workspace, build.rs, registry/lockfile resolution은 여전히 QStar core가
해석하지 않는다. 자세한 예제와 한계는 `docs/rust-provider.md`에 둔다.

기존 object artifact bridge도 계속 유효하다. 외부 compiler 호출을 더 세밀하게 제어해야 하면
`qstar.custom_target`으로 작성하고, 결과 object를 `qstar.output(path, {format = "object"})`로
표시한 뒤 consuming target의 `sources`에 넣는다.

Provider source unit lowering과 provider final artifact lowering은 Stella와 Ninja가 같은 backend contract로 실행한다.
Provider implementation은 `qstar.argv()`와 `ctx.tool`, `ctx.input`, `ctx.output`,
`ctx.cache`, `ctx.option`으로 action을 만든다. Source action은 `ctx.input("source")`와
`ctx.output("object")`를 사용하고, final action은 `ctx.input("sources")`,
`ctx.output("artifact")`, `ctx.kind()`를 사용한다. QStar는 그 결과의 `command`,
`env`, `inputs`, `outputs`, `depfile`을 Graph IR에 저장하고 action-log/replay,
response file, depfile-discovered input, Ninja emission에 같은 값을 사용한다.
`env`는 `"NAME=value"` string list이며, 실행에는 실제 값을 넘기지만 action-log/replay에는
`NAME=<redacted>`로만 기록한다. Provider가 build-local cache directory를 필요로 하면
`ctx.cache("name")`으로 package-relative cache path를 받아 env에 넣는다.

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
      macos_min_version = "",
      compile_options = {"-Ddemo"},
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

이 문법 중 provider activation, manifest validation, `provider.lua` sandbox loading,
`lang.zig` namespace gate, provider-defined option schema validation, raw provider source
classification, explicit provider helper source token, object output allocation, provider action
lowering, Stella/Ninja backend execution은 구현되어 있다.

## 관련 CLI

```sh
qstar --file qstar.lua dry-run //:app
qstar --file qstar.lua build //:app
qstar --file qstar.lua -G ninja build //:app
```

## 관련 diagnostic

- `activate a language provider with qstar.use_language(...) before listing this source`
- `qstar.output(..., {format = "object"})`
- `qstar: unsupported language provider api 'qstar.lang/999'; supported APIs: qstar.lang/1, qstar.lang/2`
- `qstar: unknown language namespace lang.zig`
- `qstar: unknown field lang.zig.<option>`
- `qstar: lang.zig.<option> has unsupported enum value '...'`
- `qstar: duplicate language provider namespace lang.zig`
- `qstar: circular language provider activation`
- `qstar: source '...' matches multiple provider source units (...)`
- `qstar: source '...' matches both a built-in source suffix and provider source unit ...`

## 관련 문서

- `wiki/reference/object-artifacts.md`
- `wiki/reference/custom-target.md`
