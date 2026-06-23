# QStar v1 Readiness Gap Report

이 문서는 QStar 1.0을 공개하기 전에 남은 gap을 판단하기 위한 기준 문서다.
목적은 "언제 1.0을 붙일 수 있는가"와 "왜 아직 붙이면 안 되는가"를 감이 아니라
checklist로 고정하는 것이다.

```txt
status: v1 readiness gaps defined
current public line: qstar 0.7.19-beta
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

- stable DSL compatibility policy는 `docs/qstar-compatibility-policy.md`에
  봉인되었지만, v1 release line에서 README/wiki/man/smoke/release note까지 반복 적용된
  적은 아직 없다.
- Windows는 validation-backed beta candidate지만, GitHub Release에 게시된 official
  Windows asset과 downloaded-asset smoke는 Q253 opt-in publication gate가 green으로 남긴
  evidence가 있을 때만 조건부 해소된다.
- daemon은 documented beta opt-in이며, default-on이나 stable protocol promise가 아니다.
- GLP consumer surface와 standard provider consumer contract는 stable 후보로 정리됐고,
  provider-author API는 `qstar.lang/1` versioned beta contract로 남겨졌다.
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

The canonical policy is `docs/qstar-compatibility-policy.md`. Q256 sealed the
stable-at-v1 candidate list, beta opt-in surfaces, out-of-core boundaries,
legacy hard-cut rules, post-v1 deprecation window, diagnostic period, and release
note obligations.

QStar 0.x may continue to hard-cut legacy beta syntax when doing so protects the
generic build-system boundary. That policy ends at v1. After v1, a stable
surface cannot be removed inside the same major version without the documented
replacement, diagnostic period, release notes, docs/wiki/man/snippet updates,
and smoke guard described in the compatibility policy.

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
| Windows x86_64 | Validation-backed beta candidate with release-backed evidence. The v0.7.19-beta Windows workflow run published `qstar-v0.7.19-beta-windows-x86_64.zip` to GitHub Release and download-smoked it from the uploaded asset. Evidence: https://github.com/deeyed/qstar/actions/runs/27935992747. | Windows remains beta until repeated release gates are clean on the next beta/RC tag and the v1 candidate tag, but the former release-asset blocker has concrete evidence: `windows-hosted-release-decision.txt` recorded `windows_release_asset status=published` and `download_smoke=ok`, and the downloaded smoke verified `qstar --version`, docs/man lookup, provider vendoring, `qstar init app`, Stella build, and Ninja build. |

Official host support means all required artifacts are release-backed, not merely
local or candidate artifacts. Windows now has one release-backed beta evidence
run on `v0.7.19-beta`, but it remains beta until the same gate is repeated for
the next beta/RC tag and the v1 candidate tag, and the remaining
platform/daemon/compatibility blockers are resolved.

## Windows Release Gate Repetition

The `v0.7.19-beta` Windows publish/download smoke is seed evidence, not final v1
closure. The next beta or release-candidate tag must repeat the same
release-mutating workflow before this evidence can be treated as durable.

After the next beta/RC tag and GitHub Release exist, run:

```sh
gh workflow run windows-validation.yml \
  --ref <tag> \
  -f release_tag=<tag> \
  -f publish_windows_asset=true
```

The run counts as repeated Windows release evidence only when the
`qstar-windows-x86_64-published-release-asset` artifact contains
`windows-hosted-release-decision.txt` with:

```txt
windows_release_asset status=published
download_smoke=ok
```

The downloaded smoke must continue to cover the uploaded zip checksum,
`qstar --version`, docs/man lookup, bundled provider vendoring, `qstar init app`,
Stella build, and Ninja build from the extracted release tree. A plain
`windows-validation.yml --ref main` freshness run is useful, but it does not
replace this release-backed repetition gate because it does not mutate and then
re-consume a GitHub Release asset.

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

## GLP Provider API Boundary

GLP already makes external languages feel close to built-in C/C++/ASM from the
user side. Q257 resolves the provider-author question by keeping provider
authoring in the versioned beta bucket while preserving consumer-facing GLP as a
v1 stable candidate.

Consumer-facing v1 candidates:

- `qstar.use_language`
- `lang.<namespace>` validated option tables
- provider helper exports such as `zig.tools`, `zig.options`, `zig.object`
- raw source classification through activated provider source suffixes
- provider final artifact selection for pure provider targets
- `qstar init --use-language` vendoring from installed standard providers
- standard `zig`, `rust`, and `cuda` short ids, namespaces, documented options,
  helper exports, and source suffixes

Provider-author beta surface:

- `qstar.language_provider { api = "qstar.lang/1", ... }`
- manifest fields `id`, `version`, `namespace`, `implementation`, `tools`,
  `units`, `finals`, `options`, `exports`, `scaffold`
- implementation helpers `qstar.provider_tools`, `qstar.language_options`,
  `qstar.source`, `qstar.argv`
- provider implementation sandbox capabilities
- lowering result fields `command`, `env`, `inputs`, `outputs`, `depfile`
- `qstar.scaffold/1` provider scaffold metadata

QStar accepts only `api = "qstar.lang/1"` today. Future provider manifest API
versions are rejected instead of guessed, and must be introduced by a separate
versioned design.

Provider-author API can become stable in a later release only when:

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
- Compatibility/removal policy is linked from README, wiki, manpages, release notes, and AI index.
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
| Windows official artifact | v1 promises all three OSes. This blocker has v0.7.19-beta evidence, but must be repeated on the next beta/RC tag and again on the final v1 candidate tag. | Q254 evidence exists: `windows-validation.yml` run 27935992747 published the Windows zip and produced `windows-hosted-release-decision.txt` with `windows_release_asset status=published` and `download_smoke=ok`. Repeat the same release-mutating gate with `publish_windows_asset=true`; a non-publishing freshness run is not enough. |
| Stable compatibility policy enforcement | Q256 defines the promise, but v1 must keep it synced across every public reference and release note. | `docs/qstar-compatibility-policy.md` is the canonical policy. Keep README/wiki/man/AI index links and smoke guards green through the v1 release branch. |
| Daemon boundary | daemon is beta opt-in and not default/stable. | Keep daemon beta in v1 docs, or finish separate stable daemon gate. |
| Release matrix repetition | One fresh run is not enough for v1 confidence. | Repeat macOS/Linux/Windows release gates on final release branch/tag. |
| Package manager boundary | Users may expect dependency resolution if not explicit. | Keep registry/resolver/lockfile/fetch policy out-of-core in docs and release notes. |

## Non-Blockers

These are allowed to remain outside v1 if documented:

- daemon default-on behavior
- remote daemon access
- provider-author API stable promotion, because Q257 leaves `qstar.lang/1` in the
  documented beta bucket while keeping consumer GLP as a stable candidate
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
