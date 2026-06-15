# Tutorial: C App

QStar는 C/C++/ASM을 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다. 이 튜토리얼은
가장 작은 C executable을 만든다.

## 최소 예제

```lua
qstar.executable "app" {
  sources = {"src/main.c"},
}
```

## 전체 예제

```txt
c-app/
├── qstar.lua
└── src/main.c
```

```lua
qstar.project {
  name = "c-app",
  version = "0.1.0",
  root = ".",
}

qstar.executable "app" {
  sources = {
    "src/main.c",
  },
  lang = {
    c = {
      compile_options = {
        "-Wall",
      },
    },
  },
}
```

## 실패 예제

```lua
qstar.executable "app" {
  sources = {"src/main.h"},
}
```

Header를 `sources`에 넣으면 lint warning 대상이다. Header surface는 `lang.c.public_headers`
또는 `lang.c.private_headers`에 둔다.

## 관련 CLI

```sh
qstar init c-app /tmp/c-app
qstar --file /tmp/c-app/qstar.lua build //:app
qstar --file /tmp/c-app/qstar.lua clean --target //:app
```

## 관련 diagnostic

- `QSTAR040 header listed as source`
- `source path is missing`
