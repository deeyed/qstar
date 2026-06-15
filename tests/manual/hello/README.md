# QStar hello manual fixture

이 디렉터리는 QStar local graph와 dry-run/build 동작을 손으로 확인하기 위한 작은 fixture다.

## 포함 내용

- root `qstar.lua`
- subdir fragment
- 작은 C source
- public header
- generated-source edge 예시

## 사용법

```txt
../../../build/bin/qstar --file qstar.lua --dump-graph
../../../build/bin/qstar --file qstar.lua list-targets
../../../build/bin/qstar --file qstar.lua check //:app
../../../build/bin/qstar --file qstar.lua explain //:app
../../../build/bin/qstar --file qstar.lua dry-run //:app
../../../build/bin/qstar --file qstar.lua build //:app
../../../build/bin/qstar --file qstar.lua why-rebuild //:app
```

QStar repo root에서 `make -C qstar`를 실행하면 기본 binary는 `qstar/build/bin/qstar`에 생긴다. 이 fixture는 QStar authoring UX와 deterministic graph output을 확인하기 위한 것이며, external language 언어 semantics를 검증하는 fixture는 아니다.
