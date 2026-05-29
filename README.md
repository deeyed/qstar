# QStar Developer Binary

QStar is currently a standalone explain-first build graph evaluator.

Round 7 still builds only `build/bin/qstar`; it is not installed by
`install-user` and is not wired into `cale build` yet.

The Lua evaluator uses the official Lua repository as a git submodule at
`qstar/vendor/lua`, pinned to tag `v5.4.8`. The license text is preserved in
`LICENSE/lua.txt`.

Implemented commands:

```txt
qstar --file qstar.lua --dump-graph
qstar --file qstar.lua explain //:app
qstar --file qstar.lua --package-alias @core=/path/to/core explain //:app
qstar --file qstar.lua --profile debug --target arm64-apple-macos explain //:app
```

`--dump-graph` prints the raw canonical Graph IR. `explain` validates the
selected target closure, computes dependency-first order, and prints a
non-executing Build Plan IR with action-key material. It does not spawn
compilers, linkers, package fetchers, or Ninja.

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

You can already experiment with small `qstar.lua` and subdir `foo.qs` files for
graph shape, source classification, dependency order, and command skeleton
output. Treat that surface as developer-preview until actual source discovery
globs, generated-source edges, and toolchain/profile resolution are stabilized.
