# Tutorial: Generated Config

QStar는 C/C++/ASM을 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다. 이 튜토리얼은
config header와 generated C source를 compile input으로 연결한다.

## 최소 예제

```lua
qstar.configure_file "cfg" {
  output = qstar.output("generated/config.h"),
  defines = {"APP_VALUE=42"},
}
```

## 전체 예제

```lua
qstar.configure_file "cfg" {
  output = qstar.output("generated/config.h"),
  defines = {
    "APP_VALUE=42",
  },
}

qstar.custom_target "value_source" {
  inputs = {"tools/gen-value.sh"},
  outputs = {qstar.output("generated/value.c")},
  command = qstar.cli {"tools/gen-value.sh", qstar.output(0)},
}

qstar.executable "app" {
  sources = {
    "src/main.c",
    qstar.output("generated/value.c"),
  },
  lang = {
    c = {
      private_headers = {
        qstar.output("generated/config.h"),
      },
      include_dirs = {
        "generated",
      },
    },
  },
}
```

## 실패 예제

```lua
qstar.custom_target "a" { outputs = {qstar.output("generated/value.c")} }
qstar.custom_target "b" { outputs = {qstar.output("generated/value.c")} }
```

같은 generated output을 둘 이상의 target이 만들면 ambiguous producer로 reject된다.

## 관련 CLI

```sh
qstar init generated /tmp/generated
qstar --file /tmp/generated/qstar.lua build //:app --explain-cache
qstar --file /tmp/generated/qstar.lua why-rebuild //:app
```

## 관련 diagnostic

- `multiple producers`
- `cache_miss reason=depfile-changed`
- `generated output must be package-relative`
