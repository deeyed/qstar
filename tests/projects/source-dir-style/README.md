# QStar Source-Dir Style Corpus

이 corpus는 root `qstar.lua`가 source directory fragment를 직접 include하는
사용 방식을 검증한다.

```sh
qstar --file qstar.lua lint //...
qstar --file qstar.lua explain //app/src:app
qstar --file qstar.lua dry-run //app/src:app
qstar --file qstar.lua build //app/src:app
```

`qstar.subdir("app/src")`는 `app/src/src.qs`를 읽고, 그 안의 target label은
`//app/src:app`가 된다.

public header는 해당 fragment package의 `include/` 아래에 둔다. 예를 들어
`qstar.subdir("lib/src")`로 읽힌 `//lib/src:core` target의 public header는
`lib/src/include/core.h`처럼 배치한다.
