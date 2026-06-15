# QStar Pilot Readiness Seal

status: pilot-readiness seal

This gate verifies that QStar behaves like a generic standalone build system:

- formatter/help/wiki/CLI sync
- `qstar <subcommand> --help`
- C/C++/ASM sample corpora
- generated object artifact bridge
- stage/install/replay/log UX
- Stella/Ninja parity checks
- medium project performance gates
- VSCode/LSP authoring surface

Gate:

```sh
make qstar-pilot-readiness-tests
```

The gate must not rely on domain-specific builtins. Package transforms, smoke
wrappers, and staged layouts are represented with `qstar.custom_target`,
`qstar.run_target`, and `qstar.stage`.
