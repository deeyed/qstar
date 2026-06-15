# Toolsets

QStar는 특정 언어에 종속되지 않는 빌드시스템이다. `qstar.toolset`은 compiler, assembler, archive, link, response-file, external tool
policy를 명시적으로 선언하는 graph node다. QStar는 target triple, CPU, ABI, sysroot 같은
domain-specific 의미를 해석하지 않는다. 그런 값이 필요하면 `qstar.config`의
`compile_options`, `link_options`, `link_inputs`에 그대로 작성한다.

## 최소 예제

```lua
qstar.toolset "host" {
  tools = {
    c = qstar.cli {"cc"},
    cxx = qstar.cli {"c++"},
    asm = qstar.cli {"cc"},
    archive = qstar.cli {"ar"},
    link = qstar.cli {"cc"},
  },
  response_files = "auto",
  response_style = "posix",
  path_tools = {"python3"},
}
```

`tools` table의 role allowlist는 `c`, `cxx`, `asm`, `archive`, `link`뿐이다.
각 role은 shell string이 아니라 `qstar.cli { ... }` argv-vector로 작성한다.

## 전체 예제

Toolset은 compile/link option bundle이 아니다. 반복 option은 `qstar.config`에 둔다.

```lua
qstar.toolset "host" {
  tools = {
    c = qstar.cli {"cc"},
    cxx = qstar.cli {"c++"},
    asm = qstar.cli {"cc"},
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

## External Tools

`qstar.custom_target`과 `qstar.run_target`에서 실행할 package-local wrapper나 PATH tool은
`path_tools`로 허용한다.

```lua
qstar.toolset "host" {
  tools = {
    c = qstar.cli {"cc"},
    cxx = qstar.cli {"c++"},
    asm = qstar.cli {"cc"},
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
    c = qstar.cli {"cc"},
    cxx = qstar.cli {"c++"},
    asm = qstar.cli {"cc"},
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

Tool role은 `c`, `cxx`, `asm`, `archive`, `link`만 허용된다.

## 관련 diagnostic

- `qstar: unknown toolset field`
- `qstar: toolset '...' requires tools table`
- `qstar: unknown toolset tool role`
- `qstar: target '...' references unknown toolset`
- `external tool is not allowed by toolset policy`
