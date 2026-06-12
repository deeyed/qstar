# Freestanding Corpus Fixture

This fixture keeps the profile/toolchain doctor surface stable for a small
firmware-style project. It intentionally uses package-local fake tools so the
graph can be explained on machines without a cross compiler installed.

Manual smoke:

```sh
./build/bin/qstar --file tests/corpus/freestanding/qstar.lua doctor
./build/bin/qstar --file tests/corpus/freestanding/qstar.lua explain //:kernel
```
