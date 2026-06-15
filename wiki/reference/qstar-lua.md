# QStar Lua

QStar는 C/C++/ASM을 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다. Authoring
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
`QSTAR_PACKAGE_ROOT`, `QSTAR_PROJECT_ROOT`,
`qstar.version`, `qstar.host.os`, `qstar.host.arch`, `qstar.project.root`다.
`qstar.host`는 read-only namespace다. Host별 분기는 별도 condition DSL이 아니라 일반 Lua
`if qstar.host.os == "macos" then ... end` 형태로 작성한다.

## Graph entrypoints

- `qstar.project`: project metadata, `build_dir`, `generated_dir`, compile database policy.
- `qstar.toolset`: explicit compiler/archive/link/response-file/external tool role bundle.
- `qstar.config`: reusable option bundle used through target `configs`.
- `qstar.executable`, `qstar.staticlib`, `qstar.sharedlib`, `qstar.test`: artifact targets.
- `qstar.custom_target`, `qstar.configure_file`: generated outputs.
- `qstar.run_target`: external smoke/run action.
- `qstar.group`: deps-only aggregate with no command, output, install surface, or artifact.
- `qstar.stage`: copy-only package/stage tree.
- `qstar.target_family`: shared-source lint grouping.
- `qstar.subdir`, `qstar.import_file`, `qstar.import_module`: explicit graph/module loading.

`qstar.sharedlib`는 macOS host policy에서는 `.dylib`, Linux host policy에서는 `.so`를
생성한다. sharedlib dependency를 link하는 artifact target은 build-tree 실행을 위해
macOS `@loader_path`, Linux `$ORIGIN` 기반 rpath를 자동으로 받는다. Windows
runtime `.dll`, import `.lib`, PDB/debug artifact 정책은 deferred diagnostic이다.

`qstar.target_file("//:group")`은 error다. Group은 dependency closure를 묶는 label일 뿐
artifact-producing target이 아니다.

Artifact target과 config는 `link_options`와 `link_inputs`를 분리한다. `link_options`는
link argv에 그대로 추가되고, `link_inputs`는 package-relative file이나
`qstar.target_file(...)` artifact를 link action rebuild input으로만 추적한다.
macOS framework link는 generic top-level field가 아니라 macOS branch 안의
`link = { frameworks = {...} }`로만 작성한다.

## Builtin authoring helpers

QStar는 Makefile식 `$VAR` 문자열 치환을 하지 않는다. 반복되는 path와 option은 Lua `local`
변수, local function, `.qsm` helper module, 그리고 아래 builtin helper로 조립한다.

```lua
local vendor = "third_party/acme"
local vendor_include = qstar.join(vendor, "include")
local strict = qstar.append({"-Wall"}, "-Wextra", "-Werror")
```

- `qstar.join("a", "b", "c")`는 slash로 연결한 path string을 반환한다.
- `qstar.join {list_a, list_b, item}`은 기존 authoring 호환을 위해 list를 한 단계 flatten한다.
- `qstar.copy(table)`은 nested table을 deep copy한다.
- `qstar.append(list, ...)`는 원본 list를 바꾸지 않고 뒤에 값이나 list를 붙인 새 list를 반환한다.
- `qstar.merge(...)`는 plain table을 새 table로 deep merge한다. 같은 list field는 append된다.
- `qstar.extend(base, ...)`는 `base` table을 deep merge로 갱신하고 다시 반환한다.
- `qstar.status("...")`는 사용자 정의 build step description을 나타내는 validated helper를 반환한다.

`.qsm` module에서는 target을 선언하지 말고 이런 helper로 path, option table, config skeleton,
status description을 반환한다.

```lua
local M = {}

function M.generating(path)
  return qstar.status("Generating " .. path)
end

return M
```

## Explicit imports

```lua
qstar.import_file("qstar/policies/common.qst")
local paths = qstar.import_module("qstar/modules/paths")
```

`qstar.import_file`은 package-root 기준 `.qst` fragment만 읽는다. 해당 file은 graph
declaration을 포함할 수 있고 once-only로 평가된다.

