# Reusable Configs

`qstar.config`는 여러 target이 공유하는 option bundle이다. Target, generated action,
stage처럼 label을 가지지만 source, dependency, command, output은 만들지 않는다.
Config는 target 선언 시점에 병합되므로 사용하는 target보다 먼저 평가되어야 한다.
보통 root `qstar.lua`에서 policy `.qst`를 먼저 `qstar.import_file(...)`로 읽고,
그 다음 `qstar.subdir(...)`로 leaf target을 읽는다.

## Basic Form

```lua
qstar.config "module_c" {
  toolset = "//:host",
  lang = {
    c = {
      public_include_dirs = {"include"},
      compile_options = {
        "-std=c23",
        "-Wall",
        "-Wextra",
      },
    },
  },
}

qstar.staticlib "core" {
  configs = {"//:module_c"},
  sources = {"src/core.c"},
  lang = {
    c = {
      compile_options = {"-DCORE_BUILD=1"},
    },
  },
}
```

## Cross-Fragment Use

`qstar.config`는 일반 `.qst` graph fragment에서 선언한다.

```lua
qstar.import_file("qstar/policies/common.qst")

qstar.staticlib "driver" {
  configs = {
    "//qstar/policies:module_c",
    "//qstar/policies:strict_warnings",
  },
  sources = {"drivers/driver.c"},
}
```

`.qsm` module은 helper table만 반환하므로 `qstar.config`를 선언할 수 없다.

## Merge Rule

- `configs` list는 target이 선언한 순서대로 적용된다.
- list field는 config 순서대로 append된다.
- target local list field는 config에서 온 list 뒤에 append된다.
- scalar field는 뒤의 config가 앞의 config를 override한다.
- target local scalar field가 최종 override한다.

`explain`, `dry-run`, `query`, `--dump-graph`, `list-targets --format json`은 target의
`configs` label과 merged option 결과를 함께 보여준다.

## Allowed Fields

Config는 target option만 담는다.

- `lang.c`, `lang.cxx`, `lang.asm`
- `libs`, `lib_dirs`
- `link.frameworks` for macOS-only framework names
- `link_options`, `link_inputs`
- `toolset`, `artifact_name`

다음 field는 config에서 금지된다.

- `sources`
- `deps`, `public_deps`, `private_deps`
- `visibility`
- `command`, `timeout`, `expect`
- generated output이나 stage file

언어별 option은 config에서도 target과 동일하게 반드시 `lang.*` 아래에 둔다.
