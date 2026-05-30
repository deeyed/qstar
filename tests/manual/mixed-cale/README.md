# QStar C/Cale mixed dry-run sample

이 sample은 QStar가 C와 Cale source를 같은 target의 build input으로 인식하는지
확인하기 위한 dry-run 중심 project다. 실제 build는 `cale` compiler가 PATH에 있을 때만
시도한다.

```txt
make -C qstar
qstar/build/bin/qstar --file qstar/tests/manual/mixed-cale/qstar.lua dry-run //:mixed
```

QStar는 `.cale` 파일 내용을 해석하지 않고 `cale -c ...` process invocation을 계획한다.
