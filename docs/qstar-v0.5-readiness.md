# QStar 0.5 Readiness Gate

이 문서는 QStar를 다음 beta/0.5 라인으로 올릴 준비가 되었는지 판단하기 위한
readiness gate다. Q100은 판단 기준을 고정했고, Q110은 이 기준을 바탕으로
`0.5.0-beta.1` release-prep line을 열었다. Q117은 Q111-Q116의 performance,
platform, backend, language-provider 결과를 묶어 `0.5.1-beta.1` beta patch release
후보를 판단한다.

```txt
status: 0.5 beta patch readiness gate
current runtime version: qstar 0.5.1-beta.1
candidate line: qstar 0.5.1-beta.1
gate: make -C qstar qstar-v0.5-readiness-tests
baseline date: 2026-06-13
```

## Verdict

QStar는 0.5 beta line으로 이동할 수 있는 기반은 갖췄다. 단, 0.5를 stable처럼
선언하면 안 된다. 현재 상태는 다음과 같이 판단한다.

| 영역 | 상태 | 0.5 판단 |
| --- | --- | --- |
| Self-host | 통과 경로 있음 | 0.5 beta gate에 포함 |
| Stella executor | medium corpus에서 no-op/incremental 양호 | 0.5 기본 backend 유지 |
| Ninja backend | 일반 C/C++/ASM/sharedlib project 후보 수준 | 비교/backend 후보로 유지 |
| macOS release packaging | public beta gate 있음 | macOS arm64 beta asset 가능 |
| Linux | validation + binary release candidate dry-run | 0.5 release note에 보수적으로 표기 |
| Windows | native validation candidate prep | 0.5 official support로 표기 금지 |
| Docs/CLI drift | smoke guard로 관리 | 0.5 전에 한 번 더 sync 필요 |
| Cale backend | Stella-only language-provider contract | Ninja wrapper lowering deferred |

0.5의 목표는 "QStar를 medium-size C/C++/systems-style project에 실험적으로 적용할 수
있는 beta"다. v1.0 조건인 macOS/Linux/Windows official support, CI/release matrix,
장기 안정 API는 아직 충족하지 않는다.

Q117 판단은 `0.5.0-beta.2`가 아니라 `0.5.1-beta.1`을 추천한다. Q115의 shared library
policy와 Q116의 Cale backend contract는 기존 beta line의 단순 재포장이 아니라
user-facing surface를 보강한 patch-level 변화이기 때문이다.

## Required Gate

0.5 후보를 만들기 전에 다음 명령이 통과해야 한다.

```sh
make all
make check
make qstar-self-host-tests
make qstar-medium-project-readiness-tests
make qstar-public-beta-release-tests
git diff --check
./build/bin/qstar --version
```

`qstar-v0.5-readiness-tests`는 현재 전체 local regression을 대표하는 `make check`에
연결되어 있다. Release 직전에는 위의 explicit gate를 모두 실행한다.

## Self-Host Summary

Self-host는 QStar repository 자체를 QStar graph로 표현하는 parallel build path다.
Makefile은 여전히 canonical bootstrap/release build path이고, self-host는 그 위에 얹은
검증 경로다.

현재 self-host가 확인하는 것:

- root `qstar.lua`가 QStar source graph를 표현한다.
- `//:qstar`는 QStar-built binary를 만든다.
- `//:self_host`는 smoke target group이다.
- Makefile-built binary와 QStar-built binary의 `--version`이 일치해야 한다.
- Stella executor와 Ninja backend 모두 self-host graph를 build해야 한다.
- Ninja backend는 repository root에 `.ninja_log`나 `.ninja_deps`를 만들면 안 된다.

0.5에서는 self-host를 release gate 후보가 아니라 regular beta gate로 승격할 수 있다.
다만 release artifact의 기준 binary는 계속 Makefile-built binary로 둔다.

## Stella vs Ninja Benchmark Summary

Round Q137 local macOS arm64 대표 측정값:

```txt
medium_project_gate target_count=47 min_targets=40
medium_project_gate scheduler host_jobs=10
medium_project_gate scheduler default_jobs=10 ready_width=40 async_final_actions=40 trace_elapsed_ms=257
medium_project_gate backend=stella phase=clean elapsed_ms=247
medium_project_gate backend=stella phase=noop elapsed_ms=67
medium_project_gate backend=stella phase=incremental elapsed_ms=89
medium_project_gate backend=stella-jobs jobs=10 phase=clean elapsed_ms=237
medium_project_gate backend=stella-jobs jobs=10 phase=noop elapsed_ms=68
medium_project_gate backend=stella-jobs jobs=10 phase=incremental elapsed_ms=88
medium_project_gate staticlib_argv_parity=ok target=//sys/kern/mm:kernel_mm
medium_project_gate backend=ninja phase=clean elapsed_ms=251
medium_project_gate backend=ninja phase=noop elapsed_ms=73
medium_project_gate backend=ninja phase=incremental elapsed_ms=97
medium_project_gate compare phase=clean stella_ms=247 ninja_ms=251 ratio_x100=200 slack_ms=250
medium_project_gate compare phase=noop stella_ms=67 ninja_ms=73 ratio_x100=200 slack_ms=250
medium_project_gate compare phase=incremental stella_ms=89 ninja_ms=97 ratio_x100=200 slack_ms=250
medium_project_gate compare backend=stella-jobs phase=clean stella_ms=237 ninja_ms=251 ratio_x100=200 slack_ms=250
medium_project_gate compare backend=stella-jobs phase=noop stella_ms=68 ninja_ms=73 ratio_x100=200 slack_ms=250
medium_project_gate compare backend=stella-jobs phase=incremental stella_ms=88 ninja_ms=97 ratio_x100=200 slack_ms=250
medium_project_gate status=ok perf_issue_count=0 report_only=1
```

