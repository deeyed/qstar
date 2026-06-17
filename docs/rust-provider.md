# Rust Provider

QStar의 표준 Rust provider는 현재 `rustc --emit=obj`로 Rust source를 object artifact로 낮춘다.
이 object는 QStar의 기존 `staticlib`, `executable`, `sharedlib` link graph에서 다른 object와
같은 방식으로 소비된다.

## 지원 범위

- `qstar.use_language("rust")`로 표준 provider를 활성화한다.
- `tools = { rust = rust.tools { compiler = qstar.cli {"rustc"} } }`로 rustc를 지정한다.
- `lang.rust = rust.options { ... }`로 rustc object emission option을 지정한다.
- 활성화된 provider의 `.rs` suffix는 raw string source classification에 참여한다.
- `rust.object("path.rs", {...})` helper는 source-local option이 필요할 때 사용한다.

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

`crate_type`은 `rustc --crate-type`으로 내려간다. 기본값은 `lib`이다. 현재 provider가 검증하는
값은 `lib`, `rlib`, `staticlib`, `cdylib`, `dylib`, `bin`, `proc-macro`다.

`compile_options`는 마지막에 그대로 추가된다. QStar는 Rust package manager, crate graph,
Cargo metadata, proc-macro host build, Rust std link policy를 해석하지 않는다.

## Staticlib Consumer Example

Rust object를 QStar static library로 묶고 C executable에서 소비하는 예:

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

이 경로는 `tests/corpus/real-glp/rust-static-consumer`에서 실제 `rustc`로 검증한다.

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

## Executable Limitation

현재 Rust provider는 Rust source를 object artifact로 낮추는 provider다. QStar의 `executable`
final action은 platform C-style linker contract 위에서 동작하므로, `rustc --crate-type bin
--emit=obj`로 만든 object를 `cc`/`clang`/`link.exe`에 바로 넘겨 완전한 Rust executable을
만드는 것을 정식 지원하지 않는다. 일반 Rust executable은 Rust std runtime, panic runtime,
linker driver, crate metadata, Cargo-style dependency graph가 함께 필요하다.

따라서 지금의 권장 경로는 다음 중 하나다.

- Rust code를 C ABI object/staticlib로 노출하고 QStar target에서 소비한다.
- 완전한 Rust executable은 `qstar.custom_target`으로 `rustc` 또는 `cargo`를 직접 호출하고,
  산출물을 explicit generated artifact로 다룬다.
- 장기적으로는 GLP final-action 확장을 통해 provider가 compile unit뿐 아니라 executable/link
  final action까지 선언하는 설계를 추가한다.
