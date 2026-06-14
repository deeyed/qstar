# Persistent Stella Daemon Workflow

이 문서는 Round Q143에서 persistent Stella daemon의 장기 계약을 고정한다. 아직 구현된
CLI surface가 아니라 설계 문서다. 목표는 QStar의 Lua DSL과 rich diagnostics는 유지하면서,
반복 build invocation마다 Lua eval, Graph IR load, plan cache load, dirty state load를
처음부터 반복하지 않는 구조를 만드는 것이다.

```txt
status: design-only
round: Q143
command namespace: qstar daemon
initial hosts: macOS and Linux over Unix domain sockets
deferred hosts: Windows named pipe
related: docs/perf/stella-plan-cache-design.md
```

## Decision

User-facing command namespace는 `qstar daemon`으로 한다.

```sh
qstar daemon start --file qstar.lua -B build/qstar
qstar daemon status --file qstar.lua -B build/qstar
qstar daemon stop --file qstar.lua -B build/qstar
qstar build --daemon auto //:app
qstar build --daemon on //:app
qstar build --daemon off //:app
```

`qstar stella-daemon`은 채택하지 않는다. 이유는 세 가지다.

- `daemon`은 QStar CLI 안에서 service lifecycle을 다루는 더 일반적인 namespace다.
- 현재 daemon은 Stella executor용이지만, IDE/editor/AI integration은 같은 daemon
  lifecycle을 사용할 수 있다.
- user-facing command가 executor 내부 이름에 너무 강하게 묶이지 않는다.

`stella`라는 이름은 계속 generator/executor identity에 남는다. Daemon protocol의 service
name도 `stella`로 기록할 수 있지만, CLI command는 `qstar daemon`이다.

## Rollout Policy

초기 구현은 opt-in이다.

| Mode | 의미 |
| --- | --- |
| `--daemon off` | daemon을 사용하지 않고 현재 Stella build path를 그대로 사용한다. |
| `--daemon auto` | daemon 연결을 시도하고 실패하면 normal Stella build로 fallback한다. |
| `--daemon on` | daemon 연결 또는 request가 실패하면 command failure로 처리한다. |

초기 beta에서는 `qstar build` 기본값을 즉시 daemon으로 바꾸지 않는다. CLI standalone build의
예측 가능성을 위해 normal Stella path를 유지하고, IDE/editor frontend와 performance
실험은 `--daemon auto` 또는 explicit daemon lifecycle로 시작한다. 충분히 안정화되면 future
release에서 default policy를 다시 검토한다.

Fallback은 silent success가 아니다. `--verbose` 또는 `--schedule-trace`에서는 다음처럼
보여야 한다.

```txt
daemon status=unavailable reason=socket-missing fallback=stella
```

일반 output에서는 daemon fallback이 build result를 오염시키지 않아야 한다. 단,
`--daemon on`에서는 fallback하지 않고 명확한 diagnostic을 낸다.

## Why A Daemon

현재 Stella는 다음 파일 기반 cache를 이미 가진다.

```txt
build/qstar/stella/manifest.json
build/qstar/stella/inputs.json
build/qstar/stella/graph.qsg
build/qstar/stella/actions.qsa
build/qstar/state/state.db
build/qstar/state/deps.db
```

이 구조는 no-op과 incremental build에서 큰 효과가 있다. 하지만 CLI process가 매번 새로
뜬다는 사실은 그대로다. Persistent daemon은 다음 비용을 process lifetime 안으로 끌어올린다.

- Lua runtime 초기화
- authoring input fingerprint read
- lowered action plan load
- compact state/deps DB load
- target/config/profile lookup table materialization
- repeated path/label/description formatting cache
- file watcher event history

즉 daemon은 QStar 문법을 바꾸는 기능이 아니라 Stella 실행 경로의 residency model을 바꾸는
기능이다.

## Architecture

