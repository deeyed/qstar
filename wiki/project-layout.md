# Project Layout

QStar package root에는 반드시 `qstar.lua`를 둔다. QStar는 현재 authoring file에서
위로 올라가며 가장 가까운 `qstar.lua`를 package root로 삼는다. `qstar.workspace`
marker는 제거되었다.

```txt
.
├── qstar.lua
└── src
    └── main.c
```

큰 project는 `qstar.subdir()`로 fragment를 나눈다.

```lua
qstar.subdir("lib")
qstar.subdir("app/src")
```

`qstar.subdir("lib")`는 `lib/lib.qst`를 요구한다. `qstar.subdir("app/src")`는
`app/src/src.qst`를 요구한다.

## Package-root style

```txt
lib/
├── lib.qst
├── include/
│   └── core.h
└── src/
    └── core.c
```

`lib/lib.qst`:

```lua
qstar.staticlib "core" {
  sources = {"lib/src/core.c"},
  lang = {
    c = {
      public_headers = {"lib/include/core.h"},
      public_include_dirs = {"lib/include"},
    },
  },
}
```

이 target label은 `//lib:core`다.

## Source-dir style

```txt
app/
└── src/
    ├── src.qst
    └── main.c
```

`app/src/src.qst`:

```lua
qstar.executable "app" {
  sources = {"app/src/main.c"},
}
```

이 target label은 `//app/src:app`다.

깊은 source-dir fragment도 같은 규칙을 따른다.

```txt
kernel/
└── arch/
    └── aarch64/
        ├── aarch64.qst
        ├── boot.c
        └── start.S
```

Root `qstar.lua`:

```lua
qstar.subdir("kernel/arch/aarch64")
```

`kernel/arch/aarch64/aarch64.qst`의 target label은
`//kernel/arch/aarch64:<name>`이 된다.

## Removed fallback

`foo/qstar.qst` fallback은 없다. Fragment는 항상 `<folder>.qst`다.
`.qs` fragment가 남아 있으면 lint는 `QSTAR003` error를 낸다.
