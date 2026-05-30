# QStar Manual Hello Fixture

This fixture is for manual QStar experiments. It is intentionally small and is
primarily meant for graph/query/check/explain/dry-run UX. Its generated action
uses a placeholder tool name, so it is not the recommended `qstar build` smoke.

From the repository root:

```sh
make qstar
build/bin/qstar --file qstar/tests/manual/hello/qstar.lua --dump-graph
build/bin/qstar --file qstar/tests/manual/hello/qstar.lua list-targets
build/bin/qstar --file qstar/tests/manual/hello/qstar.lua query //:app
build/bin/qstar --file qstar/tests/manual/hello/qstar.lua doctor
build/bin/qstar --file qstar/tests/manual/hello/qstar.lua check //:app
build/bin/qstar --file qstar/tests/manual/hello/qstar.lua explain //:app
build/bin/qstar --file qstar/tests/manual/hello/qstar.lua dry-run //:app
```

The expected behavior is a deterministic graph, authoring check, Build Plan IR,
and dry-run stream. For an executor smoke, create a C-only local package with a
package-relative generated tool as shown in `docs/qstar/examples.md`.
