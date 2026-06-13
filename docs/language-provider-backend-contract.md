# QStar Language Provider Backend Contract

Q116 기준 QStar의 language provider backend 계약을 고정한다. 목적은 QStar가 Cale 전용
도구처럼 보이지 않게 하면서도 Cale source를 사용하는 프로젝트가 backend 선택 실패를 예측할 수
있게 하는 것이다.

## 결정

Cale source는 이번 release에서 Stella-only language-provider action이다.

QStar는 Cale source를 다음처럼 다룬다.

- `.cale`/`.cl` suffix는 source registry에서 `language=cale`, `provider=cale` compile input이다.
- Stella executor는 configured `cale` compiler를 process로 호출한다.
- QStar는 Cale frontend/backend 내부 API를 호출하지 않는다.
- QStar는 HCL을 해석하지 않는다.
- HCL은 `lang.cale.public_headers`, `lang.cale.private_headers`, include dir, install/export
  surface에 들어가는 header-like path다.
- Ninja backend는 Cale compile wrapper rule을 생성하지 않는다.

## 왜 Ninja wrapper를 열지 않는가

Ninja lowering을 열려면 최소한 다음 계약이 필요하다.

- Cale compiler의 stable argv contract
- Cale depfile 또는 discovered-input contract
- response-file style
- generated header/object ownership
- action-log/replay wrapper format
- failure-kind와 last-failure replay mapping

이 계약 없이 단순히 `cale` process를 Ninja rule로 감싸면 QStar가 Cale provider semantics를
부분적으로 소유하는 것처럼 보이고, backend별 rebuild/replay 결과가 달라질 수 있다.

## 현재 backend별 동작

| Source kind | Stella | Ninja |
| --- | --- | --- |
| C | compile action | lowered |
| C++ | compile action | lowered |
| ASM | compile action | lowered |
| Cale | provider process action | stable diagnostic |
| HCL | opaque header-like path | opaque header-like path |

Ninja에서 Cale source target을 build하면 다음 형태의 diagnostic을 낸다.

```txt
qstar: Cale source 'src/core.cale' is a Stella-only language-provider action in this release; Ninja wrapper lowering is deferred; use -G stella
```

## Future 조건

Cale source Ninja support를 추가하려면 wrapper lowering을 별도 라운드로 열고, 위의 argv,
depfile, response-file, replay 계약을 먼저 문서화해야 한다. 그 전까지 `-G ninja`는 Cale
source를 가진 target에서 실패하는 것이 정본 동작이다.
