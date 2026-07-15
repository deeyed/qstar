# QStar Compatibility Policy

Status: Q256 stable DSL compatibility policy seal.

QStar is still beta, so this document is a release-candidate promise rather than
a v1 guarantee today. Its job is to define which surfaces are intended to become
stable at QStar 1.0, which surfaces remain beta or experimental, and how future
removals must be handled after v1.

The compatibility boundary is intentionally about public authoring and CLI
contracts. Internal file names, cache layouts, planner state, executor
implementation details, and unversioned debug dumps are not stable unless a
public document explicitly promotes them.

## Buckets

Every public surface must live in one of these buckets.

| Bucket | Meaning |
| --- | --- |
| Stable at v1 | The surface is intended to be covered by the v1 compatibility policy once the v1 release gate passes. It can receive additive fields, clearer diagnostics, and bug fixes, but it must not be hard-cut inside the same major version. |
| Beta opt-in | The surface is documented and supported for validation, but QStar does not yet promise stable/default behavior. It may change with release notes and migration guidance before promotion. |
| Out-of-core | The feature is intentionally not QStar core. QStar may interoperate with tools that provide it, but QStar should not grow built-in semantics for it without a separate design. |

## Stable At V1 Candidate Surface

The following authoring surfaces are v1 stable candidates. They are not a final
v1 promise until the v1 release checklist passes, but future work should treat
them as protected unless this document is updated first.

### Project And Loading

- Root entrypoint: `qstar.lua`.
- Fragment suffix and loading: `.qst`, `qstar.subdir`, and `qstar.import_file`.
- Helper module suffix and loading: `.qsm` and cached `qstar.import_module`
  exports.
- Project metadata: `qstar.project` with `name`, `version`, `root`,
  `build_dir`, `generated_dir`, and `compile_commands`.
- Read-only host and project constants: `QSTAR_VERSION`, `QSTAR_HOST_OS`,
  `QSTAR_HOST_ARCH`, `QSTAR_PACKAGE_ROOT`, `QSTAR_PROJECT_ROOT`,
  `qstar.version`, `qstar.host.os`, `qstar.host.arch`, and
  `qstar.project.root`.
- The sandboxed Lua authoring subset used by current docs: local helpers,
  table literals, `ipairs`, `pairs`, `table.insert`, string/list composition,
  and deterministic graph declarations.

### Toolsets And Configs

- `qstar.toolset` declarations with explicit argv-vector tools.
- Core tool roles `archive` and `link`.
- Provider namespace tool bundles such as `c`, `cxx`, `asm`, and activated
  external provider namespaces.
- Toolset response-file policy: `response_files`, `response_style`,
  `path_tools`, and `allow_absolute_tools`.
- `qstar.config` reusable option bundles.
- Config merge semantics: referenced configs apply in list order, list fields
  append, and target-local scalar fields override config scalar fields.
- Config and target fields for explicit command-line policy: `lang`, `libs`,
  `lib_dirs`, `link`, `link_options`, `link_inputs`, `toolset`, and
  `artifact_name`.

### Targets And Artifact Rules

- Artifact targets: `qstar.executable`, `qstar.staticlib`, `qstar.sharedlib`,
  and `qstar.test`.
- Generic test orchestration: `qstar.test_resource`, user-defined resource ids
  and capacities, per-test `resources`, retry/setup/cleanup/timeout/manual/skip,
  pass/fail/skip/error/timeout results, JSON `qstar-test-results-v1`, optional
  JUnit reports, and Stella/Ninja scheduler parity.
- Typed dependency targets: artifact-free `qstar.interface`, platform-selected
  package-local `qstar.imported`, and executable path `qstar.tool`.
- Explicit consumer requirements through `compile_usage = {options, inputs}`
  and `link_usage = {options, inputs}`. Options are verbatim argv items and
  inputs are rebuild dependencies; imported metadata never infers flags.
- Object collection targets: `qstar.objectlib` with `compile_context = "own"` or
  `compile_context = "consumer"` and artifact target consumption through
  `objects = {...}`. Consumer-context objectlibs compile source-owned leaf inputs
  under each consuming target's effective configs/lang/toolset with per-consumer
  object identity. Activated GLP raw source strings, explicit provider source
  tokens in `"own"` context, and generated object artifacts declared with
  `qstar.output(..., {format = "object"})` are valid objectlib sources.
  Consumer-context re-lowering of explicit provider source tokens remains
  outside the stable promise; use raw provider source strings or `"own"` context
  there.
- Utility rules: `qstar.group`, `qstar.stage`, and `qstar.target_family`.
- Generated action rules: `qstar.configure_file`, `qstar.custom_target`, and
  `qstar.transform`.
- Run actions: `qstar.run_target` with `inputs`, `command`, `timeout`, and
  `expect`.
- Target dependency fields: `deps`, `public_deps`, `private_deps`, `configs`,
  `sources`, and `visibility`.
- Target-local language option tables under `lang.<namespace>`.
- Separation of `link_options` from `link_inputs`.
- Shared-library public artifact model, including the Windows runtime/import
  artifact selector contract.