해석:

- no-op은 Stella가 67ms로 0.2초대 목표를 충분히 만족하고 Ninja와 같은 수준으로 측정됐다.
- incremental은 Stella가 89ms, Ninja가 97ms로 medium corpus에서 Ninja급 즉시 재빌드 UX를
  제공한다.
- clean build는 Q137 대표 측정에서 Stella default 247ms, explicit jobs 237ms, Ninja 251ms로
  2배 이내 목표를 넘어 1.5배 이내에도 들어왔다. 이 개선은 macOS default jobs fix,
  staticlib archive semantics fix, compile dependency edge relaxation, async final action
  scheduling이 합쳐진 결과다.
- schedule trace는 default jobs=10, ready_width=40, async_final_actions=40을 기록했고,
  staticlib argv parity check는 dependency `.a`가 archive argv에 다시 들어가지 않음을
  확인했다.
- 현재 ratio gate는 작은 project의 절대 noise를 흡수하기 위해 report-only가 기본이다.

0.5 전에는 timing hard fail을 바로 켜기보다 report-only를 유지한다. 대신 release note에는
Stella와 Ninja 비교 수치를 함께 남긴다.

## Ninja Parity Summary

Ninja backend가 lowering하는 surface:

- C/C++/ASM compile
- `qstar.staticlib`
- `qstar.sharedlib` on Darwin-like and Linux-like profiles
- `qstar.executable`
- `qstar.test`
- `qstar.configure_file`
- `qstar.custom_target`
- `qstar.run_target`
- `qstar.group`
- `compile_commands.json`

QStar-owned으로 남기는 surface:

- `stage`
- `install`
- stage/install manifest와 diff policy

0.5에서도 deferred로 남길 것:

- Windows `.dll`/import-library/PDB sharedlib policy
- Cale source Ninja wrapper lowering; Q116 fixes Cale source as Stella-only for now
- C++ modules execution policy
- advanced platform-specific dynamic library rpath/install layout policy

Ninja는 0.5에서 "comparison backend and practical C/C++ backend candidate"로 표기한다.
Stella 기본값은 유지한다.

## Linux And Windows Readiness

Linux:

- 상태는 `validation-backed source build path`에서 `binary release candidate path`로
  올라갔다. 아직 public Linux asset을 publish하지는 않는다.
- macOS local gate는 path/process/install layout smoke만 확인한다.
- Round Q109 기준 `.github/workflows/linux-validation.yml`이 `ubuntu-latest`에서 gcc와
  clang lane을 실행하는 CI 후보를 제공한다.
- Round Q113 기준 gcc lane은
  `QSTAR_RELEASE_PLATFORM=linux-x86_64 tools/package-public-beta.sh`를 실행해
  release-candidate tarball, ELF x86-64 `file(1)` report, `ldd(1)` report,
  installed docs/wiki/manpage smoke, `SHA256SUMS`를 검증한다.
- Linux release asset은 clean Linux host 또는 CI에서 `make all`, `make check`,
  `make qstar-linux-validation-tests`, install docs/man smoke, Linux tarball dry-run이
  통과한 뒤에만 추가한다.
- clang/gcc depfile behavior는 `QSTAR_LINUX_VALIDATION_CC` matrix로 확인한다.
- Round Q142부터 같은 Ubuntu gcc/clang workflow가 medium Stella/Ninja performance line
  protocol artifact를 업로드한다. Linux Stella trace는
  `medium_project_gate scheduler runner=posix_spawn event_wait=poll`을 포함해야 하고,
  Ninja clean phase도 같은 output에 있어야 한다. Timing threshold는 아직 report-only다.

Windows:

- 상태는 `planned validation`에서 `native validation candidate prep`으로 올라갔다.
- 현재는 path normalization, argv-vector process model, MSVC response file escaping,
  `.exe`/external `.lib` artifact spelling, manual Windows workflow 후보를 문서와
  corpus로 준비한 단계다.
- 0.5에서 Windows official support나 release artifact를 선언하지 않는다.
- Native Windows source build, install layout, real Windows process spawn,
  automatic static `.lib`, `.dll`/import library/PDB policy, regular CI lane은 별도
  라운드로 남긴다.

