# QStar Examples

> Current note: Round 47 이후 새 authoring syntax는
> `docs/qstar-v0.2-authoring-spec.md`와 `../wiki/`를 정본으로 본다. 이
> 문서의 오래된 v0/v0.1 예시는 v0.2 hard cut에 맞춰 점진적으로 정리 중이다.

이 문서는 QStar 문법과 Cale module/package/header path model을 예시로 보여준다.
HCL 내용 자체는 QStar가 해석하지 않는다. 아래 `.hcl` 예시는 compiler/HCL
checker가 처리할 header 파일이 build graph에 어떻게 등록되는지만 보여준다.

## Small Hosted App

```txt
hello/
  Cale.toml
  qstar.lua

  src/
    app/
      main.cale
    util/
      format.cale
      parse.cale
```

```toml
# Cale.toml
[package]
name = "hello"
version = "0.1.0"

[secure]
profile = "c-compat"
```

```lua
-- qstar.lua
qstar.executable "hello" {
    lang = {
        cale = {
            modules = qstar.modules {
                root = "src",
                include = {
                    "app",
                    "util",
                },
            },
        },
    },
    toolchain = "host",
    stdlib = "system",
}
```

```c
// src/app/main.cale
import util;

int main(void) {
    return util::run();
}
```

## Header Facade Paths

```txt
engine/
  Cale.toml
  qstar.lua

  include/
    engine/
      engine.hcl
      render.hcl
      asset.hcl

  src/
    render/
      render.qst
      renderer.cale
      pipeline.cale
    asset/
      asset.qst
      loader.cale
```

```c
// include/engine/engine.hcl
#include <engine/render.hcl>
#include <engine/asset.hcl>

export int engine_init(void);
export void engine_shutdown(void);
```

```c
// include/engine/render.hcl
struct InternalRenderLayout {
    int hidden;
};

export struct Renderer {
    int id;
};

export void renderer_draw(Renderer &renderer);
```

External user:

```c
#include <engine/engine.hcl>

int main(void) {
    engine_init();
    engine_shutdown();
    return 0;
}
```

Internal implementation:

```c
// src/render/renderer.cale
import asset;

export void renderer_draw(Renderer &renderer) {
    ...
}
```

## Distributed QStar Fragments

```txt
engine/
  Cale.toml
  qstar.lua

  src/
    render/
      render.qst
      renderer.cale
    asset/
      asset.qst
      loader.cale
    platform/
      platform.qst
      darwin.cale
      linux.cale
      windows.cale
```

```lua
-- qstar.lua
qstar.subdir("src/render")
qstar.subdir("src/asset")
qstar.subdir("src/platform")

qstar.staticlib "engine" {
    deps = {
        "//src/render:render",
        "//src/asset:asset",
        "//src/platform:platform",
    },
    lang = {
        cale = {
            public_headers = {
                "include/engine/engine.hcl",
            },
            public_include_dirs = {
                "include",
            },
        },
    },
    toolchain = "host",
    stdlib = "system",
}
```

```lua
-- src/render/render.qst
qstar.target "render" {
    kind = "modulelib",
    lang = {
        cale = {
            modules = qstar.modules {
                root = "src",
                include = {
                    "render",
                },
            },
        },
    },
    deps = {
        "//src/platform:platform",
    },
}
```

## Platform Source Selection

```lua
-- src/platform/platform.qst
qstar.target "platform" {
    kind = "modulelib",
    sources = qstar.select {
        [qstar.os.macos] = {"src/platform/darwin.cale"},
        [qstar.os.linux] = {"src/platform/linux.cale"},
        [qstar.os.windows] = {"src/platform/windows.cale"},
        default = qstar.incompatible("unsupported platform"),
    },
}
```

이 방식은 source-level `#if defined(...)`를 줄이고, target별 구현 파일을 build graph에서 분리한다.

## C-Only Local Build Preview