- Object artifact bridge through `qstar.output(path, {format = "object"})`.
- Generated output identity and producer discovery for `qstar.target_file`.
- Stage layout tokens through `qstar.stage_dir` and `qstar.stage_file`.

### Language Consumer Surface

- Built-in `lang.c`, `lang.cxx`, and `lang.asm` namespaces remain available
  without explicit provider activation.
- `qstar.use_language("<id>")` activates a bundled or project-local provider and
  returns the provider helper table.
- Short provider id resolution checks project-local
  `qstar/languages/<id>/<id>.qsm` before the installed standard bundle.
- Activated provider namespaces can validate `lang.<namespace>` option tables.
- Activated provider source suffixes can classify raw source strings in
  `sources`.
- Explicit provider helpers such as `zig.object(...)` remain the disambiguation
  path for source-local options and suffix collisions.
- `qstar init --use-language=...` can vendor installed standard providers into a
  project-local `qstar/languages` tree.
- Standard provider consumer contracts for bundled `zig`, `rust`, and `cuda`
  include their short ids, namespaces, documented helper exports, documented
  option schemas, raw source suffix classification, and provider vendoring
  behavior. `make qstar-standard-provider-compatibility-tests` is the fake-tool
  Stella/Ninja gate that keeps this consumer contract covered without requiring
  real Zig/Rust/CUDA compiler installations.

This section is the consumer-facing GLP promise. The provider-author API is a
separate beta surface until the checklist below is completed.

### Project Commands And Workflow Helpers

- Root-only `qstar.command`.
- Pure, deeply immutable `qstar.command_spec` values and root-only
  `qstar.command_set` materialization. Materialized commands share the stable
  direct-command CLI and `qstar-commands-v1` JSON contract.
- Command metadata fields: `description`, `options`, `env`, `working_dir`,
  `steps`, `is_default`, `hidden`, and `aliases`.
- Typed command options: `qstar.param.string`, `qstar.param.path`,
  `qstar.param.bool`, `qstar.param.int`, `qstar.param.enum`, and
  `qstar.param.list`.
- Runtime option references through `qstar.param("name")`.
- Bool argv helpers: `qstar.arg_if` and `qstar.args_if`.
- Command steps: `qstar.step.build`, `qstar.step.test`, `qstar.step.stage`,
  `qstar.step.check`, `qstar.step.lint`, `qstar.step.run`,
  `qstar.step.call`, and `qstar.step.export_stage`.
- Project-defined command names, including `install`, `deploy`, `package`, or
  any other non-conflicting project command name.
- `qstar.test_suite` consumer syntax, nested test/run target membership,
  free-form tag/manual selection, and repeated `qstar test --suite/--tag/--exclude-tag`.
- Test execution options `--jobs`, `--include-manual`, `--report-json`, and
  `--output-junit`.

### Common Helpers

- `qstar.cli` argv vectors.
- `qstar.status` one-line action descriptions.
- `qstar.input` and `qstar.output`.
- `qstar.target_file`, including named artifact selectors.
- `qstar.tool_file` executable dependency references for tool, imported tool,
  executable, and test targets.
- `qstar.files`, `qstar.join`, `qstar.copy`, `qstar.append`, `qstar.merge`, and
  `qstar.extend`.

### CLI And Machine-Readable Output

- Global options: `--file`, `-G`, `--generator`, `-B`, and `--color`.
- Stable authoring commands: `docs`, `init`, `list-targets`, `commands`,
  `query`, `doctor`, `check`, `lint`, `fmt`, `explain`, `dry-run`,
  `emit-ninja`, `build`, `test`, `stage`, `why-rebuild`, `clean`, `log`,
  `last-failure`, `action-log`, and `replay`.
- Project command dispatch declared by `qstar.command`.
- JSON output schemas that already advertise stable machine-readable use,
  especially `list-targets --format json`, `query --format json`, and
  `commands --format json`.
- Local docs, wiki, and manpage lookup surfaces packaged with the release.

Text progress output is intended to stay useful for humans, but only documented
machine-readable formats are compatibility anchors.

## Beta Or Experimental Surfaces

These surfaces are allowed to remain outside the v1 stable promise unless a
later round explicitly promotes them.

