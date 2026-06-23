# Compatibility Policy

QStar v1의 stable DSL promise는 source tree의 canonical 문서인
[`docs/qstar-compatibility-policy.md`](https://github.com/deeyed/qstar/blob/main/docs/qstar-compatibility-policy.md)에
둔다.

이 wiki page는 GitHub Wiki에서 compatibility 기준을 찾는 사용자를 위한 entrypoint다.

## 현재 판단

```txt
status: Q256 stable DSL compatibility policy sealed
v1 promise: candidate surface defined, v1 release gate still required
```

## Stable 후보

- `qstar.project`, `qstar.toolset`, `qstar.config`
- `qstar.executable`, `qstar.staticlib`, `qstar.sharedlib`, `qstar.test`
- `qstar.objectlib`의 `compile_context = "own" | "consumer"` object collection과
  artifact target의 `objects = {...}` 소비. Consumer-context objectlib는 source-owned
  leaf input을 각 consuming target의 effective configs/lang/toolset으로 per-consumer
  object에 compile한다. Explicit provider source-token 재-lowering은 stable 약속 밖에
  두고, raw provider source string 또는 `"own"` context를 사용한다.
- `qstar.custom_target`, `qstar.transform`, `qstar.configure_file`
- `qstar.run_target`, `qstar.group`, `qstar.stage`, `qstar.target_family`
- `qstar.command`, `qstar.step.*`, `qstar.param.*`, bool argv helper
- `qstar.import_file`, cached `qstar.import_module`, `qstar.subdir`
- consumer-facing GLP: `qstar.use_language`, `lang.<namespace>`, provider
  helper, raw provider source classification
- standard provider consumer 계약: bundled `zig`, `rust`, `cuda`의 short id,
  namespace, documented helper, option schema, raw source suffix behavior는
  `make qstar-standard-provider-compatibility-tests`로 fake-tool Stella/Ninja
  compatibility coverage를 가진다.
- generated artifact helpers: `qstar.input`, `qstar.output`,
  `qstar.target_file`, `qstar.stage_dir`, `qstar.stage_file`
- authoring helpers: `qstar.files`, `qstar.join`, `qstar.copy`,
  `qstar.append`, `qstar.merge`, `qstar.extend`

## Beta 또는 out-of-core

- provider-author API와 provider sandbox/lowering schema는 아직 beta다.
- Stella daemon은 beta opt-in이며 default build behavior가 아니다. Daemon read API와
  `qstar-daemon-query-v1`/`qstar-daemon-read-v1` marker도 stable machine-readable v1 API가 아니다.
- optional real compiler corpus와 hosted manual validation lane은 release
  evidence이지 stable authoring syntax가 아니다.
- package registry, dependency resolver, lockfile, network fetch policy는
  QStar core 밖에 둔다.

## 제거 정책

v1 이후 stable surface를 제거하려면 replacement 문서화, diagnostic period,
release note, docs/wiki/man/snippet 갱신, smoke guard가 모두 필요하다. 같은 major
version 안에서 hard cut은 기본적으로 금지한다.

이미 제거된 profile-era surface, core install command, language-shaped init
template는 compatibility shim으로 되살리지 않는다. Project가 `install` 같은 이름을
원하면 `qstar.command`로 직접 선언한다.
