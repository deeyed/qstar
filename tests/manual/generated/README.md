# QStar generated source/header smoke

This fixture is a hand-authored Round 17 sample. It shows `qstar.genrule`
producing a C source and `qstar.config_header` producing a generated header.

```txt
make -C qstar
qstar/build/bin/qstar --file qstar/tests/manual/generated/qstar.lua check //:app
qstar/build/bin/qstar --file qstar/tests/manual/generated/qstar.lua dry-run //:app
qstar/build/bin/qstar --file qstar/tests/manual/generated/qstar.lua build //:app
qstar/tests/manual/generated/.qstar/out/___app/app
```

Generated files and build state stay under this fixture directory.
