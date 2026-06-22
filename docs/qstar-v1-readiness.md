# QStar v1 Readiness Gap Report

이 문서는 QStar 1.0을 공개하기 전에 남은 gap을 판단하기 위한 기준 문서다.
목적은 "언제 1.0을 붙일 수 있는가"와 "왜 아직 붙이면 안 되는가"를 감이 아니라
checklist로 고정하는 것이다.

```txt
status: v1 readiness gaps defined
current public line: qstar 0.7.11-beta
candidate feature line: qstar 0.8.0-beta
v1 decision: not ready
baseline date: 2026-06-21
```

## Verdict

QStar는 이미 beta build system으로는 꽤 넓은 표면을 갖췄다. Generic DSL hard cut,
GLP, root project command, generated artifact dependency closure, Windows sharedlib
artifact model, release package smoke, Linux/macOS package validation, Windows beta
candidate lane까지 들어왔다.

하지만 지금 `1.0.0`을 붙이면 안 된다. 아직 남은 gap은 다음이다.

- stable DSL compatibility policy가 release line에 공식으로 적용된 적이 없다.
- Windows는 validation-backed beta candidate지만, GitHub Release에 게시된 official
  Windows asset과 downloaded-asset smoke는 Q253 opt-in publication gate가 green으로 남긴
  evidence가 있을 때만 조건부 해소된다.
- daemon은 documented beta opt-in이며, default-on이나 stable protocol promise가 아니다.
- GLP consumer surface는 강해졌지만, provider-author API와 standard provider compatibility
  policy는 v1 stable 약속으로 봉인되지 않았다.
- package manager, registry, lockfile, fetch policy는 계속 QStar core 밖에 둔다는 경계를
  v1 문서와 release note에서 반복 확인해야 한다.
- 성능 수치는 release input/report-only다. v1은 "항상 Ninja보다 빠름" 같은 마케팅 문구를
  약속하면 안 된다.

따라서 v1은 기능 추가 라운드가 아니라 blocker 제거 라운드로 열어야 한다.

## Stable DSL Candidate Surface

아래 표면은 v1에서 안정 표면으로 묶을 후보이며, v1 tag 전에 docs/wiki/man/snippet/smoke가
같은 목록을 가리켜야 한다.

### Graph Entrypoints

- `qstar.project`
- `qstar.toolset`
- `qstar.config`
- `qstar.executable`
- `qstar.staticlib`
- `qstar.sharedlib`
- `qstar.test`
- `qstar.custom_target`
- `qstar.transform`
- `qstar.configure_file`
- `qstar.run_target`
- `qstar.group`
- `qstar.stage`
- `qstar.target_family`
- `qstar.command`
- `qstar.subdir`
- `qstar.import_file`
- `qstar.import_module`
- `qstar.use_language`

### Command And Workflow Helpers

- `qstar.cli`
- `qstar.status`
- `qstar.input`
- `qstar.output`
- `qstar.target_file`
- `qstar.stage_dir`
- `qstar.stage_file`
- `qstar.param`
- `qstar.param.string`
- `qstar.param.path`
- `qstar.param.bool`
- `qstar.param.int`
- `qstar.param.enum`
- `qstar.param.list`
- `qstar.arg_if`
- `qstar.args_if`
- `qstar.step.build`
- `qstar.step.test`
- `qstar.step.stage`
- `qstar.step.check`
- `qstar.step.lint`
- `qstar.step.run`
- `qstar.step.call`
- `qstar.step.export_stage`

### Authoring Helpers

- `qstar.files`
- `qstar.join`
- `qstar.copy`
- `qstar.append`
- `qstar.merge`
- `qstar.extend`

### Language And Source Surface

Preloaded namespaces:

- `lang.c`
- `lang.cxx`
- `lang.asm`

Provider consumer surface:

- `qstar.use_language("id")`
- `lang.<namespace>` dynamic option tables after provider activation
- raw source string classification through activated provider `units.*.suffixes`
- exported provider helpers such as `zig.object(...)`
- provider final artifact lowering for pure provider `executable`, `staticlib`, and
  `sharedlib` targets

Provider authoring surface is not yet stable by default. It is listed here as a
v1 blocker because users can write providers today, but v1 must decide which parts
are stable.

- `qstar.language_provider`
- `qstar.provider_tools`
- `qstar.language_options`
- `qstar.source`
- `qstar.argv`
- provider manifest fields: `api`, `id`, `version`, `namespace`, `implementation`,
  `tools`, `units`, `finals`, `options`, `exports`, `scaffold`
