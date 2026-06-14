# Stella Daemon Beta Readiness Gate

이 문서는 Round Q151에서 Stella daemon을 계속 hidden experimental로 둘지, beta opt-in 기능으로
올릴지 판단하기 위한 readiness gate다. 여기서 `Stella daemon`은 Stella IDE가 아니라 QStar의
native executor를 장기 실행 build service로 붙이는 experimental daemon path를 뜻한다.

```txt
status: daemon beta opt-in readiness gate
current runtime version: qstar 0.6.0-beta
release line: qstar 0.6.0-beta
decision: documented beta opt-in feature, not default
baseline date: 2026-06-14
```

## Verdict

Stella daemon은 더 이상 문서 밖 hidden experiment로만 둘 단계는 지났다. Q145-Q150을 거치며
streaming output, in-memory state, watcher invalidation, performance gate, read-only IDE API가
모두 생겼다. 따라서 `0.6.0-beta`에서는 “documented beta opt-in” 기능으로 올린다.

단, daemon을 기본 build path로 켜면 안 된다.

| 항목 | Q151 판단 |
| --- | --- |
| Default `qstar build` | normal Stella executor 유지 |
| `--use-daemon=auto` | explicit opt-in으로 유지 |
| `--use-daemon=always` | diagnostic/debug opt-in으로 유지 |
| `qstar daemon --start/--stop` | background beta opt-in lifecycle 후보 |
| `qstar daemon --serve` | foreground debugging lifecycle 유지 |
| `qstar daemon --query ...` | IDE/AI read-only beta opt-in 후보 |
| Windows named pipe | deferred |
| Remote daemon | out of scope |

Version line은 `0.6.0-beta`로 승격한다. 이 승격은 daemon을 stable/default surface로 켜는
것이 아니라, 명시적 opt-in 기능과 IDE/AI read API를 beta 문서 표면으로 올리는 변화다.
Q153 이후 socket permission hardening과 protocol mismatch diagnostic은 beta opt-in 기준으로
닫혔고, Q154 이후 background lifecycle MVP도 들어왔다. Q167 이후 Linux opt-in CI lane은
`inotify` watcher trace와 skip/fail reason artifact를 남긴다. Q175는 lifecycle beta seal로
`--start`/`--status`/`--stop`, duplicate start, package-root/build-dir mismatch hard reject,
stale socket/pid/lock cleanup regression, Linux inotify status query artifact를 묶었다.
Stable API version promise, release 간 반복 Linux daemon history, Windows named pipe가 더
닫히기 전까지 daemon은 default-on이 아니다.

## Q145-Q167 Summary

| Round | 결과 |
| --- | --- |
| Q145 | daemon MVP 이후 성능 방향과 IDE 연동 목표를 정리 |
| Q146 | build output을 event stream으로 전환해 progress/warning/error를 실시간 렌더링 |
| Q147 | `state.db`/`deps.db` memory snapshot과 disk writeback path 추가 |
| Q148 | macOS `kqueue`, Linux `inotify` watcher invalidation MVP 추가 |
| Q149 | medium/large corpus에서 `backend=stella-daemon` performance line protocol 추가 |
| Q150 | `qstar daemon --query ...` read-only API 추가 |
| Q167 | Linux opt-in CI에서 `inotify` watcher trace, daemon status/reason artifact 추가 |
| Q175 | lifecycle beta seal: stale socket/pid/lock cleanup, root mismatch reject, Linux status artifact |

Q150 read API method:

```txt
hello
workspace.info
targets.list
diagnostics.list
compile_commands.path
build.summary
```

이 API는 build/test/clean mutation을 열지 않는다. Method contract는
`docs/contracts/daemon-read-api.md`에 둔다.

## Performance Snapshot

Q151 local macOS arm64, socket 허용 환경에서 실행한 medium corpus 대표값:

```txt
medium_project_gate target_count=47 min_targets=40
medium_project_gate scheduler host_jobs=10
medium_project_gate scheduler default_jobs=10 ready_width=40 async_final_actions=40 trace_elapsed_ms=365
medium_project_gate scheduler runner=posix_spawn event_wait=poll
medium_project_gate backend=stella phase=clean elapsed_ms=299
medium_project_gate backend=stella phase=noop elapsed_ms=73
medium_project_gate backend=stella phase=incremental elapsed_ms=93
medium_project_gate backend=stella-daemon phase=clean elapsed_ms=293 cli_clean_ms=299
medium_project_gate backend=stella-daemon phase=noop elapsed_ms=89 cli_noop_ms=73
medium_project_gate backend=stella-daemon phase=incremental elapsed_ms=104 cli_incremental_ms=93
medium_project_gate backend=stella-jobs jobs=10 phase=clean elapsed_ms=294
medium_project_gate backend=stella-jobs jobs=10 phase=noop elapsed_ms=79
medium_project_gate backend=stella-jobs jobs=10 phase=incremental elapsed_ms=112
medium_project_gate staticlib_argv_parity=ok target=//sys/kern/mm:kernel_mm
medium_project_gate backend=ninja phase=clean elapsed_ms=300
medium_project_gate backend=ninja phase=noop elapsed_ms=79
medium_project_gate backend=ninja phase=incremental elapsed_ms=107
medium_project_gate compare backend=stella-daemon phase=clean stella_ms=293 ninja_ms=300 ratio_x100=200 slack_ms=250
medium_project_gate compare backend=stella-daemon phase=noop stella_ms=89 ninja_ms=79 ratio_x100=200 slack_ms=250
medium_project_gate compare backend=stella-daemon phase=incremental stella_ms=104 ninja_ms=107 ratio_x100=200 slack_ms=250
medium_project_gate status=ok perf_issue_count=0 report_only=1
```

