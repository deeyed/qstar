# Local Action Cache

QStar local action cache는 compile/generated file action의 산출물을 effective build directory
안의 content-addressed store에 저장하고 재사용하는 opt-in 기능이다. 기본값은 `off`이며,
strict sandbox나 remote cache/remote execution을 의미하지 않는다.

## 활성화

```lua
qstar.project {
  name = "demo",
  root = ".",
  action_cache = "local",
}
```

Invocation override:

```sh
qstar build //:app --action-cache local
qstar build //:app --action-cache off
```

## Action 제외

Artifact target, test, objectlib, config와 generated action은 `cacheable` boolean을 받는다.

```lua
qstar.transform "snapshot" {
  input = "inputs/request.bin",
  output = qstar.output("generated/snapshot.bin"),
  command = qstar.cli {"tools/snapshot", qstar.input(0), qstar.output(0)},
  cacheable = false,
}
```

기본값은 `true`지만 이는 후보 선언일 뿐이다. 현재 실제 CAS 대상은 source compile과
regular-file generated action이다. Archive, link, run/test/stage/command action은 저장하지
않는다. Tool을 resolve할 수 없거나 output ownership이 없으면 제외한다.

외부 system emulator/HIL 계열 tool, `/dev/tty...`, `/Volumes/...`, serial/device token을 참조하는
action은 외부 상태와 상호작용하는 것으로 보고 자동으로 non-cacheable 처리한다. 프로젝트가
장치나 외부 runtime을 다루는 action에는 `cacheable = false`를 명시하는 것이 권장된다.

## Key와 손상 검증

Key에는 argv, declared input과 depfile-discovered header content, output identity, tool executable content fingerprint,
`PATH`/`SDKROOT`/`CPATH`/`LIBRARY_PATH`, explicit action env가 들어간다. Env 값은 log에
출력하지 않고 fingerprint 또는 redacted metadata로만 다룬다.

Restore 시 manifest와 blob content digest를 검증한다. 손상된 entry는 폐기하고 miss로
처리한 뒤 action을 다시 실행한다. Target 단위 clean은 local CAS를 유지하지만 전체 build
directory clean은 CAS도 제거한다.

## Audit와 통계

```sh
qstar build //:app --action-cache local --explain-cache
```

Audit은 report-only다. `undeclared-path=...`는 error가 아니며,
`undeclared-access=unobserved`도 hermeticity 증명이 아니다. QStar는 아직 child process의 실제
filesystem/network access를 sandbox로 관찰하지 않는다.

최종 `local_cache_stats`는 `audited`, `eligible`, `non_cacheable`, `hits`, `misses`, `stores`,
`corruptions`를 Stella/Ninja 공통 형식으로 출력한다. `qstar explain`, `query`,
`list-targets --format json`에서는 선언된 project policy와 cacheability를 확인할 수 있다.

내부 계약과 범위 밖 항목은
[Local Action Cache와 Hermeticity Audit](../../docs/local-action-cache.md)에 정리되어 있다.
