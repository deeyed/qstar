# Local Action Cache와 Hermeticity Audit

이 문서는 QStar의 opt-in local content-addressed action cache(CAS)와 report-only
hermeticity audit 계약을 정의한다. 이 기능은 strict sandbox, remote cache, remote execution을
도입하지 않는다. 기본 빌드 결과, target artifact 경로, generated output 경로도 바꾸지 않는다.

## 활성화

기본값은 `off`다. 프로젝트가 반복 빌드에서 local CAS를 사용하려면 root `qstar.lua`에서
다음처럼 명시한다.

```lua
qstar.project {
  name = "demo",
  root = ".",
  action_cache = "local",
}
```

한 invocation에서만 바꾸려면 build option을 사용한다.

```sh
qstar build //:app --action-cache local
qstar build //:app --action-cache off
```

CLI 값은 project policy보다 우선한다. `remote` 같은 다른 값은 허용하지 않는다.

## 선언별 cacheability

Artifact target, `qstar.test`, `qstar.objectlib`, `qstar.config`,
`qstar.custom_target`, `qstar.transform`, `qstar.configure_file`은 strict boolean
`cacheable` field를 받는다. 기본값은 `true`다.

```lua
qstar.config "local_machine_policy" {
  cacheable = false,
}

qstar.transform "device_snapshot" {
  input = "inputs/request.bin",
  output = qstar.output("generated/snapshot.bin"),
  command = qstar.cli {"tools/snapshot", qstar.input(0), qstar.output(0)},
  cacheable = false,
}
```

`cacheable = true`는 강제 cache hit 약속이 아니다. 선언이 cache 후보가 될 수 있다는 뜻이다.
실제 판정은 action kind, output ownership, tool resolution, external interaction heuristic까지
검사한다. `cacheable = false`는 언제나 우선하며 해당 action을 CAS lookup/store에서 제외한다.

## 현재 cache 대상

현재 local CAS가 저장하고 복원하는 action은 다음 둘뿐이다.

- built-in 또는 GLP source compile action
- `qstar.custom_target`/`qstar.transform` 계열 generated file action

Archive, link, stage, test/run, project command step, provider final artifact action은 이번 계약에서
cache 대상이 아니다. 대상이 아닌 action도 local mode의 audit 통계에는
`action-kind-not-supported`로 나타날 수 있다. 이 제한은 잘못된 재사용보다 좁고 명확한
cacheability를 우선하기 위한 것이다.

Regular file output만 CAS에 저장한다. Tree output이나 소유권이 불명확한 output은
`non-file-output` 또는 `no-owned-outputs`로 제외한다.

## Content Key

Action key에는 다음 material이 들어간다.

| Material | 의미 |
| --- | --- |
| action id와 kind | 서로 다른 owner/action의 결과 충돌 방지 |
| 전체 argv vector | option과 명시 path 변화 추적 |
| declared input과 depfile-discovered input content | source/generated input 및 header 변화 추적 |
| declared output identity | materialization destination과 output 순서 고정 |
| tool executable path와 file content | compiler/generator binary 또는 wrapper 변경 추적 |
| allowlisted process env와 explicit action env | 실행 환경 변화 추적 |

Process env allowlist는 현재 `PATH`, `SDKROOT`, `CPATH`, `LIBRARY_PATH`다. Provider action이
명시한 env도 key에 포함된다. Audit/action-log에는 env 값 자체를 출력하지 않고 fingerprint나
redacted metadata만 남긴다.

Tool은 실제 executable file로 resolve되어야 한다. PATH tool과 package-local tool 모두 실제
실행 파일 내용이 fingerprint에 들어간다. Resolve할 수 없는 tool은
`tool-unresolved`로 non-cacheable이다.

## External Interaction Exclusion

다음 action은 선언이 `cacheable = true`여도 자동으로 제외한다.

- 알려진 외부 system emulator 또는 `hil` 성격의 tool 실행
- argv가 `/dev/...`, `/dev/tty...`, `/Volumes/...`를 참조하는 실행
- argv에 `serial` 또는 `hardware-in-loop` 성격의 token이 있는 실행