| Surface | Current boundary |
| --- | --- |
| Provider-author API | `qstar.language_provider { api = "qstar.lang/1" }`, manifest fields `id`, `version`, `namespace`, `implementation`, `tools`, `units`, `finals`, `options`, `exports`, `scaffold`, implementation helpers `qstar.provider_tools`, `qstar.language_options`, `qstar.source`, `qstar.argv`, provider sandbox capability, lowering result fields `command`, `env`, `inputs`, `outputs`, `depfile`, scaffold schema, and provider implementation loading remain a versioned beta contract. Unknown/future manifest APIs are rejected instead of guessed. |
| Standard provider internals | Bundled Zig, Rust, and CUDA provider ids, namespaces, documented helper exports, option schemas, raw source classification, final-action availability, and init vendoring behavior are consumer-facing stable candidates. Their `provider.lua` implementation details, exact compiler argv construction, cache layout, and future language-tool integration choices remain beta. |
| Provider final-action lowering internals | Consumer behavior is a stable candidate; provider-author lowering hooks and result schema remain beta until versioned or frozen. |
| Stella daemon | The daemon is beta opt-in, not default behavior. `--use-daemon=auto|always`, `qstar daemon --start/--stop/--serve/--status`, daemon build forwarding, the read API, `qstar-daemon-query-v2`/`qstar-daemon-read-v1`, socket lifecycle, watcher internals, daemon performance numbers, and Windows named pipe support are not v1 stable. `make qstar-daemon-beta-boundary-tests` is the current beta guard for fallback parity, normal Stella parity, read API freshness, socket permission checks, identity mismatch rejection, and stale socket/pid/lock cleanup. |
| Optional real compiler corpus | Real Rust/Zig compiler validation is useful evidence, but it is optional and skipped when compilers are unavailable. |
| Hosted manual validation lanes | GitHub Actions workflow names and artifact layouts are release evidence contracts, not general user authoring syntax. |
| Performance numbers | Performance snapshots are report-only release inputs. They are not speed guarantees. |
| Debug state dumps | `QSTAR_DEBUG_STATE_DUMPS`, internal state files, cache tables, and planner debug JSON are not stable public APIs. |

## Out-Of-Core Surface

QStar v1 must not silently become a package manager or deployment platform.

Out-of-core for v1:

- package registry
- dependency resolver
- lockfile format
- network fetch policy
- credential or auth storage
- remote package cache
- remote daemon access
- domain-specific marker parsers or environment-specific image semantics

QStar may build artifacts, transform artifacts, stage layouts, and run explicit
commands. It should not own domain semantics that belong to a compiler, test
harness, package manager, deployment tool, or project-specific checker.

## Removed Legacy Surfaces

These surfaces are already removed and must not reappear as compatibility
shims.

- The profile-era top-level API remains removed.
- Profile-era CLI switches for target/toolchain/stdlib policy remain removed.
- Automatic cross-compilation flag injection remains removed; users express
  target, sysroot, resource directory, standard library, and runtime policy as
  explicit toolset/config argv.
- The core install command remains removed. Projects may declare an `install`
  command with `qstar.command` and `qstar.step.export_stage`.
- Language-shaped init templates remain removed. The stable shape vocabulary is
  `app`, `lib`, `tool`, `empty`, and `workspace`; language support is selected
  with `--use-language`.
- Removed API shims must not be registered just to print migration diagnostics.
  A removed surface should behave as if it never existed unless a release note
  explicitly grants a temporary diagnostic period.

The smoke guard must continue to reject public examples that teach old
profile-era syntax, old core install semantics, old language-shaped init forms,
or domain-specific build-system semantics.

## Additive Changes

These are compatible within a stable major version:

- adding a new optional field with a documented default
- adding a new target kind only when it does not change existing target behavior
- adding a new command step or param type without changing existing ones
- adding a new provider namespace
- adding a new artifact selector to a target that already has a default artifact
- adding clearer diagnostics while preserving diagnostic code meaning
- adding a new JSON field while preserving existing fields and schema version

Additive changes must still be documented before release.

## Breaking Changes After V1

After QStar 1.0, stable surfaces must not be hard-cut inside the same major
version. A breaking change to a stable surface requires all of the following:

1. A replacement exists and is documented.
2. The old surface emits a diagnostic or warning for at least two public minor
   releases or 90 days, whichever is longer.
3. Every affected release note names the old surface, the replacement, and the
   planned removal version.
4. README, wiki, manpages, snippets, and AI index stop teaching the old surface
   before removal.
5. Smoke or sync tests protect the replacement and prevent stale examples from
   reappearing.
6. Actual removal waits for the next major version unless the old behavior is
   unsound, unsafe, or impossible to execute correctly on supported hosts.

Security fixes and correctness fixes may reject previously accepted invalid
graphs sooner, but release notes must call out the behavior change and tests
must cover the new diagnostic.

## Diagnostic And Schema Policy

- Diagnostic codes are compatibility anchors; wording may improve, but the code
  meaning should not drift silently.
- JSON output intended for tools must carry or document a schema identity when
  possible.
- New JSON fields are allowed. Removing or renaming fields requires the breaking
  change process.
- Human text output can be clarified, but documented progress contracts and
  machine-readable modes must remain stable.

## Release Gate

Before a QStar v1 release candidate, maintainers must verify:

- this document is linked from README, README.ko, docs README, wiki, manpages,
  and AI index;
- `docs/qstar-v1-readiness.md` treats compatibility as a sealed policy with
  ongoing enforcement, not an undefined blocker;
- smoke tests guard the stable policy document and removed legacy boundaries;
- release notes mention whether the compatibility policy changed;
- `git diff --check`, wiki/CLI sync, smoke tests, and the release gate pass.