- provider implementation result fields: `command`, `env`, `inputs`, `outputs`,
  `depfile`

## Compatibility And Removal Policy

QStar 0.x may continue to hard-cut legacy beta syntax when doing so protects the
generic build-system boundary. That policy ends at v1.

After v1:

- Stable DSL entries listed in this document must not be removed silently.
- Stable field removal requires a documented deprecation window and a migration
  diagnostic before hard removal.
- The minimum deprecation window is one minor feature line unless the syntax is
  a security issue or corrupts builds.
- Removed 0.x compatibility shims must not return as public APIs.
- New experimental APIs must be explicitly documented as experimental or beta and
  must not be mixed into the stable list by accident.
- Error message wording can improve, but machine-readable schema names and JSON
  fields that are documented as stable require compatibility notes.
- Generated build files, cache internals, and physical log file locations are not
  stable unless a document explicitly says they are.

For v1, every public reference must classify APIs into one of three buckets:

| Bucket | Meaning |
| --- | --- |
| Stable | Covered by v1 compatibility policy. |
| Beta opt-in | Documented and supported for testing, but no stable/default promise. |
| Out-of-core | Intentionally not QStar core. |

## OS Support Matrix

| Host | Current status | Official v1 condition |
| --- | --- | --- |
| macOS arm64 | Public beta release asset exists. Local package/download smoke and codesign checks are active. | Release asset must be produced from a clean tag, uploaded, downloaded, checksum-verified, docs/man/wiki smoke-tested, and `make qstar-v0.8-release-tests` or successor gate must pass on the release branch. |
| Linux x86_64 | Public beta release asset exists from hosted Ubuntu lane. gcc/clang source validation, Ninja parity, package dry-run, download smoke, and performance artifacts exist. | Ubuntu hosted release workflow or clean Linux host must produce the artifact. Uploaded asset must pass download smoke with `file`, `ldd`, docs/wiki/man checks, Ninja backend parity, install smoke, and medium performance artifact collection. |
| Windows x86_64 | Validation-backed beta candidate. MSYS2 UCRT64 lane builds source, runs execution/prep/sharedlib gates, creates/extracts zip candidate, and uploads artifacts. Q253 adds an explicit `publish_windows_asset=true` release publication/download-smoke job. | Windows GitHub Release zip must be published from the native workflow, downloaded again, checksum-verified, extracted, and smoke-tested with `qstar --version`, docs/man lookup, provider vendoring, `qstar init app`, Stella build, Ninja build, install/stage layout, and sharedlib runtime/import `.lib` consumer link. The blocker is conditionally closable when `windows-hosted-release-decision.txt` records `windows_release_asset status=published` and `download_smoke=ok` for the target release tag. |

Official host support means all required artifacts are release-backed, not merely
local or candidate artifacts. Windows remains beta until the Q253 release asset
publication and downloaded-asset smoke are both green for the selected release tag.

## Daemon Stable Conditions

The daemon remains beta opt-in. It is useful and validation-backed, but not yet
v1 stable.

Daemon can become stable only when all conditions hold:

- Normal Stella and daemon builds have parity tests for build result, action-log,
  replay, diagnostics, and read-only query output.
- Unix socket permission, owner mismatch, stale socket, stale pid, stale lock, and
  package/build identity mismatch regressions stay green.
- Linux `inotify` and macOS `kqueue` watcher behavior are repeatedly green in
  hosted or documented local gates.
- Windows named pipe support is implemented or Windows daemon support is clearly
  excluded from the stable matrix.
- The daemon read API has a versioned compatibility policy.
- Default `qstar build` behavior remains normal Stella unless a separate stable
  default-on decision document is written.
- Remote daemon access remains out of scope unless a security model exists.

Until then, daemon docs must keep the words beta opt-in and must not imply v1
stable/default behavior.

## GLP Stable Provider API Conditions

GLP already makes external languages feel close to built-in C/C++/ASM from the
user side. For v1, the remaining question is provider-author stability.

GLP can be called stable only when:

- `qstar.language_provider { api = "qstar.lang/1", ... }` has a frozen manifest
  schema or a documented version negotiation story.
- Provider `tools`, `units`, `finals`, `options`, `exports`, and `scaffold`
  diagnostics have stable wording enough for provider authors to debug.
- Provider implementation sandbox capabilities are documented as allowed and
  forbidden.
- Provider lowering result schema is frozen for Stella and Ninja parity:
  `command`, `env`, `inputs`, `outputs`, and `depfile`.