```txt
qstar CLI client
  -> Unix socket
  -> qstar daemon process
       -> workspace root guard
       -> loaded Graph IR / Stella Plan IR
       -> state.db / deps.db memory index
       -> file watcher invalidation queue
       -> Stella scheduler
       -> action log / replay writer
```

Daemon은 package root와 build directory 단위로 하나의 active instance를 가진다. 같은 source
tree라도 `-B build/a`와 `-B build/b`는 서로 다른 daemon identity로 본다. `-G ninja`는 daemon
대상이 아니다. Daemon은 Stella executor service다.

## Socket Layout

macOS와 Linux는 Unix domain socket을 먼저 지원한다.

```txt
build/qstar/stella/daemon/
  qstar-daemon.sock
  qstar-daemon.pid
  qstar-daemon.lock
  qstar-daemon.log
```

Socket은 build directory 아래에 둔다. Project root에 `.qstar.sock` 같은 파일을 만들지
않는다. Build directory가 지워지면 daemon은 stale로 간주된다.

권장 permission:

```txt
directory: 0700
socket:    0600
pid file:  0600
lock file: 0600
```

Windows named pipe는 deferred다. Future path는 다음 형태를 후보로 둔다.

```txt
\\.\pipe\qstar\<package-root-hash>\<build-dir-hash>
```

Windows에서는 path separator, ACL, process lifetime, console control event가 다르므로
Unix socket 설계가 곧바로 official contract가 되지 않는다.

## Protocol

Protocol은 newline-delimited JSON request/response를 1차 후보로 둔다. QStar는 이미 LSP와
JSON output surface를 갖고 있으므로 IDE/AI integration에도 자연스럽다. 구현 시 큰 JSON
dependency를 추가하지 않고 QStar-local minimal parser로 시작할 수 있다.

첫 handshake:

```json
{"id":1,"method":"hello","params":{"client":"qstar-cli","protocol":1,"qstar_version":"0.5.1-beta.1"}}
```

응답:

```json
{"id":1,"ok":true,"result":{"service":"stella","protocol":1,"package_root":"/abs/project","build_dir":"build/qstar"}}
```

Build request:

```json
{
  "id": 2,
  "method": "build",
  "params": {
    "file": "qstar.lua",
    "label": "//:app",
    "generator": "stella",
    "build_dir": "build/qstar",
    "profile": "default",
    "target": "host",
    "progress": "auto",
    "color": "auto"
  }
}
```

Streaming events are response objects with the same request id:

```json
{"id":2,"event":"progress","percent":75,"message":"Linking C executable app"}
{"id":2,"event":"diagnostic","severity":"warning","message":"warning: ..."}
{"id":2,"event":"action","action_id":"//:app:link:0","description":"Linking C executable app"}
```

Final response:

```json
{"id":2,"ok":true,"result":{"status":"ok","run":3,"skip":12,"fail":0,"elapsed_ms":81}}
```

Protocol requirements:

- Requests are package-root scoped.
- Every request carries enough identity to reject build_dir/profile/generator mismatch.
- The daemon never accepts shell-string commands from clients.
- The daemon does not expose arbitrary filesystem read/write RPC.
- Build output remains compatible with normal CLI output because the CLI client renders daemon
  events through the same progress/color renderer.

## Persistent Cache Model

Daemon memory state mirrors existing on-disk state.

| State | Source | Daemon behavior |
| --- | --- | --- |
| Authoring input fingerprints | `stella/inputs.json` and file watcher | Keep current in memory, invalidate on watcher event. |
| Lowered graph summary | `stella/graph.qsg` | Load once, reuse until fingerprint mismatch. |
| Lowered action plan | `stella/actions.qsa` | Load once, reuse for matching build request closure. |
| Dirty-check state | `state/state.db` | Keep compact index in memory, batch write after build. |
| Discovered deps | `state/deps.db` | Keep header list index in memory, refresh on depfile change. |
| Debug dumps | opt-in only | Do not create unless requested by env/trace. |

