# v1 Readiness

QStar 1.0은 아직 ready가 아니다. Canonical gap report는 source tree의
[`docs/qstar-v1-readiness.md`](https://github.com/deeyed/qstar/blob/main/docs/qstar-v1-readiness.md)에
둔다.
Stable DSL compatibility promise는
[`docs/qstar-compatibility-policy.md`](https://github.com/deeyed/qstar/blob/main/docs/qstar-compatibility-policy.md)에
둔다.

이 wiki page는 사용자가 GitHub Wiki에서 v1 기준을 찾을 때의 entrypoint다.

## 현재 판단

```txt
status: v1 readiness gaps defined
v1 decision: not ready
```

## 핵심 blocker

- Windows official GitHub Release asset과 downloaded-asset smoke. Q253 기준
  `windows-validation.yml`의 `publish_windows_asset=true` run이
  `v0.7.19-beta`에서 `windows_release_asset status=published`,
  `download_smoke=ok` evidence를 남겼다. 이 evidence는 보유 상태지만, 다음
  beta/RC tag와 v1 candidate tag에서 같은 release-mutating gate를 반복해야
  Windows official blocker가 닫힌다.
- Stable DSL compatibility/removal policy의 release-line 적용. Q256 policy는
  봉인되었고, v1 전까지 docs/wiki/man/smoke guard sync를 유지해야 한다.
- daemon stable/default boundary 확정. 현재는 beta opt-in이다.
- macOS/Linux/Windows release matrix 반복 green.
- 자세한 OS별 release evidence ledger는
  [`docs/release-matrix-evidence.md`](https://github.com/deeyed/qstar/blob/main/docs/release-matrix-evidence.md)에 둔다.
  이 ledger는 local smoke, candidate artifact, published GitHub Release asset을 구분한다.
- package resolver, registry, lockfile, fetch policy out-of-core 원칙 유지.

## Stable Surface 후보

Stable 후보는 project/toolset/config, artifact targets, generated actions,
run/stage/group, root `qstar.command`, imports/modules, GLP consumer activation,
command/workflow helpers, and authoring helpers다. Provider-author API는 아직
stable 조건을 별도로 통과해야 한다. Q257 기준 provider-author API stable promotion은
non-blocker로 분리되었고, `qstar.lang/1`은 versioned beta contract이며,
consumer-facing GLP는 stable 후보로 남는다.

## Release 판단

v1 release candidate는 최소한 다음 gate를 통과해야 한다.

```sh
make check
make qstar-v0.8-release-tests
gh workflow run linux-validation.yml --ref main
gh workflow run windows-validation.yml --ref main
gh workflow run windows-validation.yml --ref v<version> \
  -f release_tag=v<version> \
  -f publish_windows_asset=true
git diff --check
```

Windows release gate는 GitHub Release asset을 실제로 publish한 뒤 다시 내려받아
smoke해야 한다. `windows-validation.yml --ref main` freshness run은 유용하지만,
release asset을 mutate/re-consume하지 않으므로 반복 evidence를 대체하지 않는다.

미래에 v1 전용 release gate 이름이 생길 수 있지만, 범위는 Q251 v0.8 gate보다 좁아지면
안 된다.
