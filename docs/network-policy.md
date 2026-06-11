# QStar Network Policy

QStar의 기본 network policy는 Fetch-only다.

```txt
graph/build evaluation:
  network forbidden

package fetch/update:
  network conditionally allowed
```

## Forbidden In QStar Graph Phase

`qstar.lua`와 `<dirname>.qst` 평가 중에는 네트워크를 사용할 수 없다.

금지:

- HTTP request
- git fetch
- package download
- registry query
- tool download
- remote script load
- current network state probe

Build graph는 네트워크 상태에 따라 달라지면 안 된다.

## Forbidden In QStar Build Action By Default

QStar build action도 v0에서는 네트워크를 직접 요구할 수 없다.

예를 들어 `qstar.custom_target`이 remote API를 호출해 source를 생성하는 모델은 v0에서 금지한다.

장기적으로 action-level network permission을 검토할 수는 있지만, sandbox, lock 기록, cache key, CI policy가 준비되기 전에는 열지 않는다.

## Allowed Package Commands

네트워크는 명시 package-manager 단계에서만 조건부로 허용한다.

```txt
cale fetch
cale add
cale update
cale vendor
cale publish  # future
```

QStar는 resolved package root map만 소비한다.

## Modes

장기 CLI 방향:

```txt
--offline
  네트워크 완전 금지

--locked
  lock file이 없거나 resolved graph가 drift하면 실패

--frozen
  lock file과 package cache를 변경하지 않음

--allow-network=none
  네트워크 금지

--allow-network=fetch
  package fetch 단계만 허용
```

권장 기본값:

```txt
qstar build:
  --allow-network=none

cale fetch/add/update:
  --allow-network=fetch

CI:
  --frozen --offline
```

## Lock And Checksum

Network fetch는 반드시 package-manager-owned lock/checksum 기록을 남겨야 한다.
QStar graph evaluation은 이미 resolve된 package root map만 소비하며, lock을 갱신하거나
fetch를 수행하지 않는다.

## Rationale

이 정책은 다음을 보장하기 위한 것이다.

- reproducible build
- deterministic graph
- CI stability
- offline build
- secure package fetch boundary
- audit-friendly build log
