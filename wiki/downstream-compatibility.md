# Downstream Compatibility Gate

QStar는 stable 후보 문법이 실제 외부 프로젝트에서 계속 해석되는지 확인하는 opt-in
호환성 gate를 제공한다. 이 gate는 특정 프로젝트의 도메인 의미를 QStar core로 가져오지
않는다. 기존 target, object library, generated action, stage, test, project command의
label, artifact path, dry-run argv가 유지되는지만 검사한다.

정본 설계와 실행 환경 변수는 source tree의
[`docs/downstream-safe-upgrade.md`](https://github.com/deeyed/qstar/blob/main/docs/downstream-safe-upgrade.md)에
둔다.

## 실행

```sh
make qstar-downstream-safe-upgrade-tests
```

외부 checkout 경로가 없으면 gate는 명확한 skip 결과를 낸다. 경로가 주어지면 원본을
수정하지 않고 `${TMPDIR}` 아래 복사본에서 다음을 확인한다.

- `qstar check //...`
- target, generated action, stage, command compatibility snapshot
- 대표 artifact path
- Stella/Ninja dry-run의 `command_argv` parity
- 명시적으로 opt-in한 대표 build와 smoke

snapshot은 compatible subset이다. 기존 계약의 삭제·rename·argv 변경은 실패하고,
새 target과 command의 additive 확장은 허용한다.

## 범위

이 gate는 새 DSL을 추가하지 않는다. LSP completion/hover도 기존 generic surface를
그대로 사용한다. 외부 프로젝트의 이름, tag, metadata 값은 QStar builtin 의미론이
아니다. 실제 release asset publish/download와 host별 product claim도 별도 release gate가
담당한다.
