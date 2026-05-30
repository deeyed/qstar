# QStar C-only app/lib/test sample

이 sample은 QStar v0가 C static library, executable, test target, install skeleton을
다루는 최소 project다.

```txt
make -C qstar
qstar/build/bin/qstar --file qstar/tests/manual/c-only/qstar.lua build //:app
qstar/build/bin/qstar --file qstar/tests/manual/c-only/qstar.lua test //:unit
qstar/build/bin/qstar --file qstar/tests/manual/c-only/qstar.lua install //:core --prefix /tmp/qstar-c-only-install
qstar/build/bin/qstar --file qstar/tests/manual/c-only/qstar.lua clean
```

이 sample은 `qstar init`이 아직 없을 때 수동 template 역할을 한다.
