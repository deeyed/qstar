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
qstar.sharedlib "plugin" { sources = {"src/plugin.c"} }
qstar.test "unit" { sources = {"tests/unit.c"} }
qstar.custom_target "generated" { outputs = {qstar.output("generated/value.c")} }
qstar.run_target "smoke" { command = qstar.cli {"tools/smoke.sh"} }
qstar.group "module_parts" { deps = {"//lib:core", "//plugins:main"} }
qstar.configure_file "cfg" { output = qstar.output("generated/config.h") }
qstar.stage "bundle" { root = "stage/bundle", files = {} }
qstar.target_family "module_variants" { variants = {"fast", "portable"} }
```

`sharedlib`는 macOS host policy에서 `lib<name>.dylib`, Linux host policy에서
`lib<name>.so`를 생성한다. Stella와 Ninja backend 모두 C/C++/ASM source를 compile하고
`link-shared` final action을 실행한다. sharedlib에 의존하는 executable/test/sharedlib는
build-tree 실행을 위해 macOS에서는 `@loader_path`, Linux에서는 `$ORIGIN` 기반 rpath를
자동으로 받는다. Windows runtime `.dll`, import `.lib`, PDB/debug artifact 정책은 아직
deferred이며 Windows host에서는 `docs/windows-artifact-policy.md`를 가리키는
명확한 diagnostic을 낸다.

Artifact target은 `configs = {"//:common_c"}`로 reusable option bundle을 참조할 수
있다. Config merge 규칙은 [Reusable Configs](configs.md)에 둔다.

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

## 실패 예제

```lua
qstar.exe "app" {
  sources = {"src/main.c"},
}
```

Removed API alias는 compatibility layer가 아니라 stable diagnostic이다.

## 관련 CLI

```sh
qstar --file qstar.lua list-targets --format json
qstar --file qstar.lua query //:app
qstar --file qstar.lua explain //:app
```

## 관련 diagnostic

- `qstar.exe removed; use qstar.executable`
- `qstar.genrule removed; use qstar.custom_target`
- `qstar.config_header removed; use qstar.configure_file`