이는 특정 도메인 target을 만드는 문법이 아니다. Local process가 QStar가 소유하지 않는
장치, mounted volume, 외부 runtime state와 상호작용할 가능성이 높은 경우 재사용을 막는
보수적 policy다. 필요한 프로젝트는 명시적으로 `cacheable = false`를 사용해야 하며, 이
heuristic을 cacheability 보증으로 간주해서는 안 된다.

## Report-Only Hermeticity Audit

QStar는 이번 버전에서 process filesystem access를 sandbox로 가로채지 않는다. 따라서 audit은
declared inputs/outputs와 argv에서 보이는 path를 비교한다.

```text
hermeticity_audit id=//:asset:generate:0 kind=generate enforcement=report-only cacheable=true reason=eligible ... undeclared-path=tools/counter report-only
```

`undeclared-path=...`는 경고성 관찰 결과이며 build error가 아니다.
`undeclared-access=unobserved`도 완전한 hermeticity 증명이 아니다. Process가 argv에 나타나지
않는 file/env/device를 읽었을 수 있다. Strict sandbox와 undeclared access enforcement는 별도
후속 설계다.

## 저장, 복원, 손상 처리

CAS는 effective build directory 아래 `cas/v1/<content-key>/`에 manifest와 numbered blob을
저장한다. Target 단위 `qstar clean //:label`은 CAS를 유지하므로 output이 지워진 뒤 복원할 수
있다. 전체 build directory clean은 그 안의 local CAS도 제거한다.

Restore 전에 manifest, output count/order, blob content digest를 검증한다. Entry가 잘렸거나
blob digest가 다르면 해당 entry를 폐기하고 `corrupt-entry` miss로 처리한 뒤 action을 정상
실행한다. 손상은 stale output을 materialize하는 이유가 되지 않는다.

Stella는 cache hit action을 scheduler에서 skip하고 output을 materialize한다. Ninja backend는
emit 시 같은 resolver로 output을 materialize하고, 해당 edge command를 platform no-op으로
바꿔 Ninja의 log/mtime 판단이 compiler나 generator를 다시 실행하지 않게 한다. Action이
실제로 실행된 뒤에는 두 backend 모두 같은 CAS store contract를 사용한다.
Compile action의 depfile도 내부 CAS metadata로 보존한다. Output/depfile이 target clean으로
사라진 경우 dependency alias가 depfile을 먼저 복원하고 현재 header content로 full key를 다시
검증한다. Header가 바뀌면 `dependency-changed` miss로 처리하며 object를 복원하지 않는다.

## 관찰성

`--explain-cache`를 켜면 action별 audit, lookup, store 결과와 최종 통계를 출력한다.

```text
local_cache_stats backend=stella mode=local audited=2 eligible=1 non_cacheable=1 hits=1 misses=0 stores=0 corruptions=0
```

| Field | 의미 |
| --- | --- |
| `audited` | cacheability를 판정한 action 수 |
| `eligible` | local CAS 후보 수 |
| `non_cacheable` | 선언 또는 policy로 제외된 action 수 |
| `hits` | 검증 후 output을 materialize한 entry 수 |
| `misses` | eligible action 중 entry 없음 또는 손상 수 |
| `stores` | 새 entry를 commit한 action 수 |
| `corruptions` | 검증 실패로 폐기한 entry 수 |

`qstar explain`, `qstar query`, `qstar list-targets --format json`, graph dump에는 project
`action_cache` policy와 target/generated `cacheable` 값이 나타난다. Fingerprint와 cache hit
통계는 action 실행 시점 값이므로 build output에서 확인한다.

## 범위 밖

- remote cache protocol
- remote execution
- strict filesystem/network sandbox
- undeclared access의 기본 error 승격
- device/HIL result의 재사용
- link/archive/final artifact action cache

이 경계를 넓히려면 action ownership, tree artifact, sandbox observation을 별도 versioned
contract로 먼저 정의해야 한다.

## 회귀 게이트

```sh
make qstar-local-action-cache-tests
```

이 gate는 Stella/Ninja clean build, incremental skip, output 제거 뒤 cache hit, blob corruption,
tool executable 변경, generated action reuse, config/target `cacheable = false`, external runtime
제외, CLI override와 strict schema diagnostic을 검증한다.
