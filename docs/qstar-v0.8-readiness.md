# QStar 0.8 / v1 Roadmap Gate

이 문서는 `v0.7.0-beta` 공개 이후 다음 feature line을 `0.7.x` patch로 유지할지,
`0.8.0-beta`로 열지 판단하기 위한 Q177 roadmap gate다.

```txt
status: 0.8 roadmap gate
current public line: qstar 0.7.0-beta
candidate feature line: qstar 0.8.0-beta
baseline date: 2026-06-15
decision: open 0.8 for Windows beta path and Windows sharedlib implementation; keep daemon opt-in
```

## Verdict

다음 feature line은 `0.8.0-beta`로 연다. 0.7에서 macOS arm64와 Linux x86_64 public
beta asset, macOS/Linux sharedlib, Stella/Ninja performance gate, daemon beta opt-in,
Windows manual alpha까지 들어왔기 때문에, 0.8은 더 큰 문법 추가보다 platform matrix를
정식 release 후보 쪽으로 당기는 라인이어야 한다.

우선순위:

1. Windows beta path를 연다.
2. Windows sharedlib `.dll`/import `.lib` multi-artifact model을 구현한다.
3. Windows process/event runner portability를 닫는다.
4. daemon은 default-on이 아니라 "default 준비" 수준으로 보안/수명주기/CI 이력을 쌓는다.
5. Linux asset은 이미 public beta asset이므로 0.8에서는 유지보수와 hosted verification
   freshness를 맡긴다.
6. package resolver, registry, lockfile, fetch policy는 계속 QStar core 밖에 둔다.

0.8은 QStar를 "macOS/Linux에서 쓸 수 있는 beta build system"에서
"Windows beta support를 실제로 닫기 시작한 cross-host build system"으로 옮기는 라인이다.
단, v1.0은 아니다. v1.0은 Windows official release asset과 all-host release/CI matrix가
반복 green으로 닫힌 뒤에만 붙인다.

## 0.7 Result Summary

| Area | 0.7 result | 0.8 implication |
| --- | --- | --- |
| macOS arm64 | Public beta asset | 유지, release smoke/codesign fresh run만 patch line에서 보강 |
| Linux x86_64 | Public beta asset published from hosted Ubuntu lane | 0.8에서도 release-backed beta host로 유지 |
| Windows | Manual native CI alpha, no asset | 0.8의 primary workstream |
| Stella executor | Medium/large corpus에서 Ninja급 timing report | 계속 성능 gate freshness 유지 |
| Ninja backend | C/C++/ASM/custom/configure/run/group/sharedlib parity candidate | Windows artifact parity로 확장 |
| Sharedlib | macOS `.dylib`, Linux `.so` sealed | Windows `.dll`/import `.lib` 구현 필요 |
| Stella daemon | documented beta opt-in, lifecycle/security hardening | default 준비는 하되 default-on 금지 |
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

## Windows Alpha Status

Windows는 아직 official support가 아니다.

현재 기준:

| Item | Status |
| --- | --- |
| Workflow | `.github/workflows/windows-validation.yml`, manual `workflow_dispatch` |
| Baseline lane | MSYS2 UCRT64 gcc |
| Public asset | none |
| Native source build | alpha failure list 관리 중 |
| Current known native blocker | repeat hosted Windows alpha after Q222 install/stage layout expansion |
| Daemon transport | Windows named pipe deferred |
| `.exe`/static `.lib` | contract and local fake-tool regression sealed |
| Execution corpus | MSYS2 UCRT64 GCC build/run/install baseline added in Q178; Stella CreateProcess runner added in Q220; Ninja execution path added in Q221; install/stage layout evidence added in Q222 |
| `.dll`/import `.lib` | implementation deferred |

0.8의 첫 번째 목표는 Windows를 "manual alpha"에서 "validation-backed beta candidate"로
올리는 것이다. 이 말은 곧바로 Windows asset을 공개한다는 뜻이 아니다. 먼저 다음이 필요하다.

