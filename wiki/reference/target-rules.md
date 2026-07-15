# Target Rules

QStar는 C/C++와 외부 object artifact bridge를 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다. 이 장은
target/rule API의 정본 이름을 정리한다.

## 최소 예제

```lua
qstar.executable "app" {
  sources = {"src/main.c"},
}
```

## 전체 예제

```lua
qstar.executable "app" { sources = {"src/main.c"} }
qstar.config "common_c" { lang = { c = { compile_options = {"-Wall"} } } }
qstar.staticlib "core" { sources = {"src/core.c"} }
qstar.objectlib "core_objects" { sources = {"src/core_extra.c"} }
qstar.interface "api_contract" { compile_usage = { options = {"-DAPI=1"} } }
qstar.imported "vendor" {
  artifacts = {
    default = {
      {id = "archive", role = "link", path = "vendor/libvendor.a", primary = true},
    },
  },
}
qstar.tool "generator" { path = "tools/generator" }
qstar.sharedlib "plugin" { sources = {"src/plugin.c"} }
qstar.test "unit" { sources = {"tests/unit.c"} }
qstar.custom_target "generated" { outputs = {qstar.output("generated/value.c")} }
qstar.run_target "smoke" { command = qstar.cli {"tools/smoke.sh"} }
qstar.group "module_parts" { deps = {"//lib:core", "//plugins:main"} }
qstar.configure_file "cfg" { output = qstar.output("generated/config.h") }
qstar.stage "bundle" { root = "stage/bundle", files = {} }
qstar.target_family "module_variants" { variants = {"fast", "portable"} }
```

`sharedlib`는 macOS platform context에서 `lib<name>.dylib`, Linux platform context에서
`lib<name>.so`를 생성한다. Stella와 Ninja backend 모두 C/C++/ASM source를 compile하고
`link-shared` final action을 실행한다. sharedlib에 의존하는 executable/test/sharedlib는
build-tree 실행을 위해 macOS에서는 `@loader_path`, Linux에서는 `$ORIGIN` 기반 rpath를
자동으로 받는다. Windows platform context에서는 Q223부터 Graph IR가 runtime `.dll`과
import `.lib` artifact map을 모델링한다. `qstar.target_file("//:plugin")`은 primary
runtime `.dll`을 가리키고,
`qstar.target_file("//:plugin", { artifact = "import_lib" })`은 import `.lib`를
가리킨다. Q224부터 Stella/Ninja는 Windows `link-shared` action에서 runtime `.dll`과
import `.lib`를 함께 만들고, dependent artifact target은 import `.lib`를 link input으로
사용한다. PDB/debug ownership과 일반 Windows runtime search path 정책은 아직 deferred다.

Artifact target은 `configs = {"//:common_c"}`로 reusable option bundle을 참조할 수
있다. Config merge 규칙은 [Reusable Configs](configs.md)에 둔다.

`qstar.objectlib`는 archive/link final artifact를 만들지 않는 object collection target이다.
`compile_context = "own"`은 source를 objectlib 자신의 configs/lang/toolset으로 한 번
compile한다. `compile_context = "consumer"`는 source ownership을 objectlib에 두고,
각 consuming executable/staticlib/sharedlib/test의 effective configs/lang/toolset 안에서
per-consumer object로 compile한다. Executable/staticlib/sharedlib/test는
`objects = {"//:core_objects"}`로 objectlib를 소비한다. `qstar.target_file("//:core_objects")`
는 objectlib에 primary artifact가 없기 때문에 error다.
Objectlib source는 built-in C/C++/ASM source, activated GLP raw source string,
`"own"` context의 explicit provider source token, generated object output을 받을 수 있다.
Explicit provider source token은 concrete action/output을 담기 때문에
`compile_context = "consumer"`에서는 raw provider source string이나 `"own"` context를 쓴다.

`qstar.group`은 output이 없는 deps-only aggregate target이다. `build //:module_parts`는
group의 dependency closure만 빌드하고 archive/link/run action을 만들지 않는다.
`qstar.target_file("//:module_parts")`, install, artifact output은 명확한 diagnostic으로
거부된다. Link interface를 전파하지 않으므로 executable이나 library가 실제 link input을
원하면 staticlib/sharedlib target을 직접 deps에 둔다.

`qstar.target_family`는 target rule이 아니라 lint grouping primitive다. Variant target이
같은 source를 의도적으로 공유할 때만 `allow_shared_sources = true`로 duplicate source
warning을 family 안에서 억제한다.

```lua
qstar.target_family "module_variants" {
  variants = {"fast", "portable"},
  allow_shared_sources = true,
}

qstar.staticlib "module_fast" {
  sources = {"src/start.c"},
}

qstar.staticlib "module_portable" {
  sources = {"src/start.c"},
}
```

`qstar.interface`, `qstar.imported`, `qstar.tool`은 typed dependency target이다.
Interface는 artifact 없이 compile/link usage를 전파한다. Imported는 current platform의
package-local artifact map을 고르고 link consumer에는 `role = "link"` artifact path를
전달한다. Tool은 executable path를 `qstar.tool_file(label)`로 generated command에
연결한다. QStar-built executable/test도 `qstar.tool_file` target이 될 수 있다.
Imported target은 artifact suffix나 `artifact_kind`에서 compiler/linker flag를 추론하지
않는다. 전체 계약은 [Typed Dependencies](typed-dependencies.md)에 있다.

## 실패 예제

```lua
qstar.executable "bad" {
  include_dirs = {"include"},
}
```

Top-level language shortcut은 target field가 아니다. Include path와 compile option은
`lang.c`, `lang.cxx`, `lang.asm` namespace 아래에 둔다.

## 관련 CLI

```sh
qstar --file qstar.lua list-targets --format json
qstar --file qstar.lua query //:app
qstar --file qstar.lua explain //:app
```

## 관련 diagnostic

- `unknown target field 'include_dirs'`
