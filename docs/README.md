# QStar Documentation

This directory contains developer-facing design notes, release gates, and
platform validation notes. User-facing authoring documentation lives in
`../wiki/`, and the AI-oriented entrypoint is `../wiki/AI_INDEX.md`.

Current DSL surface:

- `qstar.project`
- `qstar.toolset`
- `qstar.use_language`
- provider authoring: `qstar.language_provider`, `qstar.provider_tools`,
  `qstar.language_options`, `qstar.source`, `qstar.argv`
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

Generic Language Provider (GLP) note: the current runtime preloads built-in
`c`, `cxx`, and `asm` provider namespaces for C/C++/ASM source handling.
`qstar.use_language("id")` first checks a project-local provider manifest at
`qstar/languages/<id>/<id>.qsm` and then falls back to the installed standard
provider bundle under `share/qstar/languages/<id>`. QStar currently ships the
standard `zig`, `rust`, and `cuda` providers. The explicit folder form, such as
`qstar.use_language("qstar/languages/zig")`, stays project-relative. Provider
manifests must return
`qstar.language_provider { api = "qstar.lang/1", ... }`; their `provider.lua`
implementation is loaded in a restricted provider sandbox and only `exports`
are returned to user code. Provider-defined `options` schemas now validate
`lang.<namespace>` tables with string, bool, list, enum, and default metadata.
Provider-defined `units` register source suffixes with the graph-level source
registry, so activated providers can classify raw source strings such as
`"src/main.zig"` into consuming-target-owned object artifacts. Provider-defined
`finals` let pure provider targets lower `executable`, `staticlib`, and
`sharedlib` final artifacts through provider-owned compiler actions instead of
the native C-style linker/archive path. Explicit helpers
such as `zig.object("src/main.zig", {...})` remain available for source-local
options and suffix collision disambiguation. The lowered `command`, `inputs`,
`env`, `outputs`, and `depfile` action template is shared by Stella and Ninja,
including response-file handling and action-log/replay redaction. The object artifact bridge remains
available for hand-written foreign compiler flows.

Important documents:

- `syntax.md`: short current syntax cheat sheet.
- `model.md`: package, target, source/header/output model.
- `graph-ir.md`: internal Graph IR notes.
- `pipeline.md`: current evaluation/build pipeline notes.
- `rule-model.md`: target rule and link model notes.
- `ninja-backend-parity.md`: Ninja lowering parity contract.
- `language-provider-backend-contract.md`: external object artifact bridge boundary.
- `zig-provider.md`: standard Zig provider options, final artifact lowering,
  cache behavior, macOS target ergonomics, and real Zig staticlib/executable fixtures.
- `rust-provider.md`: standard Rust provider options, final artifact lowering,
  real rustc staticlib consumer path, and executable support boundary.
- `init-glp-scaffold.md`: Korean reference for the generic `qstar init` shape
  model, provider vendoring, and provider-defined scaffold metadata.
- `../tests/corpus/real-glp/README.md`: optional real Rust/Zig compiler corpus
  for GLP provider validation beyond fake compiler shims.
- `../tests/real-language-init-scaffold.sh`: optional real Rust/Zig compiler gate
  that validates `qstar init --use-language=rust|zig` app/lib/tool/workspace
  scaffolds with Stella and Ninja.
- `performance-gates.md`: Stella/Ninja performance gate contract.
- `qstar-generic-dsl-backend-seal.md`: Q193 backend/performance seal after the
  generic DSL hard cut.
- `perf/q166-large-performance-refresh.md`: large synthetic Stella/Ninja/daemon timing.
- `progress-output.md`: CMake-style progress output and warning/error color policy.
- `daemon/stella-daemon.md`: Stella daemon design and beta surface.
- `daemon-beta-readiness.md`: daemon beta readiness and opt-in decision.
- `contracts/daemon-read-api.md`: daemon read-only API contract.
- `windows-path-process.md`: Windows path/process preparation.
- `windows-native-alpha.md`: Windows beta candidate validation workflow.
- `windows-artifact-policy.md`: Windows artifact policy.
- `windows-artifact-graph-ir.md`: Windows multi-artifact Graph IR contract.
- `linux-validation.md`: Linux host validation and release asset criteria.
- `public-beta-release.md`: release packaging and wiki sync checklist.
- `qstar-submodule-extraction-prep.md`: submodule extraction notes.
- `qstar-pilot-readiness-seal.md`: pilot readiness gate notes.
- `qstar-v0.2-release-candidate-seal.md`: v0.2 release-candidate gate.
- `qstar-v0.3-seal.md`: v0.3 release-candidate gate.
- `qstar-v0.4-stella-seal.md`: v0.4 Stella workflow gate.
- `qstar-v0.5-readiness.md`: v0.5 readiness gate.
- `qstar-v0.6-readiness.md`: v0.6 readiness gate.
- `qstar-v0.6-post-release-smoke.md`: post-release artifact smoke notes.
- `qstar-v0.7-readiness.md`: v0.7 readiness gate.
- `qstar-v0.8-readiness.md`: next feature-line readiness gate.
- `releases/TEMPLATE.md`: release note template.
- `releases/v0.7.0-beta.md`: current public beta release note.
- `releases/v0.8.0-beta.md`: draft release note for the next beta feature line.

Hard-cut rule: public docs must teach the current generic DSL only. Historical
notes may keep round names, but examples must point readers back to the current
toolset/config/object-bridge surface.