- Q220의 Stella CreateProcess runner를 hosted Windows alpha에서 재검증
- Q221의 Ninja launcher parity를 같은 platform process layer 위에서 검증
- Q222의 `.exe`/static archive/generated object bridge install-stage layout을 hosted
  Windows alpha에서 재검증
- MSYS2 UCRT64 lane이 반복 green이거나, 실패하더라도 structured known issue만 남기는 상태
- `.exe`, static archive, response-file, generated object bridge, install smoke를 real Windows
  host에서 통과
- install/stage layout이 Windows host에서 실제로 동작
- Windows sharedlib diagnostic에서 실제 `.dll`/import `.lib` implementation으로 이동

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
- Windows source build, install smoke, release asset, artifact policy 구현
- CI/release matrix가 macOS, Linux, Windows를 모두 커버

### P0: Windows Artifact Implementation

- executable `.exe` real host validation
- static `.lib` real archive tool validation
- shared runtime `.dll` primary artifact
- import `.lib` link/interface artifact
- `qstar.target_file(label, { artifact = "import_lib" })` 또는 동등한 selector 구현
- stage/install layout: `.exe`/runtime `.dll` in `bin`, static/import `.lib` in `lib`
- PDB/debug artifact는 stable opt-in 전까지 implicit install 금지

### P1: Backend Parity And Performance

- Stella와 Ninja가 Windows artifact map을 같은 방식으로 계산
- Ninja root pollution 방지 유지
- medium/large performance gate freshness 유지
- daemon performance는 release input이지 stable guarantee가 아님을 유지

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
- Windows native alpha failure reduction
- Windows `.exe`/static `.lib` real-host validation
- Windows sharedlib `.dll` + import `.lib` Graph IR/action model
- Stella/Ninja Windows sharedlib parity tests
- Windows install/stage layout smoke for `.exe`, static archive, and generated object bridge
- daemon lifecycle/security regression 강화
- Linux/macOS release-backed artifact freshness checks
- QStar docs/wiki/manpage/AI_INDEX platform status refresh

0.8에 넣지 않을 것:

- daemon default-on
- Windows public asset without native build/install/package gate
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
| Q178-Q181 | Windows execution corpus and process/event portability | MSYS2 alpha reaches past executor `<poll.h>` and Stella CreateProcess execution, then records the next native Windows blocker |
| Q182-Q185 | Windows `.dll`/import `.lib` implementation | Stella/Ninja sharedlib parity fixture green |
| Q186-Q188 | Windows install/stage and native smoke | `.exe`, static `.lib`, runtime `.dll`, import `.lib` layout smoke green |
| Q189-Q190 | Daemon default-prep hardening | daemon remains opt-in; security/lifecycle report refreshed |
| Q191 | 0.8 beta readiness seal | platform/perf/docs/release gates summarized |

## 0.8 Release Draft Gate

Before tagging a future `v0.8.0-beta`:

```sh
make all
make check
make qstar-self-host-tests
make qstar-generic-dsl-backend-parity-tests
make qstar-medium-project-readiness-tests
make qstar-large-project-performance-tests
make qstar-public-beta-release-tests
git diff --check
./build/bin/qstar --version
```

Linux hosted validation:

```sh
make all
make check
make qstar-linux-validation-tests
make qstar-ninja-backend-parity-tests
QSTAR_RELEASE_PLATFORM=linux-x86_64 QSTAR_RELEASE_TAG=v0.8.0-beta \
  tools/package-public-beta.sh
```

Windows alpha/beta candidate:

```sh
make all CC=gcc
build/bin/qstar --version
make qstar-windows-native-alpha-tests CC=gcc
make qstar-windows-execution-corpus-tests CC=gcc
make qstar-windows-prep-tests CC=gcc
```

If Windows sharedlib lands in 0.8, add a named Windows artifact parity gate rather than hiding it
inside the generic smoke.

## Release Line Decision

Use:

- `0.7.x-beta`: release/package/docs/perf gate hotfixes only.
- `0.8.0-beta`: Windows beta path, Windows sharedlib implementation, daemon default-prep hardening.
- `1.0.0`: macOS/Linux/Windows official support and stable release matrix.