- Standard `zig`, `rust`, and `cuda` providers each have fake-tool CI and optional
  real-compiler validation paths.
- Raw source classification, explicit helper precedence, suffix collision
  diagnostics, and provider final artifact lowering are tested across Stella and
  Ninja.
- Provider scaffold output from `qstar init --use-language=...` is validated for
  app/lib/tool/empty/workspace shapes, or missing provider shapes emit documented
  fallback warnings.
- There is a compatibility policy for installed standard provider bundles and
  project-local provider override behavior.

Consumer-facing `qstar.use_language`, `lang.<namespace>`, raw source
classification, and provider helpers may be treated as v1 candidates. Provider
authoring should remain beta until the above checklist is sealed.

## Package Manager Boundary

QStar v1 must not accidentally become a package manager.

Out-of-core for v1:

- package registry
- package resolver
- dependency resolver
- lockfile format
- network fetch policy
- credential/auth storage
- vendored third-party package cache

In-core:

- project-local files
- installed standard provider bundles
- explicit `qstar.import_file` and cached `qstar.import_module`
- explicit language provider vendoring during `qstar init`
- explicit artifact transforms, stages, commands, and generated outputs

This boundary keeps QStar a generic build system rather than a package ecosystem
manager. A future package manager can exist next to QStar, but it needs its own
design, cache, provenance, and security policy.

## v1 Exit Gate

QStar can start a v1 release candidate only when this checklist is true:

- Stable DSL list in this document, `docs/README.md`, `docs/syntax.md`,
  `wiki/reference/qstar-lua.md`, manpages, snippets, and smoke guards match.
- Compatibility/removal policy is linked from README, wiki, release notes, and AI index.
- macOS arm64 release asset is uploaded and downloaded successfully.
- Linux x86_64 release asset is uploaded and downloaded successfully.
- Windows x86_64 release asset is uploaded and downloaded successfully.
- Windows sharedlib `.dll` plus import `.lib` consumer link remains green.
- `make check` passes locally.
- `make qstar-v0.8-release-tests` or successor v1 release gate passes locally.
- Linux hosted validation passes on gcc and clang.
- Windows beta/RC validation passes on the official native workflow.
- Daemon remains clearly beta opt-in, or a separate stable daemon gate has passed.
- GLP provider authoring is either stable by checklist or explicitly beta.
- Package manager/registry/fetch policy remains out-of-core in docs and release notes.
- `git diff --check` passes.

## Immediate Blockers

| Blocker | Why it blocks v1 | Required closure |
| --- | --- | --- |
| Windows official artifact | v1 promises all three OSes, but Windows is still candidate-only until the Q253 publication gate records release-backed evidence. | Run `windows-validation.yml` with `publish_windows_asset=true` for the release tag. The resulting artifact must include `windows-hosted-release-decision.txt` with `windows_release_asset status=published` and `download_smoke=ok`. |
| Stable compatibility policy | v1 needs a promise for removals and field stability. | Link this policy from README/wiki/man/release notes and add drift guards. |
| GLP provider-author stability | External language authors need a stable manifest/lowering contract. | Freeze or version `qstar.lang/1`, provider sandbox, lowering result, and provider scaffold contracts. |
| Daemon boundary | daemon is beta opt-in and not default/stable. | Keep daemon beta in v1 docs, or finish separate stable daemon gate. |
| Release matrix repetition | One fresh run is not enough for v1 confidence. | Repeat macOS/Linux/Windows release gates on final release branch/tag. |
| Package manager boundary | Users may expect dependency resolution if not explicit. | Keep registry/resolver/lockfile/fetch policy out-of-core in docs and release notes. |

## Non-Blockers

These are allowed to remain outside v1 if documented:

- daemon default-on behavior
- remote daemon access
- Windows PDB/debug artifact ownership
- MSVC bootstrap lane, if GCC/MSYS2 is the official Windows v1 compiler lane
- package manager, registry, lockfile, fetch
- VSCode Marketplace publication
- absolute performance superiority over Ninja

## Validation

For this document change:

```sh
make check
make qstar-wiki-cli-sync-tests
git diff --check
```

For a future v1 release candidate:

```sh
make qstar-v0.8-release-tests
gh workflow run linux-validation.yml --ref main
gh workflow run windows-validation.yml --ref main
gh workflow run windows-validation.yml --ref v<version> \
  -f release_tag=v<version> \
  -f publish_windows_asset=true
```

The exact target name may change to a v1-specific release gate later, but the
scope must remain at least as broad as the Q251 v0.8 gate.
