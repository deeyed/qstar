# QStar 0.8 Beta Readiness Seal

이 문서는 `v0.7.x-beta` 이후의 0.8 feature line을 Windows beta candidate와
0.8.0-beta 준비 상태로 봉인하기 위한 Q225 readiness gate다.

```txt
status: 0.8 beta readiness sealed
current public line: qstar 0.7.14-beta
candidate feature line: qstar 0.8.0-beta
baseline date: 2026-06-17
decision: seal Windows as validation-backed beta candidate; keep daemon opt-in; keep package resolver out-of-core
```

## Verdict

다음 feature line은 `0.8.0-beta`로 준비한다. 0.7에서 macOS arm64와 Linux x86_64 public
beta asset, macOS/Linux sharedlib, Stella/Ninja performance gate, daemon beta opt-in,
Windows manual alpha까지 들어왔고, Q218-Q224에서 Windows execution, Ninja launcher,
install/stage, sharedlib artifact lowering이 닫혔다. Q225 기준 0.8의 핵심 판단은
Windows를 official support라고 부르지 않으면서도 "validation-backed beta candidate"라고
부를 수 있는가다. 현재 답은 yes다.

우선순위:

1. Windows beta candidate path를 검증 가능한 상태로 봉인한다.
2. Windows sharedlib `.dll`/import `.lib` multi-artifact model을 Stella/Ninja 양쪽에서 유지한다.
3. Windows process/event runner portability를 CreateProcess platform layer 위에 둔다.
4. daemon은 default-on이 아니라 "default 준비" 수준으로 보안/수명주기/CI 이력을 쌓는다.
5. Linux asset은 이미 public beta asset이므로 0.8에서는 유지보수와 hosted verification
   freshness를 맡긴다.
6. package resolver, registry, lockfile, fetch policy는 계속 QStar core 밖에 둔다.

0.8은 QStar를 "macOS/Linux에서 쓸 수 있는 beta build system"에서
"Windows beta path를 검증 가능한 artifact와 gate로 가진 cross-host build system"으로
옮기는 라인이다.
단, v1.0은 아니다. v1.0은 Windows official release asset과 all-host release/CI matrix가
반복 green으로 닫힌 뒤에만 붙인다.

## 0.7 Result Summary

| Area | 0.7 result | 0.8 implication |
| --- | --- | --- |
| macOS arm64 | Public beta asset | 유지, release smoke/codesign fresh run만 patch line에서 보강 |
| Linux x86_64 | Public beta asset published from hosted Ubuntu lane | 0.8에서도 release-backed beta host로 유지 |
| Windows | Validation-backed beta candidate, Actions public beta candidate zip build/extract smoke, opt-in GitHub Release publish/download gate | official support 전 반복 검증 대상 |
| Stella executor | Q250 medium/large refresh에서 report-only timing gate green; large 500 normal Stella clean은 Ninja보다 느렸지만 daemon/no-op/incremental은 우세 | 계속 성능 gate freshness 유지 |
| Ninja backend | C/C++/ASM/custom/configure/run/group/sharedlib parity candidate | Windows artifact parity로 확장 |
| Sharedlib | macOS `.dylib`, Linux `.so`, Windows `.dll`/import `.lib` sealed | PDB/debug ownership은 deferred |
| Stella daemon | documented beta opt-in, lifecycle/security hardening, Q250 local daemon timing refresh | default 준비는 하되 default-on 금지 |
| Docs/wiki/man/AI_INDEX | 0.7 surface drift guard | 0.8 Windows/daemon/release status 추가 |

## Linux Asset Status

Linux x86_64는 `v0.7.0-beta` 기준 public beta release asset으로 본다.
`docs/linux-validation.md`와 `docs/releases/v0.7.0-beta.md`에 따르면 Q171 hosted
verification은 Ubuntu gcc/clang validation, large performance report, Linux asset publish,
uploaded asset download smoke를 통과했고, decision artifact는 다음 상태를 기록한다.

```txt
linux_release_asset status=published
tag=v0.7.0-beta
platform=linux-x86_64
download_smoke=ok
artifact=qstar-v0.7.0-beta-linux-x86_64.tar.gz
```

0.8에서 Linux는 새 기능의 주 대상이 아니라 release-backed beta host 유지 대상이다.
필수 작업은 다음 정도로 제한한다.

- gcc/clang lane green 유지
- uploaded asset download smoke fresh run
- medium/large performance artifact format 유지
- daemon socket smoke는 opt-in lane으로 유지
- Linux daemon behavior를 release-backed라고 쓰려면 `daemon_socket_smoke=true` run이 green이어야 함

## Windows Beta Candidate Status