`qstar.import_module`은 folder path만 받는다. `qstar.import_module("qstar/modules/paths")`는
`qstar/modules/paths/paths.qsm`을 읽고, module은 반드시 table을 반환해야 한다.
`.qsm` 안에서는 target/toolset/project/subdir/import_file 같은 graph declaration이 금지된다.

```lua
local M = {}
local base_c = {
  public_include_dirs = {qstar.join("include")},
  compile_options = {"-Wall"},
}

function M.common_c()
  return qstar.merge(base_c, {
    compile_options = {"-Wextra"},
  })
end

return M
```

## Reusable configs

Lua local helper는 같은 파일 안에서 table을 만들 때 유용하고, `qstar.config`는 여러
fragment가 공유하는 graph-level option bundle을 만들 때 쓴다.

```lua
qstar.config "common_c" {
  lang = {
    c = {
      public_include_dirs = {"include"},
      compile_options = {"-Wall", "-Wextra"},
    },
  },
}

qstar.staticlib "core" {
  configs = {"//:common_c"},
  sources = {"src/core.c"},
  lang = {
    c = {
      compile_options = {"-DCORE_BUILD=1"},
    },
  },
}
```

`configs`는 label list다. `qstar.import_file("qstar/policies/common.qst")`로 읽은
fragment의 config는 `//qstar/policies:common_c`처럼 참조한다.

Merge rule:

- list field는 `configs` 순서대로 append된다.
- target local list field는 마지막에 append된다.
- scalar field는 뒤의 config가 앞의 config를 override하고 target local scalar가 최종 override한다.
- config는 source, deps, command, output을 만들 수 없다.
- `.qsm` module 안에서는 `qstar.config`도 다른 graph declaration처럼 금지된다.
- config는 사용하는 target보다 먼저 평가되어야 한다.

## Project output policy

`qstar.project`의 `generated_dir`는 generated action output이 놓일 package-relative
root를 정한다. 생략하면 기존 surface와 같이 `generated`를 사용한다.

```lua
qstar.project {
  root = ".",
  build_dir = "build/qstar",
  generated_dir = "build/qstar/generated",
  compile_commands = "build",
}

qstar.configure_file "cfg" {
  output = qstar.output("build/qstar/generated/config.h"),
  defines = {"APP_VALUE=42"},
  description = qstar.status("Configuring generated config.h"),
}
```

`qstar.custom_target.outputs`와 `qstar.configure_file.output`은 effective
`generated_dir` 아래에 있어야 한다. Target `sources`, `public_headers`,
`private_headers`에서 그 directory 아래 path를 참조하면 반드시 해당 path를 만드는
generated action owner가 있어야 한다.

## Build status descriptions

사용자 정의 action은 CMake-style progress line에 표시할 description을 지정할 수 있다.

```lua
qstar.custom_target "version_header" {
  outputs = {qstar.output("build/qstar/generated/version.h")},
  command = qstar.cli {"tools/gen-version", qstar.output(0)},
  description = qstar.status("Generating version.h"),
}

qstar.run_target "smoke" {
  deps = {"//:app"},
  command = qstar.cli {qstar.target_file("//:app")},
  expect = {
    contains = "OK",
  },
  description = qstar.status("Running smoke test"),
}
```

`description`은 `qstar.custom_target`, `qstar.configure_file`, `qstar.run_target`,
`qstar.stage`에서 지원된다. Raw string은 받지 않고, 빈 문자열, newline, 240 byte 초과
문자열은 diagnostic으로 거절한다.
같은 문자열은 Stella progress line, Ninja `description = ...`,
`qstar action-log`, `qstar replay`, `qstar last-failure`의 `description=`
metadata에도 사용된다.

## 실패 예제

```lua
leaked_global = 1
io.open("secret.txt")
```

Global assignment와 filesystem/process/network/dynamic loading API는 금지된다.
Lua `require`도 금지된다. Helper module은 `qstar.import_module`로만 불러온다.
문자열 안의 `$SRC`, `$TRIPLE` 같은 token도 자동 확장되지 않는다.

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
- `qstar: duplicate import`
- `qstar: circular import includes`
- `qstar: module '...' must return a table`