If watcher state is uncertain, the daemon must fall back to the same fingerprint checks that a fresh
CLI invocation performs. Watcher events are an acceleration signal, not the only correctness source.

## File Watcher

Initial watcher backends:

| Host | Backend | Status |
| --- | --- | --- |
| macOS | FSEvents or kqueue candidate | Design target |
| Linux | inotify | Design target |
| Windows | named pipe plus ReadDirectoryChangesW | Deferred |

Watcher scope:

- `qstar.lua`
- imported `.qst`
- imported `.qsm`
- source/header paths in the requested graph
- generated_dir paths owned by QStar
- build state files under the effective build_dir

Watcher must not watch or traverse package root parents. If a source path escapes the package root,
existing QStar validation rejects it before daemon use.

Watcher invalidation classes:

| Event | Invalidation |
| --- | --- |
| authoring file changed | Drop Graph IR and Stella Plan IR. |
| source/header changed | Keep graph, mark affected actions dirty. |
| generated output deleted | Mark producing/consuming action dirty. |
| build_dir deleted | Stop daemon or mark stale. |
| profile/toolchain input changed | Drop Graph IR and Stella Plan IR. |

## Security

Daemon security is part of the contract, not an afterthought.

- The daemon serves exactly one package root and one effective build directory.
- Every path received from a client is normalized and checked against the package root or build dir.
- Socket directory and socket file must be owner-only.
- If socket owner does not match the current user, the CLI refuses to connect.
- The daemon must reject requests with a mismatched package root, build dir, qstar version, or
  protocol version.
- The daemon never accepts arbitrary command execution RPC. It only executes actions produced by the
  validated QStar graph.
- Stale pid/socket cleanup requires both pid liveness check and socket handshake failure before
  unlinking.
- Remote access is out of scope. A future remote daemon must be an explicit opt-in feature with
  authentication and audit logging.

## AI And IDE Integration

The daemon is the long-term bridge between QStar and IDE/editor/AI workflows.

Possible read APIs:

```txt
workspace.info
targets.list
target.explain
diagnostics.list
build.status
action.log
replay.plan
compile_commands.path
```

Possible action APIs:

```txt
build.request
test.request
clean.request
cancel.request
```

Default IDE/AI capability should be read-only. Mutating actions such as build/test/clean require an
explicit user action or a trusted local client policy. File edits are not daemon responsibilities.
They belong to the editor or AI tool, with separate preview/apply/audit rules.

## CLI Fallback

Fallback behavior:

| Condition | `--daemon auto` | `--daemon on` |
| --- | --- | --- |
| socket missing | normal Stella build | fail |
| protocol mismatch | normal Stella build | fail |
| stale daemon | cleanup then normal Stella build | fail after cleanup attempt |
| request rejected due to root mismatch | fail | fail |
| daemon build action fails | fail | fail |
| daemon crashes mid-build | fail, leave last-failure if available | fail |

Root mismatch is never a fallback case because falling back could hide a security problem.

## Implementation Rounds

Recommended future implementation order:

1. `qstar daemon status/start/stop` skeleton with Unix socket and stale cleanup.
2. Read-only `hello`, `workspace.info`, `targets.list` protocol.
3. Persistent plan cache load with authoring fingerprint validation.
4. `qstar build --daemon auto` forwarding, CLI progress rendering, normal fallback.
5. File watcher invalidation for authoring files and source/header paths.
6. In-memory `state.db` and `deps.db` indexes with batch write-back.
7. IDE/AI read-only API surface and audit log.
8. Windows named pipe design refresh and native validation.

## Non-Goals

- No mandatory long-running service for normal QStar use.
- No package manager or dependency resolver.
- No remote daemon in the first implementation.
- No shell-string execution escape hatch.
- No new QStar authoring syntax.
- No Windows official support until a native named-pipe/process validation lane exists.
