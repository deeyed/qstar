# QStar Developer Binary

QStar is currently a standalone explain-first build graph evaluator.

Round 1 builds only `build/bin/qstar`; it is not installed by `install-user`
and is not wired into `cale build` yet.

The Lua evaluator uses the official Lua repository as a git submodule at
`qstar/vendor/lua`, pinned to tag `v5.4.8`. The license text is preserved in
`LICENSE/lua.txt`.

Initial commands:

```txt
qstar --file qstar.lua --dump-graph
qstar --file qstar.lua explain //:app
```
