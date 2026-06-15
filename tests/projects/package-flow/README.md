# Package Flow Corpus

This self-contained project is QStar's package-flow release corpus. It models
practical artifact and staging flows without adding domain-specific target kinds
to the core DSL:

- C + preprocessed assembly + linker input handling.
- Object-like output conversion through `qstar.custom_target` + `qstar.cli`.
- Copy-only package staging with `qstar.stage`.
- External smoke validation through `qstar.run_target` and `expect`.
- Alternate output naming and MSVC-style response-file handling through
  target-local artifact and link policy.

The tools in `tools/` are deterministic fake tools. A real package can replace
them through `qstar.toolset` and `qstar.config` declarations in `qstar.lua`
without changing the target model.

Useful commands:

```sh
qstar --file qstar.lua dry-run //:module
qstar --file qstar.lua stage //:bundle
qstar --file qstar.lua build //:package_smoke
qstar --file qstar.lua --target x86_64-pc-windows-msvc --toolchain clang dry-run //:msvc_payload
qstar --file qstar.lua --target x86_64-pc-windows-msvc --toolchain clang stage //:runtime
```

The smoke wrapper writes a deterministic log that includes `QSTAR-SMOKE-DONE`
so QStar can validate `run_target` expectation plumbing without requiring any
machine-specific tool.
