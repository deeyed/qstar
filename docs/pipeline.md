# QStar Pipeline

Current high-level pipeline:

1. Read `qstar.lua`.
2. Evaluate explicit `.qst` imports and subdir fragments.
3. Evaluate `.qsm` helper modules as side-effect-free returned tables.
4. Validate package-relative paths, labels, configs, toolsets, generated outputs,
   and dependency edges.
5. Merge `configs` into target-local options.
6. Resolve tool role argv through `qstar.toolset`.
7. Lower targets and generated actions into Stella action plans.
8. Optionally emit Ninja with `-G ninja`.
9. Execute build/test/stage/project-command workflows through the effective generator boundary.

QStar keeps commands shell-free. All external commands are `qstar.cli { ... }`
argv vectors. Stage copy and manifest work remain QStar-owned even when artifact
production is delegated to Ninja. Project-defined export workflows use
`qstar.command` and `qstar.step.export_stage`.

The current public syntax reference is `../wiki/reference/qstar-lua.md`.
