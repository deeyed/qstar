# Zig Provider

QStar의 표준 Zig provider는 `zig build-obj` object unit lowering과 provider-owned final
artifact lowering을 함께 지원한다. 순수 Zig `executable`, `staticlib`, `sharedlib` target은
provider final action을 통해 `zig build-exe` 또는 `zig build-lib`가 직접 최종 산출물을 만든다.
Provider가 Zig package manager나 `build.zig` graph를 해석하지는 않는다.

## 지원 범위

- `qstar.use_language("zig")`로 표준 provider를 활성화한다.
- `tools = { zig = zig.tools { compiler = qstar.cli {"zig"} } }`로 Zig compiler를 지정한다.
- `lang.zig = zig.options { ... }`로 Zig compile/final option을 지정한다.
- 활성화된 provider의 `.zig` suffix는 raw string source classification에 참여한다.
- `zig.object("path.zig", {...})` helper는 source-local option이 필요할 때 사용한다.
- Provider action은 `ZIG_GLOBAL_CACHE_DIR`와 `ZIG_LOCAL_CACHE_DIR`를 action-local cache로
  지정한다. 이 값은 process에는 전달되지만 action-log/replay에는 redacted form으로만 남는다.
- 모든 compile source가 Zig provider source이고 target이 native link input/deps를 갖지 않으면
  provider final action이 자동 선택된다.

## Zig Options

```lua
zig = zig.options {
  target = "native",
  optimize = "Debug",
  macos_min_version = "",
  compile_options = {},
}
```

`target`은 Zig의 `-target` 문자열이다. 기본값 `native`는 `-target` argv를 만들지 않는다.
`optimize`는 `-O` 값이며 `Debug`, `ReleaseSafe`, `ReleaseFast`, `ReleaseSmall`을 받는다.
`compile_options`는 마지막에 그대로 추가된다.

`macos_min_version`은 macOS minimum target을 지정하기 위한 편의 option이다.
`target = "native"`와 함께 쓰면 provider가 read-only `qstar.host`를 보고 macOS host에서만
`aarch64-macos.11.0` 또는 `x86_64-macos.11.0` 같은 Zig target 문자열을 만든다. macOS가 아닌
host에서는 `native` 그대로 둔다. `target = "aarch64-macos"`처럼 macOS target base를 직접
명시한 경우에도 `.11.0`을 붙인다.

```lua
qstar.config "native" {
  lang = {
    zig = zig.options {
      target = "native",
      optimize = "Debug",
      macos_min_version = "11.0",
    },
  },
}
```

이 pattern은 macOS에서 Zig object/final artifact가 현재 patch OS version으로 찍히고 C linker는 major.0
minimum으로 링크하면서 생기는 `built for newer 'macOS' version` warning을 피하기 위한
권장 경로다. 더 세밀하게 제어하고 싶으면 `target = "aarch64-macos.13.0"`처럼 완성된 Zig
target 문자열을 직접 쓰면 된다.

## Staticlib Artifact Example

Zig provider final action으로 static library를 만드는 예:

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
    },
  },
}

qstar.staticlib "zig_core" {
  configs = {"//:native"},
  sources = {"src/zig_core.zig"},
}
```

`src/zig_core.zig`는 C ABI symbol을 노출해야 한다.

```zig
export fn zig_value() i32 {
    return 88;
}
```

이 경로는 `tests/corpus/real-glp/zig-static-consumer`에서 실제 `zig build-lib -static`으로
artifact 생성을 검증한다. macOS Zig 0.16.0에서는 `zig build-lib -static` 산출물을 Apple C
linker가 바로 소비할 때 archive alignment/index 후처리가 필요할 수 있다. 따라서 현재 real
corpus는 Zig staticlib artifact 생성과 action-log를 검증하고, C consumer 링크는 Rust staticlib
fixture에서 검증한다.

## Executable Example

순수 Zig executable은 C linker를 우회하고 provider final action으로 빌드된다.

```lua
qstar.executable "app" {
  configs = {"//:native"},
  sources = {"src/main.zig"},
}
```

```zig
extern fn puts([*:0]const u8) c_int;

pub fn main() void {
    _ = puts("zig-exe-ok");
}
```

이 fixture는 `compile_options = {"-lc"}`로 libc link를 명시한다. 경로는
`tests/corpus/real-glp/zig-executable`에서 Stella/Ninja 양쪽으로 검증한다. 다만 QStar가 Zig
`build.zig`, package graph, Zig std executable link policy를 대신 해석하는 것은 아니다.

## Object Helper Example

Target 전체 default와 다른 Zig source-local option이 필요하면 helper를 쓴다.

```lua
qstar.staticlib "core" {
  configs = {"//:native"},
  sources = {
    zig.object("src/special.zig", {
      optimize = "ReleaseFast",
      compile_options = {"-fPIC"},
    }),
  },
}
```
