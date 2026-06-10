# Labels And Fragments

QStar는 C/C++/Cale을 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다. Label은
target을 가리키는 안정적인 주소이고, fragment는 큰 project를 나누는 authoring file이다.

## 최소 예제

```lua
qstar.subdir("lib")
```

`qstar.subdir("lib")`는 `lib/lib.qst`를 읽고, 그 안의 `qstar.staticlib "core"`는
`//lib:core` label을 가진다.

## 전체 예제

```txt
.
├── qstar.lua
├── lib/lib.qst
└── app/src/src.qst
```

```lua
qstar.subdir("lib")
qstar.subdir("app/src")
```

```lua
qstar.executable "app" {
  sources = {
    "app/src/main.c",
  },
  deps = {
    "//lib:core",
  },
}
```

`qstar.subdir("app/src")`는 `app/src/src.qst`를 요구하고 target label은
`//app/src:app`이 된다.

## 실패 예제

```lua
qstar.subdir("lib")
```

`lib/lib.qst`가 없으면 missing fragment diagnostic을 낸다. Legacy fragment fallback은
없다.

## 관련 CLI

```sh
qstar --file qstar.lua list-targets
qstar --file qstar.lua query //lib:core
qstar --file qstar.lua explain //app/src:app
```

## 관련 diagnostic

- `QSTAR002 subdir fragment must be <folder>.qst`
- `QSTAR010 unknown target label`
- `duplicate target label`
