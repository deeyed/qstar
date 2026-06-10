# Project Layout

QStar package root에는 `qstar.lua`를 둔다. Workspace marker는 `qstar.workspace`다.

```txt
.
├── qstar.lua
├── qstar.workspace
└── src
    └── main.c
```

큰 project는 `qstar.subdir()`로 fragment를 나눈다.

```lua
qstar.subdir("lib")
qstar.subdir("app/src")
```

`qstar.subdir("lib")`는 `lib/lib.qs`를 요구한다. `qstar.subdir("app/src")`는
`app/src/src.qs`를 요구한다.

## Package-root style

```txt
lib/
├── lib.qs
├── include/
│   └── core.h
└── src/
    └── core.c
```

`lib/lib.qs`:

```lua
qstar.staticlib "core" {
  sources = {"lib/src/core.c"},
  public_headers = {"lib/include/core.h"},
  lang = {
    c = {
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
    ├── src.qs
    └── main.c
```

`app/src/src.qs`:

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
        ├── aarch64.qs
        ├── boot.c
        └── start.S
```

Root `qstar.lua`:

```lua
qstar.subdir("kernel/arch/aarch64")
```

`kernel/arch/aarch64/aarch64.qs`의 target label은
`//kernel/arch/aarch64:<name>`이 된다.

## Deprecated fallback

`foo/qstar.qs`는 deprecated fragment name이다. Lint는 `QSTAR003` warning을 낸다.
새 project에서는 항상 `<folder>.qs`를 사용한다.