Round 14/15의 `qstar build`는 Cale compiler와 연결되기 전의 제한 executor다. 작은
C-only fixture는 다음처럼 직접 실행할 수 있다.

```txt
capp/
  Cale.toml
  qstar.lua
  src/main.c
```

```toml
profile = "dev"

[profile.dev]
target = "host"
toolchain = "host"
stdlib = "system"
```

```lua
qstar.executable "app" {
    sources = qstar.files {"src/*.c"},
}
```

```txt
qstar --file qstar.lua dry-run //:app
qstar --file qstar.lua build //:app
qstar --file qstar.lua why-rebuild //:app
```

`dry-run`은 `.qstar/out` 경로와 argv를 보여주지만 실행하지 않는다. `build`는
package-local generated action, C compile, archive/link만 실행하고 log를
`.qstar/logs`에 저장한다. 반복 build에서는 `.qstar/state/actions.json`의 action
key를 보고 unchanged action을 skip하며, `compile_commands.json`을 package root에
쓴다.

## Generated Source/Header Paths

```lua
qstar.custom_target "version_header" {
    inputs = {
        "Cale.toml",
    },
    outputs = {
        qstar.output("generated/version.c"),
        qstar.output("generated/version.hcl"),
    },
    command = qstar.cli {
        "gen-version",
        "--in", qstar.input(0),
        "--out-c", qstar.output(0),
        "--out", qstar.output(1),
    },
}

qstar.executable "app" {
    sources = {
        "src/app/main.cale",
        qstar.output("generated/version.c"),
    },
    lang = {
        cale = {
            modules = qstar.modules {
                root = "src",
                include = {"app"},
            },
            public_headers = qstar.join {
                {"include/app/app.hcl"},
                {qstar.output("generated/version.hcl")},
            },
            public_include_dirs = {
                "include",
                "generated",
            },
        },
    },
}
```

Round 48의 generated API는 `qstar.cli` argv-vector와 command placeholder를 사용한다.
`qstar.input(0)`은 첫 번째 input, `qstar.output(0)`은 첫 번째 output이다.
`qstar.generated`나 `qstar.generated_dir` 같은 label-based convenience API는 future
work다. `qstar dry-run //:app`으로 executor-shaped step order를 확인하고, `qstar build`
로 package-local generated action을 실행할 수 있다.

직접 실행해 볼 수 있는 수동 fixture는 repository 안의
`qstar/tests/manual/hello/qstar.lua`에 둔다.

Round 9부터는 같은 fixture에 대해 `qstar check //:app`을 실행해 source/header/input
파일이 package root 아래 실제로 존재하는지 확인할 수 있다.

Round 10/11부터는 직접 만든 프로젝트에서 다음 흐름을 함께 확인한다.

```txt
qstar --file qstar.lua list-targets
qstar --file qstar.lua query //:app
qstar --file qstar.lua doctor
qstar --file qstar.lua check //:app
qstar --file qstar.lua explain //:app
qstar --file qstar.lua dry-run //:app
```

## Workspace / Monorepo

```txt
workspace/
  Cale.toml
  Cale.lock
  qstar.lua

  packages/
    core/
      Cale.toml
      qstar.lua
      include/
        core/
          core.hcl
      src/
        mem/
          mem.qst
          arena.cale

    engine/
      Cale.toml
      qstar.lua
      include/
        engine/
          engine.hcl
      src/
        render/
          render.qst
          renderer.cale

  tools/
    shaderc/
      Cale.toml
      qstar.lua
      src/
        app/
          main.cale
```

```toml
# workspace/Cale.toml
[workspace]
members = [
  "packages/core",
  "packages/engine",
  "tools/shaderc",
]
```

```toml
# packages/engine/Cale.toml
[package]
name = "engine"
version = "0.1.0"

[dependencies]
core = { path = "../core" }
```

```c
// packages/engine/src/render/renderer.cale
import core::mem;
```
