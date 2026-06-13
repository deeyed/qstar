# Object Artifacts

QStar는 C/C++/Cale을 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다. QStar가
직접 모르는 언어도 외부 compiler가 object file을 만들 수 있다면 `qstar.custom_target`과
`qstar.output(path, {format = "object"})`로 link graph에 연결할 수 있다.

이 방식을 object artifact bridge라고 부른다. QStar는 해당 언어의 문법, module, package,
semantic rule을 해석하지 않는다. QStar가 소유하는 것은 generated object artifact의 path,
dependency edge, command argv, cache key, replay/log뿐이다.

## 최소 예제

```lua
qstar.custom_target "foreign_object" {
  inputs = {"src/foreign_source.ext"},
  outputs = {
    qstar.output("generated/foreign.o", {
      format = "object",
    }),
  },
  command = qstar.cli {"tools/compile-foreign.sh", qstar.input(0), qstar.output(0)},
  description = qstar.status("Building foreign object generated/foreign.o"),
}

qstar.executable "app" {
  sources = {
    "src/main.c",
    qstar.output("generated/foreign.o"),
  },
}
```

## 전체 예제

macOS에서 Objective-C source를 C target에 연결하려면 Objective-C 자체를 QStar language
provider로 추가하지 않는다. 대신 package-local wrapper나 profile `path_tools`로 허용된
compiler를 `qstar.custom_target`에서 호출하고, 결과 `.o`를 artifact target의 `sources`에
넣는다.

```lua
qstar.project {
  name = "mixed-macos-app",
  root = ".",
  build_dir = "build/qstar",
  generated_dir = "build/qstar/generated",
}

qstar.profile "default" {
  path_tools = {"clang"},
}

qstar.custom_target "objc_app_delegate" {
  inputs = {"src/AppDelegate.m"},
  outputs = {
    qstar.output("build/qstar/generated/objc/AppDelegate.o", {
      format = "object",
    }),
  },
  command = qstar.cli {
    "clang",
    "-x", "objective-c",
    "-c", qstar.input(0),
    "-o", qstar.output(0),
  },
  description = qstar.status("Building Objective-C object AppDelegate.o"),
}

qstar.executable "app" {
  sources = {
    "src/main.c",
    qstar.output("build/qstar/generated/objc/AppDelegate.o"),
  },
  frameworks = {"Foundation"},
}
```

Rust, Zig, Swift 같은 toolchain도 같은 원칙을 따른다. 외부 compiler가 package-relative
object file을 만들고, 그 output이 `format = "object"`로 선언되어 있으며, consuming target이
그 object path를 `sources`에 넣으면 QStar는 그 file을 다시 compile하지 않고 final
archive/link input으로 사용한다.

## 실패 예제

```lua
qstar.executable "bad" {
  sources = {
    "src/main.c",
    "src/AppDelegate.m",
  },
}
```

지원되지 않는 source suffix를 `sources`에 직접 넣으면 QStar는 그 언어를 compile할 provider를
찾지 않는다. 이런 source는 `qstar.custom_target`으로 object artifact를 먼저 만들고,
generated `.o` 또는 `.obj`를 consuming target의 `sources`에 넣는다.

## 관련 CLI

```sh
qstar --file qstar.lua check //...
qstar --file qstar.lua dry-run //:app
qstar --file qstar.lua explain //:app
qstar --file qstar.lua build //:app
qstar --file qstar.lua -G ninja build //:app
qstar --file qstar.lua action-log //:objc_app_delegate:generate:0
```

## 관련 diagnostic

- `qstar: unsupported source extension`
- `qstar: generated source '...' has no generating action`
- `qstar: generated output '...' must be under generated_dir`
- `qstar: generated output '...' has multiple producers`