Windows는 아직 official support가 아니다.

현재 기준:

| Item | Status |
| --- | --- |
| Workflow | `.github/workflows/windows-validation.yml`, manual `workflow_dispatch` |
| Baseline lane | MSYS2 UCRT64 gcc |
| Public beta candidate asset | `qstar-v<version>-windows-x86_64.zip` candidate artifact built, extracted, and smoke-tested in Actions |
| Release publication | Q253 opt-in `publish_windows_asset=true` job uploads and download-smokes the Windows zip |
| Native source build | beta candidate lane에서 `make all CC=gcc` 검증 |
| Current known native blocker | no known blocker in the sealed candidate contract; future failures must be recorded as structured artifacts |
| Daemon transport | Windows named pipe deferred |
| `.exe`/static `.lib` | contract, local fake-tool regression, native execution/install evidence sealed |
| Execution corpus | MSYS2 UCRT64 GCC build/run/install baseline added in Q178; Stella CreateProcess runner added in Q220; Ninja execution path added in Q221; install/stage layout evidence added in Q222 |
| `.dll`/import `.lib` | Graph IR artifact map and selector sealed in Q223; Stella/Ninja lowering sealed in Q224 |
| Status artifact | `qstar-windows-beta-candidate`, `windows-beta-candidate-status.txt`, `KNOWN_ISSUES.md`, failure detail dirs |

0.8의 첫 번째 목표였던 "manual alpha"에서 "validation-backed beta candidate"로의 이동은
Q225에서 문서/게이트/Actions artifact 기준으로 닫힌다. Q246은 여기서 한 단계 더 나아가
Windows public beta asset을 만들기 위한 package skeleton을 뒀고, Q247은 실제 Windows zip
asset candidate를 생성해 추출 smoke까지 수행한다. 이 asset은 public beta candidate로
준비/검증된 Actions artifact다. Q253은 여기서 한 단계 더 나아가 release tag에 대해
`publish_windows_asset=true`를 켰을 때 GitHub Release upload, `SHA256SUMS` merge,
uploaded zip download smoke까지 수행하는 opt-in publication gate를 둔다. 그래도 official
Windows support는 해당 hosted decision artifact가 green일 때만 판단한다. 현재 beta
candidate가 보장하는 것은 다음이다.

- Q220의 Stella CreateProcess runner가 hosted Windows lane에서 검증된다.
- Q221의 Ninja launcher parity가 같은 platform process layer 위에서 검증된다.
- Q222의 `.exe`/static archive/generated object bridge install-stage layout이 hosted
  Windows lane에서 검증된다.
- Q224의 runtime `.dll`/import `.lib` multi-output lowering, selector, dry-run/stage/install
  layout이 named sharedlib artifact parity gate로 검증된다.
- 실패가 발생하면 `native-alpha-detail/`, `windows-execution-detail/`,
  `windows-prep-detail/`, `windows-sharedlib-detail/` 중 해당 detail bundle이 남는다.
- `.exe`, static archive, response-file, generated object bridge, install smoke가 real
  Windows host에서 통과해야 beta candidate fresh run으로 인정된다.
- install/stage layout은 Windows host에서 실제 파일 layout과 slash-normalized manifest를
  함께 검증한다.
- Windows sharedlib `.dll`/import `.lib` backend lowering은 Stella/Ninja 양쪽의
  action-log, consumer import-library link, stage/install layout으로 검증한다.
- Windows release asset smoke는 `qstar-v<version>-windows-x86_64.zip`을 실제 생성하고,
  zip을 추출한 뒤 `qstar --version`, docs lookup, provider vendoring, `qstar init app`,
  Stella/Ninja 최소 build를 extracted `bin/qstar.exe`로 검증한다.
- Windows release publication smoke는 opt-in job에서 uploaded zip을 다시 내려받고 같은
  docs/provider/init/Stella/Ninja 검증을 release URL 기준으로 반복한다.

## Daemon Beta Status

Stella daemon은 documented beta opt-in 상태를 유지한다.

| Area | Current status | 0.8 decision |
| --- | --- | --- |
| CLI lifecycle | `qstar daemon --start|--stop|--status` beta path | 유지 및 hardening |
| Build usage | `qstar build --use-daemon=auto|never|always` explicit opt-in | default-on 금지 |
| Streaming output | implemented | normal Stella output parity 유지 |
| In-memory state | implemented | crash recovery/writeback regression 유지 |
| Watcher | macOS `kqueue`, Linux `inotify` MVP | Linux hosted trace history 확보 |
| Read API | read-only IDE/AI methods | mutation API 금지 |
| Security | owner/socket/root mismatch checks | repeated regression 강화 |
| Windows | named pipe deferred | v1 blocker, 0.8 optional design only |

