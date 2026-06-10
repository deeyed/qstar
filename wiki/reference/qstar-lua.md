# QStar Lua

QStar는 C/C++/Cale을 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다. Authoring
language는 sandboxed Lua 5.4 subset이며, deterministic graph evaluation을 위해 일부 API는
금지된다.

## 최소 예제

```lua
local function common_c()
  return {
    public_include_dirs = {"include"},
    compile_options = {"-Wall"},
  }
end
```

## 전체 예제

```lua
local function common_c()
  local opts = {}
  for _, flag in ipairs({"-Wall", "-Wextra"}) do
    table.insert(opts, flag)
  end
  table.insert(opts, "-DQSTAR_VERSION=" .. QSTAR_VERSION)
  table.insert(opts, "-DQSTAR_TARGET=" .. QSTAR_TARGET)
  return {
    public_include_dirs = {"include"},
    compile_options = opts,
  }
end

qstar.staticlib "core" {
  sources = {"src/core.c"},
  lang = {
    c = common_c(),
  },
}
```

공식 상수는 `QSTAR_VERSION`, `QSTAR_HOST_OS`, `QSTAR_HOST_ARCH`,
`QSTAR_PACKAGE_ROOT`, `QSTAR_PROJECT_ROOT`, `QSTAR_PROFILE`, `QSTAR_TARGET`,
`qstar.version`, `qstar.host.os`, `qstar.host.arch`, `qstar.project.root`다.

## 실패 예제

```lua
leaked_global = 1
io.open("secret.txt")
```

Global assignment와 filesystem/process/network/dynamic loading API는 금지된다.

## 관련 CLI

```sh
qstar --file qstar.lua check
qstar --file qstar.lua lint --format json
qstar --file qstar.lua --dump-graph
```

## 관련 diagnostic

- `qstar: global assignment is not allowed`
- `qstar: forbidden Lua API 'io.open'`
- `qstar: forbidden Lua API 'require'`