## Docs And CLI Drift Summary

현재 drift guard는 `tests/smoke.sh`가 담당한다. 0.5 후보 전에는 다음 surface가 서로
같은 내용을 말해야 한다.

- `qstar help`
- `qstar docs --path`
- `qstar docs --ai-index`
- `qstar docs --show ...`
- `wiki/AI_INDEX.md`
- `wiki/reference/qstar-lua.md`
- `man/man1/qstar.1`
- `man/man5/qstar-lua.5`
- VSCode snippets/syntax surface
- `README.md`, `README.ko.md`, release notes

0.5 beta patch line에서는 `qstar 0.5.1-beta.1` version bump와 함께 위 문서의 old
version string이 현재-facing 문서에 남아 있지 않은지 확인한다. 과거 seal 문서의
historical version record는 보존한다.

## Medium Project Readiness

현재 medium corpus는 47개 target을 가진다. `qstar.group`, `qstar.config`,
`qstar.import_module`, 여러 staticlib, executable, Stella/Ninja 비교를 포함한다.

판단:

- 단순 파일럿을 넘어서 medium-size C/C++/systems-style project의 beta 적용 후보는 된다.
- 대형 mono-repo, 많은 external package, remote cache, cross-platform release matrix는
  아직 0.5 scope가 아니다.
- QStar는 dependency resolver나 package fetcher가 아니므로, 0.5에서도 external dependency
  resolution은 QStar 밖의 tool이 맡아야 한다.

## Q117 Beta Patch Gate

- Runtime version bump: `qstar 0.5.1-beta.1`.
- Release notes: `docs/releases/v0.5.1-beta.1.md`.
- Public beta package smoke: `make qstar-public-beta-release-tests`.
- Self-host gate: `make qstar-self-host-tests`.
- Medium performance report: Stella vs Ninja clean/no-op/incremental 수치.
- CMake-style progress output: `[ 75%] Linking CXX executable app` 형식과
  warning/error stream coloring 상태 재검증.
- Linux validation status refresh: `.github/workflows/linux-validation.yml`의 gcc/clang
  lane, medium performance artifact, clean Linux host 결과가 있으면 반영.
- Docs/man/wiki/AI index sync: old generator name, old version string, removed API 잔재 제거.
- VSCode extension version policy: runtime과 별도로 `0.3.0` 유지. 이번 runtime tarball에는
  VSIX를 포함하지 않는다.
- Release line decision: Q115/Q116의 sharedlib/Cale backend 계약을 포함하므로
  `0.5.0-beta.2`가 아니라 `0.5.1-beta.1`로 낸다.

## Deferred After 0.5

- Windows official support and Windows release artifact.
- Linux public release artifact decision. Q113 adds the dry-run path, but attaching
  the asset to a GitHub release remains a separate release decision.
- Persistent Stella daemon maturation. Q144 adds an experimental foreground Unix socket MVP with
  `qstar daemon --serve` and `qstar build --use-daemon=...`, and Q146 adds streaming build
  events so daemon builds render the same progress/warning/error output as normal Stella builds.
  Q147 adds in-memory `state.db`/`deps.db` snapshots with disk writeback for crash recovery.
  Q148 adds experimental macOS `kqueue` and Linux `inotify` watcher invalidation. Background
  lifecycle and permission hardening remain post-0.5 work.
- Windows `.dll`/import-library/PDB sharedlib support.
- Cale source Ninja wrapper lowering.
- C++ modules execution policy.
- Remote package resolution, lockfile, registry, fetch policy.
- Stable public Graph IR/cache protocol.
- Board-specific builtins.
- VSCode Marketplace publication.

## Version Policy

- `0.4.x-beta.*`: public beta packaging, Stella/Ninja/self-host hardening line.
- `0.5.0-beta.1`: medium project readiness, self-host regular gate, refreshed docs/release line.
- `0.5.1-beta.1`: sharedlib policy, Cale backend contract, platform readiness, beta patch gate.
- `0.5.x-beta.*`: platform validation and backend parity patch line.
- `1.0.0`: macOS, Linux, Windows official release artifacts and CI matrix are required.

Runtime version is owned by `include/qstar/qstar.h`. Git tags must match
`v$QSTAR_VERSION`. VSCode extension version is independent and must be called out in
release notes.

## Known Gaps

- Performance gates are still report-only for timing thresholds.
- Linux has a binary release-candidate dry-run, but it is not yet a published
  artifact. Windows has a manual native validation candidate workflow, but it is
  not yet official host support.
- `qstar.sharedlib` supports Darwin-like `.dylib` and Linux-like `.so` builds, but
  Windows `.dll`/import-library/PDB policy is deferred.
- Cale source is a Stella-only language-provider action in this release; Ninja
  wrapper lowering is deferred by contract.
- Package/dependency resolution is intentionally outside QStar.
- Editor extension packaging is separate from runtime release packaging.