0.8에서 daemon은 "Stella IDE가 붙을 build service"로 계속 키우되, 일반 사용자의 기본
`qstar build` path를 대체하지 않는다. Default-on 결정은 다음 조건 전까지 금지한다.

- Linux daemon socket/watcher CI가 반복 green
- stale daemon cleanup과 package-root mismatch hard reject가 release마다 유지
- protocol versioning이 stable/beta boundary를 명확히 가짐
- Windows named pipe transport 방향이 최소 design gate를 통과
- daemon disabled/fallback path가 normal Stella와 항상 같은 결과를 냄

## v1.0 Blockers Reordered

v1.0 blocker를 0.8 기준으로 다시 정렬한다.

### P0: Official Host Matrix

- macOS arm64 public asset, install smoke, codesign smoke 유지
- Linux x86_64 public asset, hosted download smoke, gcc/clang validation 유지
- Windows source build, install smoke, public beta candidate asset, artifact policy 반복 검증
- Windows Actions zip asset artifact와 Q253 GitHub Release upload/download smoke evidence
- CI/release matrix가 macOS, Linux, Windows를 모두 커버

### P0: Windows Artifact Implementation

- executable `.exe` real host validation
- static `.lib` real archive tool validation
- shared runtime `.dll` primary artifact
- import `.lib` link/interface artifact
- `qstar.target_file(label, { artifact = "import_lib" })` selector 구현
- stage/install dry-run layout: `.exe`/runtime `.dll` in `bin`, static/import `.lib` in `lib`
- Windows sharedlib backend lowering: Stella/Ninja가 runtime `.dll`과 import `.lib`를 같은
  final action output set으로 생산하고 consumer가 import `.lib`를 link input으로 사용
- PDB/debug artifact는 stable opt-in 전까지 implicit install 금지

### P1: Backend Parity And Performance

- Stella와 Ninja가 Windows artifact map을 같은 방식으로 계산
- Ninja root pollution 방지 유지
- medium/large performance gate freshness 유지
- daemon performance는 release input이지 stable guarantee가 아님을 유지
- Q250 이후 performance freshness 정본은
  `docs/perf/q250-v0.8-backend-performance-refresh.md`에 둔다. Real Rust/Zig GLP compiler corpus는
  backend timing 수치에 섞지 않고 correctness gate로 분리한다.

### P1: Daemon Default Decision

- default-on은 0.8 목표가 아니다.
- default 준비를 위해 security/lifecycle/CI history를 쌓는다.
- Windows named pipe 없이는 v1 daemon parity를 주장하지 않는다.

### P1: Documentation And Tooling Drift

- README/wiki/manpage/AI_INDEX/help/snippets가 stable surface와 일치
- release note와 platform table이 asset reality와 일치
- GitHub Wiki sync checklist 유지

### P2: Non-Core Package Management

- package resolver, registry, lockfile, fetch policy는 v1 blocker가 아니다.
- QStar core는 package manager가 아니라 build graph/executor/toolchain surface다.
- resolver를 추가하더라도 별도 tool/layer로 설계하고 mandatory config path를 되살리지 않는다.

## 0.8 Feature Scope

0.8에 넣을 것:

- Windows process/event runner boundary
- Windows native beta candidate failure artifact contract
- Windows public beta package skeleton: `windows-x86_64` platform, zip naming,
  `.exe` runtime layout, docs/man/wiki/provider inclusion policy
- Windows public beta asset smoke: actual zip creation, extracted binary docs,
  provider vendoring, `qstar init app`, and Stella/Ninja minimal corpus
- Windows public beta asset publication gate: opt-in upload/download smoke for
  `qstar-v<version>-windows-x86_64.zip`
- Windows `.exe`/static `.lib` real-host validation
- Windows sharedlib `.dll` + import `.lib` Graph IR/action model
- Stella/Ninja Windows sharedlib parity tests
- Windows install/stage layout smoke for `.exe`, static archive, generated object bridge, and sharedlib runtime/import artifacts
- daemon lifecycle/security regression 강화
- Linux/macOS release-backed artifact freshness checks
- QStar docs/wiki/manpage/AI_INDEX platform status refresh

0.8에 넣지 않을 것:

- daemon default-on
- Windows GitHub Release asset without native build/install/package gate and download smoke evidence
- package resolver, registry, lockfile, fetch policy
- mandatory `qstar.toml`, `project-specific TOML`, `.foreign/toolsets/*.toml`
- domain-specific builtin target
- external language Ninja wrapper lowering while external language/provider contracts are still moving
- remote daemon access
- VSCode Marketplace publication

