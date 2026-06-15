# QStar Documentation

This directory contains developer-facing design notes, release gates, and
platform validation notes. User-facing authoring documentation lives in
`../wiki/`, and the AI-oriented entrypoint is `../wiki/AI_INDEX.md`.

Current DSL surface:

- `qstar.project`
- `qstar.toolset`
- `qstar.config`
- artifact targets: `qstar.executable`, `qstar.staticlib`, `qstar.sharedlib`,
  `qstar.test`
- generated actions: `qstar.custom_target`, `qstar.configure_file`
- utility rules: `qstar.run_target`, `qstar.group`, `qstar.stage`
- imports: `qstar.import_file`, `qstar.import_module`, `qstar.subdir`
- command helpers: `qstar.cli`, `qstar.status`, `qstar.input`, `qstar.output`,
  `qstar.target_file`, `qstar.stage_file`
- authoring helpers: `qstar.files`, `qstar.join`, `qstar.copy`, `qstar.append`,
  `qstar.merge`, `qstar.extend`

Important documents:

- `syntax.md`: short current syntax cheat sheet.
- `model.md`: package, target, source/header/output model.
- `graph-ir.md`: internal Graph IR notes.
- `pipeline.md`: current evaluation/build pipeline notes.
- `rule-model.md`: target rule and link model notes.
- `ninja-backend-parity.md`: Ninja lowering parity contract.
- `language-provider-backend-contract.md`: external object artifact bridge boundary.
- `performance-gates.md`: Stella/Ninja performance gate contract.
- `perf/q166-large-performance-refresh.md`: large synthetic Stella/Ninja/daemon timing.
- `progress-output.md`: CMake-style progress output and warning/error color policy.
- `daemon/stella-daemon.md`: Stella daemon design and beta surface.
- `contracts/daemon-read-api.md`: daemon read-only API contract.
- `windows-path-process.md`: Windows path/process preparation.
- `windows-artifact-policy.md`: Windows artifact policy.
- `linux-validation.md`: Linux host validation and release asset criteria.
- `public-beta-release.md`: release packaging and wiki sync checklist.
- `qstar-v0.8-readiness.md`: next feature-line readiness gate.
- `releases/v0.8.0-beta.md`: draft release note for the next beta feature line.

Hard-cut rule: public docs must teach the current generic DSL only. Historical
notes may keep round names, but examples must point readers back to the current
toolset/config/object-bridge surface.
