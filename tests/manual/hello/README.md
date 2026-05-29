# QStar Manual Hello Fixture

This fixture is for manual Round 8 experiments. It is intentionally small and
does not require QStar to execute compilers or generators.

From the repository root:

```sh
make qstar
build/bin/qstar --file qstar/tests/manual/hello/qstar.lua --dump-graph
build/bin/qstar --file qstar/tests/manual/hello/qstar.lua check //:app
build/bin/qstar --file qstar/tests/manual/hello/qstar.lua explain //:app
build/bin/qstar --file qstar/tests/manual/hello/qstar.lua dry-run //:app
```

The expected behavior is a deterministic graph, authoring check, Build Plan IR,
and dry-run stream. No generated file, object file, archive, or executable is
created.
