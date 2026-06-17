# Rust Provider

QStar의 표준 Rust provider는 `rustc --emit=obj` object unit lowering과 provider-owned
final artifact lowering을 함께 지원한다. 순수 Rust `executable`, `staticlib`, `sharedlib`
target은 provider final action을 통해 `rustc --crate-type bin/staticlib/cdylib`가 직접
최종 산출물을 만든다. C/C++과 섞인 target이나 object bridge flow는 기존 QStar
archive/link graph에서 계속 소비된다.

## 지원 범위

- `qstar.use_language("rust")`로 표준 provider를 활성화한다.
- `tools = { rust = rust.tools { compiler = qstar.cli {"rustc"} } }`로 rustc를 지정한다.
- `lang.rust = rust.options { ... }`로 rustc option을 지정한다.
- 활성화된 provider의 `.rs` suffix는 raw string source classification에 참여한다.
- `rust.object("path.rs", {...})` helper는 source-local option이 필요할 때 사용한다.
- 모든 compile source가 Rust provider source이고 target이 native link input/deps를 갖지 않으면
  provider final action이 자동 선택된다.

## Rust Options

```lua
rust = rust.options {
  edition = "2021",
  crate_type = "lib",
  cfg = {"feature_demo"},
  externs = {"dep=vendor/libdep.rlib"},
  compile_options = {"-C", "panic=abort"},
}
```

`crate_type`은 object unit lowering에서 `rustc --crate-type`으로 내려간다. 기본값은 `lib`이다.
Provider final action은 target kind에 따라 `bin`, `staticlib`, `cdylib`를 직접 선택한다. 현재
provider가 검증하는 값은 `lib`, `rlib`, `staticlib`, `cdylib`, `dylib`, `bin`, `proc-macro`다.

`compile_options`는 마지막에 그대로 추가된다. QStar는 Rust package manager, crate graph,
Cargo metadata, proc-macro host build, Rust std link policy를 해석하지 않는다.

## Staticlib Consumer Example

Rust provider final action으로 static library를 만들고 C executable에서 소비하는 예:

```lua
local rust = qstar.use_language("rust")

qstar.toolset "host" {
  tools = {
    c = { compiler = qstar.cli {"cc"} },
    archive = qstar.cli {"ar"},
    link = qstar.cli {"cc"},
    rust = rust.tools {
      compiler = qstar.cli {"rustc"},
    },
  },
}

qstar.config "native" {
  toolset = "//:host",
  lang = {
    rust = rust.options {
      edition = "2021",
      crate_type = "lib",
    },
  },
}

qstar.staticlib "rust_core" {
  configs = {"//:native"},
  sources = {"src/rust_core.rs"},
}

qstar.executable "consumer" {
  configs = {"//:native"},
  sources = {"src/consumer.c"},
  deps = {"//:rust_core"},
}
```

`src/rust_core.rs`는 C ABI symbol을 노출해야 한다.

```rust
#[no_mangle]
pub extern "C" fn rust_value() -> i32 {
    77
}
```

이 경로는 `tests/corpus/real-glp/rust-static-consumer`에서 실제 `rustc`로 검증한다. Action log의
Rust action id는 `//:rust_core:archive:0`이며, command는 `rustc --crate-type staticlib ...`가 된다.

## Executable Example

순수 Rust executable은 C linker를 우회하고 provider final action으로 빌드된다.

```lua
qstar.executable "app" {
  configs = {"//:native"},
  sources = {"src/main.rs"},
}
```

```rust
fn main() {}
```

이 경로는 `qstar init --use-language=rust` real scaffold gate에서 Stella/Ninja 양쪽으로 검증한다.

## Object Helper Example

Target 전체 default와 다른 Rust source-local option이 필요하면 helper를 쓴다.

```lua
qstar.staticlib "rust_core" {
  configs = {"//:native"},
  sources = {
    rust.object("src/rust_core.rs", {
      crate_type = "lib",
      cfg = {"feature_demo"},
    }),
  },
}
```

## Scope Boundary

Provider final action은 single-crate `rustc` invocation을 모델링한다. Cargo workspace,
crate registry/lockfile, proc-macro host build, build.rs, Rust package graph resolution은 여전히
QStar core의 책임이 아니다. 이런 흐름은 `qstar.custom_target`으로 Cargo를 직접 호출하거나,
후속 provider가 더 높은 수준의 package action을 명시적으로 제공해야 한다.
