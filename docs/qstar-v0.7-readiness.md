# QStar 0.7 / v1 Readiness Gate

이 문서는 QStar의 다음 공개 라인을 `0.6.x-beta` patch로 유지할지, `0.7.0-beta` feature
line으로 올릴지 판단하기 위한 Q161 readiness gate다.

```txt
status: 0.7 readiness gate
current runtime version: qstar 0.7.0-beta
candidate line: qstar 0.7.0-beta
baseline date: 2026-06-14
decision: next feature line should be 0.7.0-beta; keep 0.6.x for hotfixes only
```

## Verdict

다음 큰 공개 라인은 `0.7.0-beta`가 맞다.

판단:

- `0.7.0-beta`는 macOS arm64와 Linux x86_64 public beta asset을 이어받는 release-prep line이다.
- `0.6.x-beta`는 release smoke, checksum, install/codesign, 문서 오탈자 같은 patch에 남긴다.
- Q160 이후 Windows artifact policy가 별도 문서로 고정되었고, 다음 작업은 단순 patch보다
  Windows alpha와 platform matrix를 한 단계 올리는 feature line에 가깝다.
- Stella daemon은 beta opt-in으로는 쓸 수 있지만 default-on으로 올리려면 lifecycle,
  security, Linux daemon CI, stable protocol decision이 더 필요하다.
- Stella executor와 Ninja backend는 medium corpus뿐 아니라 Q166 large synthetic corpus에서도
  같은 급의 수치를 보인다.
- v1.0은 아직 아니다. v1.0은 macOS/Linux/Windows official support와 release/CI matrix가
  모두 닫힌 뒤에만 붙인다.

## Platform Status

| Host | Current status | 0.7 target | v1 requirement |
| --- | --- | --- | --- |
| macOS arm64 | Public beta asset | 계속 primary release host | Official release asset, install smoke, codesign smoke, CI/release lane |
| Linux x86_64 | Public beta asset | asset gate와 CI artifact 안정화 | Official release asset, gcc/clang CI, install/docs/man smoke, perf artifact |
| Windows | Manual native CI alpha, no asset | native build failure list를 줄이고 artifact policy를 실제 CI로 검증 | Official source build, install smoke, `.exe`/`.lib`/`.dll` policy, release asset |

현재 Windows alpha의 핵심 known issue는 Unix socket 기반 Stella daemon code가 Windows source
build에서 portability boundary를 요구한다는 점이다. Windows path/process/response-file
계약과 artifact policy는 준비되어 있지만, official host support로 부르기에는 아직 이르다.

## Shared Library Status

| Target | macOS | Linux | Windows |
| --- | --- | --- | --- |
| `qstar.staticlib` | supported | supported | explicit `.lib` planning only |
| `qstar.sharedlib` | `.dylib` + `install_name` | `.so` + `soname` | deferred |
| sharedlib consumer rpath | `@loader_path` | `$ORIGIN` | deferred |
| install/stage | supported for current artifacts | supported for current artifacts | policy draft only |

Windows shared libraries need a multi-artifact model: runtime `.dll`, import `.lib`,
and optional PDB/debug output. Until that policy is implemented, Stella and Ninja reject
Windows-like `qstar.sharedlib` targets with a diagnostic that points to
`docs/windows-artifact-policy.md`.

Q168 seals the macOS/Linux sharedlib regression surface: sharedlib-linked
executables and tests must run from the build tree through the generated rpath,
Ninja and Stella must both preserve stage/install artifact handling, and
Windows-like sharedlib targets must keep the deferred diagnostic until the
multi-artifact policy is implemented.

## Daemon Status

Stella daemon is a documented beta opt-in workflow, not the default build path.

| Area | Current status | 0.7 decision |
| --- | --- | --- |
| Streaming output | implemented | keep parity with normal Stella output |
| In-memory dirty/deps state | implemented | keep report-only perf gate |
| File watcher invalidation | macOS/Linux MVP | strengthen Linux CI coverage |
| Read-only IDE API | implemented | keep read-only; no mutation API yet |
| Security hardening | owner/socket/root checks implemented | continue hardening before default-on |
| Lifecycle | `--start`, `--stop`, `--status` beta path | keep opt-in |
| Windows named pipe | deferred | not a 0.7 default blocker, but v1 blocker for daemon parity |

0.7 should not enable daemon by default. The 0.7 goal is to make daemon a better
beta service for Stella IDE/AI integration while preserving normal `qstar build`
as the conservative path.

Q167 adds a Linux opt-in daemon validation lane. It records Unix socket
validation, `inotify` watcher active/event traces, server logs, and skip/fail
reason artifacts under `dist/perf/linux-daemon-validation-*`.

## Stella/Ninja Timing Snapshot

Q162 local macOS arm64 `make check` snapshot recorded on 2026-06-14:

