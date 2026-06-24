# Toolsets

QStar는 특정 언어에 종속되지 않는 빌드시스템이다. `qstar.toolset`은 compiler, assembler, archive, link, response-file, external tool
policy를 명시적으로 선언하는 graph node다. QStar는 target triple, CPU, ABI, sysroot 같은
domain-specific 의미를 해석하지 않는다. 그런 값이 필요하면 `qstar.config`의
`compile_options`, `link_options`, `link_inputs`에 그대로 작성한다.

현재 runtime의 direct core role은 `archive`, `link`다. Compiler role은 `c`, `cxx`,
`asm` 같은 provider namespace table 아래에 둔다. 외부 provider는
`qstar.use_language("zig")`로 활성화한 뒤
`tools.zig = zig.tools { compiler = qstar.cli {"zig"} }`처럼 provider가 자기 tool role을
정의하는 구조를 사용한다.

## 최소 예제

```lua
qstar.toolset "host" {
  tools = {
    c = { compiler = qstar.cli {"cc"} },
    cxx = { compiler = qstar.cli {"c++"} },
    asm = { compiler = qstar.cli {"cc"} },
    archive = qstar.cli {"ar"},
    link = qstar.cli {"cc"},
  },
  response_files = "auto",
  response_style = "posix",
  path_tools = {"python3"},
}
```

`tools.archive`와 `tools.link`는 shell string이 아니라 `qstar.cli { ... }` argv-vector로
작성한다. Compiler role은 `tools.c.compiler`처럼 provider namespace 아래에 둔다.

## 전체 예제

Toolset은 compile/link option bundle이 아니다. 반복 option은 `qstar.config`에 둔다.

```lua
qstar.toolset "host" {
  tools = {
    c = { compiler = qstar.cli {"cc"} },
    cxx = { compiler = qstar.cli {"c++"} },
    asm = { compiler = qstar.cli {"cc"} },
    archive = qstar.cli {"ar"},
    link = qstar.cli {"cc"},
  },
}

qstar.config "module_c" {
  toolset = "//:host",
  lang = {
    c = {
      public_include_dirs = {"include"},
      compile_options = {"-std=c23", "-Wall", "-Wextra"},
    },
  },
}

qstar.staticlib "core" {
  configs = {"//:module_c"},
  sources = {"src/core.c"},
}
```

Target이 `configs`를 통해 toolset을 받거나 target-local `toolset = "//:host"`를 직접
지정하면 Stella와 Ninja는 같은 role resolver를 사용한다.

## Cross Compile

Cross compile policy도 QStar built-in 의미론이 아니라 argv다. compiler driver가 target이나
SDK 경로를 알아야 한다면 tool role 또는 config option에 직접 쓴다.

```lua
qstar.toolset "cross_clang" {
  tools = {
    c = { compiler = qstar.cli {"clang", "--target=vendor-platform"} },
    cxx = { compiler = qstar.cli {"clang++", "--target=vendor-platform"} },
    asm = { compiler = qstar.cli {"clang", "--target=vendor-platform"} },
    archive = qstar.cli {"llvm-ar"},
    link = qstar.cli {"clang", "--target=vendor-platform"},
  },
}

qstar.config "cross_flags" {
  toolset = "//:cross_clang",
  lang = {
    c = {
      compile_options = {
        "--sysroot=vendor/sdk",
        "-resource-dir",
        "vendor/clang-resource",
      },
    },
  },
  link_options = {"--sysroot=vendor/sdk"},
}
```

## External Tools

`qstar.custom_target`, `qstar.transform`, `qstar.run_target`에서 실행할 package-local
wrapper나 PATH tool은 `path_tools`로 허용한다. Generated action이 `toolset`을 직접
지정하면 bare PATH command tool은 그 toolset의 `path_tools` 안에 있어야 한다.

```lua
qstar.toolset "host" {
  tools = {
    c = { compiler = qstar.cli {"cc"} },
    cxx = { compiler = qstar.cli {"c++"} },
    asm = { compiler = qstar.cli {"cc"} },
    archive = qstar.cli {"ar"},
    link = qstar.cli {"cc"},
  },
  path_tools = {"python3", "sh"},
}
```

절대 경로 tool은 기본적으로 거부된다. 꼭 필요한 경우에만
`allow_absolute_tools = true`를 사용한다.

## Response Files

```lua
qstar.toolset "rsp" {
  tools = {
    c = { compiler = qstar.cli {"cc"} },
    cxx = { compiler = qstar.cli {"c++"} },
    asm = { compiler = qstar.cli {"cc"} },
    archive = qstar.cli {"ar"},
    link = qstar.cli {"cc"},
  },
  response_files = "on",
  response_style = "posix",
}
```

`response_files`는 `auto`, `on`, `off`를 받는다. `response_style`은 host/tool family에 맞는
quoting style을 고르는 문자열이다. QStar는 style을 command semantics로 해석하지 않고 argv
materialization에만 사용한다.

## 관련 CLI

```sh
qstar --file qstar.lua doctor
qstar --file qstar.lua explain //:app
qstar --file qstar.lua list-targets --format json
```

`doctor`는 resolved tool role, PATH/package-local tool 발견 상태, response-file policy,
depfile behavior, writable build state를 보여준다.

## 실패 예제

```lua
qstar.toolset "bad" {
  tools = {
    cc = qstar.cli {"cc"},
  },
}
```

Direct tool role은 `archive`, `link`만 허용된다. Compiler tool은
`tools.c.compiler`처럼 built-in 또는 external provider namespace table 아래에 둔다.

## GLP Provider Tool Syntax

Provider activation 후 toolset은 provider namespace table을 직접 받을 수 있다.

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
```

이 문법에서 `zig`는 문자열 key이면서 `qstar.use_language("zig")`가 반환한 provider module
value와 authoring convention으로 연결된다. 내부 role은 `zig.compiler`처럼 저장되지만,
사용자는 provider helper를 통해 tool을 선언한다. 현재 runtime은 nested provider tool table을
role map으로 저장하고, provider source lowering은 이 role map을 Stella/Ninja 공통 action
template에서 해석한다.

기존 `tools.c = qstar.cli {...}`, `tools.cxx = qstar.cli {...}`,
`tools.asm = qstar.cli {...}` 직접 문법은 제거됐다. C/C++/ASM 자체는 사라지지 않고
built-in provider namespace로 `tools.c.compiler`, `tools.cxx.compiler`,
`tools.asm.compiler`를 사용한다. `lang.c`, `lang.cxx`, `lang.asm` beginner surface는 계속
제공한다.

## 관련 diagnostic

- `qstar: unknown toolset field`
- `qstar: toolset '...' requires tools table`
- `qstar: unknown toolset tool role`
- `qstar: unknown toolset provider namespace tools.zig`
- `qstar: target '...' references unknown toolset`
- `external tool is not allowed by toolset policy`