해석:

- daemon clean build는 같은 run의 normal Stella/Ninja와 같은 수준이다.
- daemon no-op은 이번 run에서 Ninja보다 약간 느리게 나왔지만 slack 안에 있고 report-only다.
- daemon incremental은 Ninja와 같은 수준이다.
- socket bind가 금지된 sandbox에서는 daemon phase가
  `elapsed_ms=skipped reason=socket-bind-not-permitted`로 기록된다.
- daemon timing은 아직 release input이며 stable performance guarantee가 아니다.

## Platform Status

| Host | 상태 | Q151 판단 |
| --- | --- | --- |
| macOS | Unix domain socket, `posix_spawn`, `kqueue` watcher 검증 | beta opt-in 가능 |
| Linux | Unix domain socket, `posix_spawn`, `poll`, `inotify` path 구현 및 CI validation 후보 | validation-backed beta opt-in 후보 |
| Windows | named pipe, native process/watch integration 없음 | deferred |

Linux는 Q142 이후 Ubuntu gcc/clang workflow가 medium performance artifact를 수집한다. Q167부터
`daemon_socket_smoke=true` workflow는 `make qstar-linux-daemon-validation-tests`를 실행해
`daemon_watcher status=active backend=inotify`와 watcher event trace를 artifact로 남긴다.
Q175부터 같은 lane은 daemon socket 준비 뒤 `qstar daemon --status`도 기록해
`daemon status=ok experimental=1 pid=...` lifecycle surface를 함께 남긴다.
그래도 daemon socket과 watcher behavior는 Linux CI에서 계속 validation-backed로만 표기한다.
Windows는 `qstar daemon` official host support로 표기하지 않는다.

## Security Gaps

Daemon 보안은 beta opt-in의 남은 핵심이다.

현재 유지할 원칙:

- remote daemon은 scope 밖이다.
- daemon read API는 arbitrary file read를 제공하지 않는다.
- build/test/clean mutation은 read API에 없다.
- package root 밖 path는 기존 QStar graph validation과 watcher registration policy로 차단한다.
- build stream 시작 후 daemon crash는 fallback하지 않고 실패한다.

남은 gap:

- stable daemon API version promise
- repeated Linux CI daemon socket/watcher lane history across releases
- Windows named pipe ACL policy

이 gap 때문에 daemon은 아직 default-on 기능이 아니다. Q153 이후 owner-only socket directory/file
검사, owner mismatch reject, protocol mismatch diagnostic, package root/build_dir hard reject, 안전한
stale socket probe는 beta opt-in 기준으로 구현되어 있다. Q154 이후 `--start`/`--stop`, pid file,
lock file, duplicate start diagnostic도 beta opt-in 기준으로 구현되어 있다. Q175 이후 smoke는
죽은 foreground daemon이 남긴 stale socket을 `--start`가 정리하는지, stale pid/lock sidecar가
보수적으로 정리되는지, 다른 package root가 같은 socket에 붙을 때 hard reject되는지도 확인한다.

## Release Gate

Daemon beta opt-in feature를 release note에 포함하려면 다음 gate가 통과해야 한다.

```sh
make all
make check
make qstar-self-host-tests
make qstar-medium-project-readiness-tests
make qstar-public-beta-release-tests
git diff --check
```

Socket 허용 host에서는 다음을 추가로 실행한다.

```sh
QSTAR_TEST_QSTAR=build/bin/qstar sh tests/smoke.sh
QSTAR_TEST_QSTAR=build/bin/qstar sh tests/medium-project-performance.sh
```

Sandbox에서 socket bind가 막히면 daemon phase skip은 허용하지만, release readiness 문서에는
`socket-bind-not-permitted`로 명시해야 한다.

## Release Note Decision

다음 beta release note에는 다음처럼 표기한다.

- Stella daemon is a documented beta opt-in workflow.
- Normal `qstar build` still uses Stella without daemon residency.
- `--use-daemon=auto|always` and `qstar daemon --query ...` remain explicit.
- macOS is the primary tested host.
- Linux daemon validation is underway through CI artifacts.
- Windows named pipe support is deferred.
- Security hardening gaps remain before any default-on decision.

## Deferred Before Default-On

- Stable daemon API version promise.
- Linux CI daemon socket/watcher lane with artifacts.
- Windows named pipe daemon.
- IDE-facing incremental diagnostics beyond current `diagnostics.list`.
