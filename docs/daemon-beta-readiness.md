# Stella Daemon Beta Readiness Gate

이 문서는 Round Q151에서 Stella daemon을 계속 hidden experimental로 둘지, beta opt-in 기능으로
올릴지 판단하기 위한 readiness gate다. 여기서 `Stella daemon`은 Stella IDE가 아니라 QStar의
native executor를 장기 실행 build service로 붙이는 experimental daemon path를 뜻한다.

```txt
status: daemon beta opt-in readiness gate
current runtime version: qstar 0.7.18-beta
release line: qstar 0.7.18-beta
decision: documented beta opt-in feature, not default
baseline date: 2026-06-17
```

## Verdict

Stella daemon은 더 이상 문서 밖 hidden experiment로만 둘 단계는 지났다. Q145-Q150을 거치며
streaming output, in-memory state, watcher invalidation, performance gate, read-only IDE API가
모두 생겼다. 따라서 `0.6.0-beta`에서 처음 “documented beta opt-in” 기능으로 올렸고,
현재 `0.7.18-beta` line에서도 explicit beta opt-in으로 유지한다.

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

Historical first beta line은 `0.6.0-beta`다. 이 승격은 daemon을 stable/default surface로 켜는
것이 아니라, 명시적 opt-in 기능과 IDE/AI read API를 beta 문서 표면으로 올리는 변화다.
Q153 이후 socket permission hardening과 protocol mismatch diagnostic은 beta opt-in 기준으로
닫혔고, Q154 이후 background lifecycle MVP도 들어왔다. Q167 이후 Linux opt-in CI lane은
`inotify` watcher trace와 skip/fail reason artifact를 남긴다. Q175는 lifecycle beta seal로
`--start`/`--status`/`--stop`, duplicate start, package-root/build-dir mismatch hard reject,
stale socket/pid/lock cleanup regression, Linux inotify status query artifact를 묶었다.
Stable API version promise, release 간 반복 Linux daemon history, Windows named pipe가 더
닫히기 전까지 daemon은 default-on이 아니다. Q233은 GLP/Windows work 이후 medium/large
daemon timing과 local daemon socket smoke freshness를 다시 확인했다. Q249는 이 상태를 v1
stable로 착각하지 않도록 help/docs를 다시 조이고, normal Stella parity, auto fallback,
read-only query API, socket permission, identity mismatch, stale socket/pid/lock cleanup을
한 번에 확인하는 beta boundary regression을 추가했다.

## Q145-Q233 Summary

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
| Q233 | GLP/Windows 이후 local medium/large daemon timing과 daemon socket smoke freshness 재측정 |
| Q249 | daemon beta boundary: fallback parity, read API freshness, socket permission, mismatch, stale lifecycle regression |

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

Q233 local macOS arm64, socket 허용 환경에서 실행한 대표값은
`docs/perf/q233-backend-daemon-refresh.md`에 보존한다.

| Corpus | Backend | Clean | No-op | Incremental | Ninja ratio clean/no-op/incremental |
| --- | --- | ---: | ---: | ---: | --- |
| medium | Stella daemon | 289ms | 68ms | 88ms | 1.14x / 0.92x / 0.76x |
| medium | Ninja | 254ms | 74ms | 116ms | baseline |
| large 200 | Stella daemon | 1179ms | 102ms | 126ms | 0.88x / 0.97x / 0.75x |
| large 200 | Ninja | 1344ms | 105ms | 167ms | baseline |
| large 500 | Stella daemon | 2614ms | 107ms | 151ms | 0.96x / 0.69x / 0.72x |
| large 500 | Ninja | 2737ms | 156ms | 211ms | baseline |

해석:

- medium daemon clean build는 normal Stella보다 느리고 Ninja 대비 `1.14x`로 나왔다. 이는
  daemon lifecycle/IPC overhead가 clean path에서 이득을 상쇄한다는 기존 판단과 맞다.
- medium daemon no-op은 Ninja보다 빠르고, incremental은 이번 run에서 Ninja보다 확실히 빠르다.
- large 200/500 corpus에서는 daemon clean/no-op/incremental 모두 Ninja보다 빠르다.
- socket bind가 금지된 sandbox에서는 daemon phase가
  `elapsed_ms=skipped reason=socket-bind-not-permitted`로 기록된다.
- daemon timing은 아직 release input이며 stable performance guarantee가 아니다.

## Platform Status

| Host | 상태 | Q233 판단 |
| --- | --- | --- |
| macOS | Unix domain socket, `posix_spawn`, `kqueue` watcher 검증. Q233 local daemon smoke 통과 | beta opt-in 가능 |
| Linux | Unix domain socket, `posix_spawn`, `poll`, `inotify` path 구현 및 opt-in CI validation lane 유지 | validation-backed beta opt-in 후보 |
| Windows | named pipe, native process/watch integration 없음 | deferred |

Linux는 Q142 이후 Ubuntu gcc/clang workflow가 medium performance artifact를 수집한다. Q167부터
`daemon_socket_smoke=true` workflow는 `make qstar-linux-daemon-validation-tests`를 실행해
`daemon_watcher status=active backend=inotify`와 watcher event trace를 artifact로 남긴다.
Q175부터 같은 lane은 daemon socket 준비 뒤 `qstar daemon --status`도 기록해
`daemon status=ok experimental=1 pid=...` lifecycle surface를 함께 남긴다.
Q249부터 같은 optional lane은 `make qstar-daemon-beta-boundary-tests`도 실행해
`daemon_beta_boundary status=ok host=Linux` artifact를 남긴다. 이 artifact는 Linux watcher
성능 자체가 아니라 beta boundary가 유지되는지 확인한다.
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
Q249 boundary test는 insecure socket directory와 non-socket path reject를 별도 artifact로 남기며,
normal Stella와 daemon build 결과가 같은지도 확인한다.

## Release Gate

Daemon beta opt-in feature를 release note에 포함하려면 다음 gate가 통과해야 한다.

```sh
make all
make check
make qstar-daemon-beta-boundary-tests
make qstar-self-host-tests
make qstar-medium-project-readiness-tests
make qstar-public-beta-release-tests
git diff --check
```

Socket 허용 host에서는 다음을 추가로 실행한다.

```sh
QSTAR_TEST_QSTAR=build/bin/qstar sh tests/smoke.sh
QSTAR_TEST_QSTAR=build/bin/qstar sh tests/daemon-beta-boundary.sh
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