```txt
medium_project_gate target_count=47 min_targets=40
medium_project_gate scheduler host_jobs=10
medium_project_gate scheduler default_jobs=10 ready_width=40 async_final_actions=40 trace_elapsed_ms=277
medium_project_gate scheduler runner=posix_spawn event_wait=poll
medium_project_gate backend=stella phase=clean elapsed_ms=274
medium_project_gate backend=stella phase=noop elapsed_ms=79
medium_project_gate backend=stella phase=incremental elapsed_ms=131
medium_project_gate backend=stella-daemon phase=clean elapsed_ms=skipped reason=socket-bind-not-permitted
medium_project_gate backend=stella-daemon phase=noop elapsed_ms=skipped reason=socket-bind-not-permitted
medium_project_gate backend=stella-daemon phase=incremental elapsed_ms=skipped reason=socket-bind-not-permitted
medium_project_gate backend=stella-jobs jobs=10 phase=clean elapsed_ms=291
medium_project_gate backend=stella-jobs jobs=10 phase=noop elapsed_ms=79
medium_project_gate backend=stella-jobs jobs=10 phase=incremental elapsed_ms=115
medium_project_gate backend=ninja phase=clean elapsed_ms=297
medium_project_gate backend=ninja phase=noop elapsed_ms=86
medium_project_gate backend=ninja phase=incremental elapsed_ms=129
medium_project_gate status=ok perf_issue_count=0 report_only=1
```

Interpretation:

- Stella default is at Ninja-level latency on the medium corpus in this run.
- no-op and incremental are below 0.1s on this host.
- clean build is also in the same range as Ninja for the current corpus.
- Timing gates remain report-only because small projects are sensitive to host noise.
- Large synthetic corpus and Linux performance artifacts should continue to be used before making
  stronger performance claims.

## Large Stella/Ninja Timing Snapshot

Q166 local macOS arm64 repeat-3 large synthetic snapshot. Full summary is stored
in `docs/perf/q166-large-performance-refresh.md`.

| Mode | Backend | Clean median ms | No-op median ms | Incremental median ms |
| --- | --- | ---: | ---: | ---: |
| 200 targets | Stella | 1054 | 80 | 137 |
| 200 targets | Stella daemon | 1389 | 95 | 129 |
| 200 targets | Ninja | 1220 | 102 | 154 |
| 500 targets | Stella | 2437 | 101 | 145 |
| 500 targets | Stella daemon | 2601 | 107 | 151 |
| 500 targets | Ninja | 2816 | 144 | 228 |

Interpretation:

- Normal Stella is at or ahead of Ninja on this local repeat-3 large corpus.
- Stella daemon does not win clean builds here, but its no-op and incremental path remains in
  the same latency band and is still useful for future IDE/build-service workflows.
- Timing is report-only. The large corpus shows host noise, so 0.7 should cite these as
  representative local medians, not stable performance guarantees.

## 0.7 Scope

Put into `0.7.0-beta`:

- Windows native alpha failure list reduction.
- Windows artifact policy follow-through in tests and CI.
- Linux asset gate hardening and post-release download smoke refresh.
- Daemon lifecycle/security hardening.
- Daemon Linux socket/watcher CI lane, if stable enough.
- Sharedlib macOS/Linux regression seal and clearer Windows deferred diagnostics.
- Performance report refresh across medium, large, Stella, Stella daemon, and Ninja.
- Documentation drift seal for platform/release status.

Keep out of `0.7.0-beta`:

- Windows public release asset unless native source build and install smoke are green.
- Daemon default-on behavior.
- Stable daemon protocol version promise.
- Remote daemon access.
- Windows named pipe daemon.
- Cale Ninja wrapper lowering while Cale language/provider contracts are still moving.
- Package resolver, lockfile, registry, and fetch policy.
- VSCode Marketplace publication.

## v1.0 Blockers

QStar should not be called `1.0` until these are true:

- macOS arm64, Linux x86_64, and Windows official host support are validated.
- Public release assets exist for all official hosts, with SHA256 and install smoke.
- CI/release matrix covers macOS, Linux, and Windows build/test/install paths.
- Windows artifact policy is implemented for executable, static `.lib`, runtime `.dll`,
  import `.lib`, and debug artifact behavior.
- Shared library policy is stable across official hosts.
- Stella executor and Ninja backend have documented support boundaries and parity gates.
- Daemon is either explicitly stable or clearly excluded from default/stable support.
- Docs/wiki/manpage/AI_INDEX/help/snippets are drift-guarded for the stable surface.
- Self-host remains green while Makefile stays canonical bootstrap/release path.
- Security-sensitive behavior such as daemon sockets, path validation, and process spawning has
  platform-specific regression coverage.

## Release Gate For 0.7 Candidate

Before creating a `v0.7.0-beta` tag:

```sh
make all
make check
make qstar-self-host-tests
make qstar-medium-project-readiness-tests
make qstar-large-project-performance-tests
make qstar-public-beta-release-tests
git diff --check
./build/bin/qstar --version
```

On Linux:

```sh
make all
make check
make qstar-linux-validation-tests
make qstar-ninja-backend-parity-tests
QSTAR_RELEASE_PLATFORM=linux-x86_64 QSTAR_RELEASE_TAG=v0.7.0-beta \
  tools/package-public-beta.sh
```

On Windows alpha:

```sh
make all CC=gcc
build/bin/qstar --version
make qstar-windows-native-alpha-tests CC=gcc
make qstar-windows-prep-tests CC=gcc
```

## Release Line Decision

Use:

- `0.6.x-beta`: patch-only release smoke, checksum, install, documentation hotfixes.
- `0.7.0-beta`: Windows alpha/platform policy, daemon readiness, performance/platform summary
  updates.
- `1.0.0`: all official OS support and stable release matrix.
