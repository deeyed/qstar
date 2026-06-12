# Medium Corpus Fixture

This fixture is a stable manual smoke input for Stella executor and Ninja backend
work. The release performance gate uses `tests/medium-project-performance.sh`,
which generates a larger temporary medium project and records clean, no-op, and
incremental timings.

Manual smoke:

```sh
./build/bin/qstar --file tests/corpus/medium/qstar.lua build //:all
./build/bin/qstar --file tests/corpus/medium/qstar.lua build //:all
```
