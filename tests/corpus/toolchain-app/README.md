# Toolchain App Corpus Fixture

This fixture keeps the toolset/doctor surface stable for a small project that
uses package-local fake tools. The graph can be explained on machines without a
non-host compiler installed.

Manual smoke:

```sh
./build/bin/qstar --file tests/corpus/toolchain-app/qstar.lua doctor
./build/bin/qstar --file tests/corpus/toolchain-app/qstar.lua explain //:module_app
```
