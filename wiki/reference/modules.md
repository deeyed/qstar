# Imports and Modules

QStar는 Lua `require`를 열지 않는다. 대신 DSL-native import를 두 개로 분리한다.

```lua
qstar.import_file("qstar/policies/common.qst")
local policy = qstar.import_module("qstar/modules/policy")
```

## `qstar.import_file`

`qstar.import_file("path.qst")`는 package-root 기준 `.qst` file만 읽는다.

- `.qst` file은 target, config, toolset, group, stage 같은 graph declaration을 만들 수 있다.
- 같은 file을 한 번 더 import하면 duplicate import error가 난다.
- circular import는 error다.
- `.qsm`이나 `qstar.lua`는 받을 수 없다.

## `qstar.import_module`

`qstar.import_module("folder/path")`는 folder path만 받는다. File path를 직접 넘기지 않는다.

```lua
local paths = qstar.import_module("qstar/modules/paths")
```

위 호출은 다음 file을 읽는다.

```txt
qstar/modules/paths/paths.qsm
```

`.qsm` file은 반드시 table을 반환한다.

```lua
local M = {}

function M.common_c()
  return {
    public_include_dirs = {"include"},
    compile_options = {"-Wall", "-Wextra"},
  }
end

return M
```

`.qsm` 안에서는 graph declaration을 할 수 없다. 즉 `qstar.project`, `qstar.toolset`,
`qstar.config`, target rule, `qstar.custom_target`, `qstar.stage`, `qstar.target_family`, `qstar.subdir`,
`qstar.import_file`은 금지된다. Helper 함수, 상수, table literal, `qstar.import_module`
을 통한 다른 helper module import는 사용할 수 있다.

`.qsm` module은 policy나 target을 직접 만들지 않고 값을 만든다. Path는 `qstar.join`,
list는 `qstar.append`, option table은 `qstar.copy`, `qstar.merge`, `qstar.extend`로 조립한다.
Makefile처럼 문자열 안의 `$VAR`를 확장하는 기능은 없다.

## GLP와 module의 경계

Generic Language Provider(GLP)는 일반 helper module과 다르다. 일반 `.qsm`은 값을 반환할
뿐 build graph semantics를 바꾸지 않는다. 반면 language provider는 source classification,
tool role, `lang.<namespace>` option schema, backend lowering에 영향을 준다.

따라서 provider는 `qstar.import_module(...)`로 조용히 등록하지 않는다.
`qstar.use_language(...)`가 provider activation의 명시적 entrypoint다. 예를 들어
`qstar.use_language("zig")`는 `qstar/languages/zig/zig.qsm` manifest를 읽고, 그 provider가
등록한 namespace만 `lang.zig` 같은 language option table에서 허용한다.
Provider manifest는 `qstar.language_provider { api = "qstar.lang/1", ... }`를 반환해야 하고,
`implementation = "provider.lua"` 파일은 제한된 provider sandbox에서 따로 로드된다.

권장 provider 배치:

```txt
qstar/
  languages/
    zig/
      zig.qsm
      provider.lua
  modules/
    paths/
      paths.qsm
```

`qstar/modules`는 helper module 공간이고, `qstar/languages`는 provider package 공간이다.
`qstar/modules/language/zig.lua`처럼 기존 module 규칙을 우회하는 flat Lua file 특별취급은
하지 않는다.

```lua
local M = {}
local prefix = "vendor/include"

function M.vendor_include()
  return qstar.join(prefix, "public")
end

function M.common_c()
  return qstar.merge({
    public_include_dirs = {"include"},
    compile_options = {"-std=c23", "-Wall"},
  }, {
    system_include_dirs = {M.vendor_include()},
    compile_options = {"-Wextra"},
  })
end

return M
```

Host별 authoring 분기도 `.qsm` helper와 일반 Lua `if`로 표현한다. `qstar.host` namespace는
read-only이며, QStar grammar에 OS/arch condition object를 만들지 않는다.

```lua
local M = {}

function M.platform_sources()
  if qstar.host.os == "macos" then
    return {"src/platform/darwin.c"}
  elseif qstar.host.os == "linux" then
    return {"src/platform/linux.c"}
  end
  return {"src/platform/portable.c"}
end

return M
```

```lua
local platform = qstar.import_module("qstar/modules/platform")

qstar.staticlib "platform" {
  sources = platform.platform_sources(),
}
```

대표 diagnostic:

```txt
qstar: import_module expects a folder path, not file 'qstar/modules/paths/paths.qsm'; use qstar.import_module("qstar/modules/paths")
qstar: import_module 'qstar/modules/missing' not found; expected module entry 'qstar/modules/missing/missing.qsm'
qstar: qstar.config is forbidden inside .qsm module; modules must return a helper table
qstar: circular import chain: qstar.lua -> qstar/modules/a/a.qsm -> qstar/modules/b/b.qsm -> qstar/modules/a/a.qsm
qstar: duplicate language provider 'qstar/languages/zig/zig.qsm'
qstar: circular language provider activation: qstar.lua -> qstar/languages/loop/loop.qsm -> qstar/languages/loop/loop.qsm
qstar: qstar.use_language is forbidden inside ordinary .qsm module
```

LSP definition navigation은 import 문자열도 해석한다. `qstar.import_file("foo/bar.qst")`
위에서 definition을 요청하면 해당 `.qst`로 이동하고,
`qstar.import_module("qstar/modules/paths")` 위에서는
`qstar/modules/paths/paths.qsm`으로 이동한다.

## 권장 배치

```txt
qstar.lua
qstar/
  policies/
    common.qst
    warnings.qst
  modules/
    paths/
      paths.qsm
lib/
  core/
    core.qst
```

Root `qstar.lua`는 project/toolset/import/subdir orchestration을 담당한다.
`qstar/policies/*.qst`는 graph policy를 선언한다.
`qstar/modules/<name>/<name>.qsm`은 target을 만들지 않는 helper module을 제공한다.
폴더 이름과 module entry 파일 이름을 일치시키면 LSP navigation, formatter, AI authoring이
같은 규칙을 공유할 수 있다.
