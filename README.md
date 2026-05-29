# QStar Developer Binary

QStar is currently a standalone explain-first, dry-run, and authoring-check
build graph evaluator.

Round 9 builds `build/bin/qstar` and installs it next to `cale` through
`install-user`. It is still not wired into `cale build`.

The Lua evaluator uses the official Lua repository as a git submodule at
`qstar/vendor/lua`, pinned to tag `v5.4.8`. The license text is preserved in
`LICENSE/lua.txt`.

Implemented commands:

```txt
qstar --file qstar.lua --dump-graph
qstar --file qstar.lua check //:app
qstar --file qstar.lua explain //:app
qstar --file qstar.lua dry-run //:app
qstar --file qstar.lua --package-alias @core=/path/to/core explain //:app
qstar --file qstar.lua --profile debug --target arm64-apple-macos explain //:app
```

`--dump-graph` prints the raw canonical Graph IR. `explain` validates the
selected target closure, computes dependency-first order, and prints a
non-executing Build Plan IR with action-key material. `dry-run` projects the
same validated closure into deterministic step records that look like executor
input, but every step is marked `execute=no`. It does not spawn compilers,
linkers, generators, package fetchers, or Ninja. `check` is the authoring UX
gate: it validates the same graph plus package-root file existence for real
inputs.

Round 3 adds fixture-level package alias and profile input plumbing. Package
aliases make external labels explainable, but QStar still does not fetch,
resolve, read, or build external packages.

Round 4 added generic header-file path checks. Round 5 deliberately keeps those
checks format-agnostic: QStar treats `.h`, `.hcl`, and future header-like files
as opaque build inputs/install entries. HCL parsing, export filtering, and Cale
language semantics belong to the compiler/HCL checker, not QStar.

Round 6 adds explicit source discovery and command skeletons. The accepted
source languages are `.c`, `.cale`, `.s`, and `.S`; unsupported source suffixes
are rejected before planning. The command skeleton remains non-executing and is
only key material for future toolchain/profile resolution.

Round 7 adds generated output edge skeletons and a target-local toolchain
resolver record. `qstar.genrule` records generator inputs/outputs/args,
`qstar.output(path)` marks generated output spelling in QStar files, and
generated sources must have exactly one producer under `generated/`. QStar still
does not execute the generator or compiler.

Round 8 adds a dry-run executor skeleton and a manual sample project under
`qstar/tests/manual/hello`. The sample intentionally contains `qstar.lua`, a
subdir `foo.qs`, C sources, a generated-source edge, and a public header so it
can be copied elsewhere and exercised by hand.

Round 9 adds `qstar check`. It verifies source/header/generated-input files
exist under the package root, while generated outputs are allowed to be absent
when a `qstar.genrule` produces them. This is intentionally separate from
`explain` and `dry-run`, which remain useful while sketching a graph before all
files exist.

You can already experiment with small `qstar.lua` and subdir `foo.qs` files for
graph shape, source classification, dependency order, command skeleton output,
and dry-run step ordering. Treat that surface as developer-preview until actual
source discovery globs, generator execution, and full toolchain/profile
resolution are stabilized.

Manual smoke:

```txt
make qstar
build/bin/qstar --file qstar/tests/manual/hello/qstar.lua --dump-graph
build/bin/qstar --file qstar/tests/manual/hello/qstar.lua check //:app
build/bin/qstar --file qstar/tests/manual/hello/qstar.lua explain //:app
build/bin/qstar --file qstar/tests/manual/hello/qstar.lua dry-run //:app
```

QStar-local regression:

```txt
make -C qstar check
```
