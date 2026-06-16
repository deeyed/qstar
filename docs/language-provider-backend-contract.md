# QStar Language Provider Backend Contract

QStar runtime은 built-in `c`, `cxx`, `asm` provider namespace를 preloaded registry로
갖고 있다. Public syntax의 `lang.c`, `lang.cxx`, `lang.asm`은 그대로 유지되지만, 내부
source classification과 tool role은 `c.compiler`, `cxx.compiler`, `asm.compiler` 같은
provider role로 내려간다. 그 밖의 언어는 아직 QStar DSL에 provider namespace를 추가하지
않고, 외부 compiler가 생성한 object artifact를 통해 build graph에 연결한다.

## 결정

- 외부 언어 source는 `sources`에 직접 넣지 않는다.
- 외부 compiler 호출은 `qstar.custom_target`으로 작성한다.
- object output은 `qstar.output(path, {format = "object"})`로 표시한다.
- consuming target은 generated object를 `sources`에 넣어 archive/link input으로 소비한다.
- QStar는 외부 compiler의 AST, module system, semantic import/export, package manager,
  internal API를 해석하거나 호출하지 않는다.

## 현재 backend별 동작

| Source kind | Stella | Ninja |
| --- | --- | --- |
| C | compile action | lowered |
| C++ | compile action | lowered |
| ASM | compile action | lowered |
| Generated object | archive/link input | archive/link input |
| Other source suffix | unsupported source diagnostic | unsupported source diagnostic |

## Future 조건

새 언어를 QStar provider로 승격하려면 stable argv, depfile/discovered input,
response-file, generated artifact ownership, action-log/replay, backend parity 계약을 먼저
문서화해야 한다. 그 전까지 built-in `c`/`cxx`/`asm` 외 언어는 object artifact bridge가
정본 경로다.
