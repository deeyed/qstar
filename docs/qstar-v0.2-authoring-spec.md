# QStar Authoring Surface Note

이 문서는 예전 authoring spec을 대체하는 현재 DSL 요약이다. Historical details are
intentionally not preserved here because public docs must not teach removed syntax.

Current authoring rules:

- Root entrypoint is `qstar.lua`.
- Fragment files use `.qst`.
- Helper modules use `.qsm`.
- Tool selection is declared with `qstar.toolset`.
- Reusable compile/link policy is declared with `qstar.config`.
- Generated files use `qstar.custom_target`, `qstar.configure_file`, and `qstar.output`.
- External object producers use `qstar.output(path, {format = "object"})`.
- Package trees use `qstar.stage`.
- Smoke wrappers use `qstar.run_target`.
- Dependency-only aggregate labels use `qstar.group`.

The canonical user-facing references are:

- `../wiki/reference/qstar-lua.md`
- `../wiki/reference/toolsets.md`
- `../wiki/reference/configs.md`
- `../wiki/reference/object-artifacts.md`
- `../wiki/reference/custom-target.md`
- `../wiki/reference/run-target.md`
- `../wiki/reference/target-rules.md`
