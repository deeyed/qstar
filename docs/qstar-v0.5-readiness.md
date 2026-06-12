# QStar 0.5 Readiness Gate

이 문서는 QStar를 다음 beta/0.5 라인으로 올릴 준비가 되었는지 판단하기 위한
readiness gate다. Q100은 version bump가 아니라 판단 기준을 고정하는 라운드다.

```txt
status: 0.5 readiness gate
current runtime version: qstar 0.4.0-beta.1
candidate line: qstar 0.5.0-beta.1
gate: make -C qstar qstar-v0.5-readiness-tests
baseline date: 2026-06-13
```

## Verdict

QStar는 0.5 beta line으로 이동할 수 있는 기반은 갖췄다. 단, 0.5를 stable처럼
선언하면 안 된다. 현재 상태는 다음과 같이 판단한다.

| 영역 | 상태 | 0.5 판단 |
| --- | --- | --- |
| Self-host | 통과 경로 있음 | 0.5 beta gate에 포함 가능 |
| Stella executor | medium corpus에서 no-op/incremental 양호 | 0.5 기본 backend 유지 가능 |
| Ninja backend | 일반 C/C++/ASM project 후보 수준 | 비교/backend 후보로 유지 |
| macOS release packaging | public beta gate 있음 | macOS arm64 beta asset 가능 |
| Linux | validation underway | 0.5 release note에 보수적으로 표기 |
| Windows | planned validation | 0.5 official support로 표기 금지 |
| Docs/CLI drift | smoke guard로 관리 | 0.5 전에 한 번 더 sync 필요 |

0.5의 목표는 "QStar를 medium-size C/C++/systems-style project에 실험적으로 적용할 수
있는 beta"다. v1.0 조건인 macOS/Linux/Windows official support, CI/release matrix,
장기 안정 API는 아직 충족하지 않는다.

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

Round Q100 local macOS arm64 측정값:

```txt
medium_project_gate target_count=47 min_targets=40
medium_project_gate backend=stella phase=clean elapsed_ms=494
medium_project_gate backend=stella phase=noop elapsed_ms=76
medium_project_gate backend=stella phase=incremental elapsed_ms=111
medium_project_gate backend=ninja phase=clean elapsed_ms=300
medium_project_gate backend=ninja phase=noop elapsed_ms=78
medium_project_gate backend=ninja phase=incremental elapsed_ms=100
medium_project_gate status=ok perf_issue_count=0 report_only=1
```

해석:

- no-op은 Stella가 76ms로 0.2초대 목표를 충분히 만족한다.
- incremental은 Stella가 111ms로 medium corpus에서 즉시 재빌드 UX를 제공한다.
- clean build는 Stella가 Ninja보다 느리지만 small/medium corpus에서 1초 미만이다.
- 현재 ratio gate는 작은 project의 절대 noise를 흡수하기 위해 report-only가 기본이다.

0.5 전에는 timing hard fail을 바로 켜기보다 report-only를 유지한다. 대신 release note에는
Stella와 Ninja 비교 수치를 함께 남긴다.

## Ninja Parity Summary

Ninja backend가 lowering하는 surface:

- C/C++/ASM compile
- `qstar.staticlib`
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

- `qstar.sharedlib` full lowering
- Cale source Ninja lowering
- C++ modules execution policy
- platform-specific dynamic library install/rpath policy

Ninja는 0.5에서 "comparison backend and practical C/C++ backend candidate"로 표기한다.
Stella 기본값은 유지한다.

## Linux And Windows Readiness

Linux:

- 상태는 `validation underway`다.
- macOS local gate는 path/process/install layout smoke만 확인한다.
- Linux release asset은 clean Linux host 또는 CI에서 `make all`, `make check`,
  `make qstar-linux-validation-tests`, install smoke가 통과한 뒤에만 추가한다.
- clang/gcc depfile behavior는 0.5 Linux release asset 전 필수 확인이다.

Windows:

- 상태는 `planned validation`이다.
- 현재는 path normalization, argv-vector process model, response file policy, `.exe`
  artifact planning을 문서와 corpus로 준비한 단계다.
- 0.5에서 Windows official support나 release artifact를 선언하지 않는다.
- Native Windows source build, install layout, process spawn, `.lib`/`.dll` policy,
  CI lane은 별도 라운드로 남긴다.

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

0.5로 가기 전에는 `qstar 0.5.0-beta.1` version bump와 함께 위 문서의 old version
string이 남아 있지 않은지 확인한다.

## Medium Project Readiness

현재 medium corpus는 47개 target을 가진다. `qstar.group`, `qstar.config`,
`qstar.import_module`, 여러 staticlib, executable, Stella/Ninja 비교를 포함한다.

판단:

- 단순 파일럿을 넘어서 medium-size C/C++/systems-style project의 beta 적용 후보는 된다.
- 대형 mono-repo, 많은 external package, remote cache, cross-platform release matrix는
  아직 0.5 scope가 아니다.
- QStar는 dependency resolver나 package fetcher가 아니므로, 0.5에서도 external dependency
  resolution은 QStar 밖의 tool이 맡아야 한다.

## Must Land Before 0.5

- Runtime version bump: `qstar 0.5.0-beta.1`.
- Release notes: `docs/releases/v0.5.0-beta.1.md`.
- Public beta package smoke: `make qstar-public-beta-release-tests`.
- Self-host gate: `make qstar-self-host-tests`.
- Medium performance report: Stella vs Ninja clean/no-op/incremental 수치.
- Linux validation status refresh: Linux CI 또는 clean Linux host 결과가 있으면 반영.
- Docs/man/wiki/AI index sync: old generator name, old version string, removed API 잔재 제거.
- VSCode extension version policy 결정: runtime과 별도 유지할지 `0.4.0`으로 올릴지 명시.

## Deferred After 0.5

- Windows official support and Windows release artifact.
- Linux release artifact if clean Linux CI가 아직 없다면 0.5 이후로 defer.
- Full `qstar.sharedlib` backend support.
- Cale source Ninja lowering.
- C++ modules execution policy.
- Remote package resolution, lockfile, registry, fetch policy.
- Stable public Graph IR/cache protocol.
- Board-specific builtins.
- VSCode Marketplace publication.

## Version Policy

- `0.4.x-beta.*`: public beta packaging, Stella/Ninja/self-host hardening line.
- `0.5.0-beta.1`: medium project readiness, self-host regular gate, refreshed docs/release line.
- `0.5.x-beta.*`: platform validation and backend parity patch line.
- `1.0.0`: macOS, Linux, Windows official release artifacts and CI matrix are required.

Runtime version is owned by `include/qstar/qstar.h`. Git tags must match
`v$QSTAR_VERSION`. VSCode extension version is independent and must be called out in
release notes.

## Known Gaps

- Performance gates are still report-only for timing thresholds.
- Linux and Windows validation are not yet equal to macOS local validation.
- `qstar.sharedlib` remains plan/check-only.
- Cale source Ninja lowering is deferred.
- Package/dependency resolution is intentionally outside QStar.
- Editor extension packaging is separate from runtime release packaging.