## 0.7.x Patch Scope

0.7.x patch로 남길 것:

- release asset smoke hotfix
- SHA256SUMS/package script small fix
- docs/wiki/manpage typo or drift fix
- CI artifact naming fix
- Linux/macOS download smoke refresh
- performance gate threshold/format bugfix
- security diagnostic wording fix

0.7.x patch로 넣지 않을 것:

- new Windows artifact syntax
- daemon default behavior change
- new package resolver surface
- breaking authoring syntax change
- v1 stable promise

## Suggested 0.8 Workstream

| Round band | Focus | Exit gate |
| --- | --- | --- |
| Q218-Q222 | Windows beta candidate process, execution, Ninja, install/stage freshness | MSYS2 lane has source build, Stella/Ninja execution, install/stage evidence, and structured failure artifacts |
| Q223-Q224 | Windows `.dll`/import `.lib` implementation | Graph IR selector, Stella/Ninja multi-output lowering, consumer import-library link, stage/install layout green |
| Q225 | 0.8 beta readiness seal | docs/wiki/man/README/platform table, Actions artifact, named sharedlib parity gate, release gate summary aligned |
| Q246 | Windows package skeleton | `windows-x86_64` zip plan, `.exe`/docs/man/wiki/provider layout, dry-run workflow artifact aligned |
| Q247 | Windows asset smoke | actual `windows-x86_64` zip artifact, extracted package smoke, provider vendoring, init/build, Stella/Ninja minimal corpus |
| Q251 | 0.8 beta release candidate gate | local `make qstar-v0.8-release-tests` umbrella gate, published-asset download smoke opt-in, hosted Linux/Windows fresh-run commands aligned |
| Q253 | Windows official beta asset publication gate | opt-in `publish_windows_asset=true` job uploads Windows zip to GitHub Release, merges `SHA256SUMS`, downloads the zip, and records `windows_release_asset status=published` with `download_smoke=ok` |

## 0.8 Release Candidate Gate

Before tagging a future `v0.8.0-beta`, run the single local gate:

```sh
make qstar-v0.8-release-tests
```

This umbrella target expands to `make check`, generic DSL backend parity, optional
real GLP compiler corpus, real-language init scaffold validation, repeat
medium/large performance summaries, current-host public beta package smoke,
Windows package/asset contract smoke, `git diff --check`, and `qstar --version`.
It deliberately skips GitHub release download smoke until assets exist.
The Windows sharedlib artifact subset remains the named
`qstar-windows-sharedlib-artifact-parity-tests` gate inside the hosted Windows
candidate lane.

After GitHub Release upload, run the published-asset smoke explicitly:

```sh
QSTAR_RUN_RELEASE_DOWNLOAD_SMOKE=1 make qstar-v0.8-release-tests
```

Linux hosted fresh run:

```sh
gh workflow run linux-validation.yml \
  --ref main \
  -f release_tag=v0.8.0-beta \
  -f publish_linux_asset=false \
  -f daemon_socket_smoke=true
```

Only set `publish_linux_asset=true` after the release tag exists and release
upload is intentionally in scope.

Windows alpha/beta candidate fresh run:

```sh
gh workflow run windows-validation.yml --ref main
```

The optional `run_ninja_parity=true` input is diagnostic-only for 0.8. The beta
candidate gate already covers the Windows execution corpus with Stella and Ninja,
Windows prep, sharedlib artifact parity, install docs/man smoke, and release
asset smoke. The broader `qstar-ninja-backend-parity-tests` corpus still carries
POSIX-style executable runtime expectations that can be skipped on Windows.

To publish a Windows beta asset after a release tag exists, use:

```sh
gh workflow run windows-validation.yml \
  --ref v0.8.0-beta \
  -f release_tag=v0.8.0-beta \
  -f publish_windows_asset=true
```

That job is release-mutating and must not be used as an ordinary freshness run.

The Windows workflow uploads `qstar-windows-beta-candidate`, including
`windows-beta-candidate-status.txt`, `KNOWN_ISSUES.md`, and the failure detail
directories needed to debug the first failed step. Q247 also puts
`release-package/qstar-v<version>-windows-x86_64.zip`, `SHA256SUMS`, package
contents/plan files, and `release-asset-smoke/` extracted-package logs in the
workflow artifacts when the asset smoke succeeds.

## Release Line Decision

Use:

- `0.7.x-beta`: release/package/docs/perf gate hotfixes only.
- `0.8.0-beta`: Windows beta candidate path, Windows sharedlib implementation, daemon default-prep hardening.
- `1.0.0`: macOS/Linux/Windows official support and stable release matrix.
