# QStar generated source/header sample

이 fixture는 Round 20 v0에서 손으로 작성한 corpus sample이다.
`qstar.genrule`이 C source를 만들고, `qstar.config_header`가 generated header를
만드는 흐름을 확인한다.

```txt
make -C qstar
qstar/build/bin/qstar --file qstar/tests/manual/generated/qstar.lua check //:app
qstar/build/bin/qstar --file qstar/tests/manual/generated/qstar.lua dry-run //:app
qstar/build/bin/qstar --file qstar/tests/manual/generated/qstar.lua build //:app
qstar/tests/manual/generated/.qstar/out/___app/app
```

생성된 파일과 build state는 이 fixture directory 아래에만 남는다.
