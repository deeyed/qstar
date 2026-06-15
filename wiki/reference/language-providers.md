# Language Providers

QStar는 특정 언어에 종속되지 않는 빌드시스템이다. QStar core는 C/C++/ASM compile
provider만 직접 소유한다. 그 밖의 언어는 source suffix나 언어별 namespace를 QStar DSL에
추가하지 않고, 외부 compiler가 object artifact를 만들게 한 뒤 그 object를 consuming
target에 연결한다. 이 경계를 object artifact bridge라고 부른다.

## 최소 예제

```lua
qstar.executable "app" {
  sources = {"src/main.c"},
}
```

## 전체 예제

```lua
qstar.custom_target "foreign_obj" {
  inputs = {"src/foreign.source"},
  outputs = {qstar.output("generated/foreign.o", {format = "object"})},
  command = qstar.cli {"tools/compile-foreign.sh", qstar.input(0), qstar.output(0)},
}

qstar.executable "app" {
  sources = {
    "src/main.c",
    qstar.output("generated/foreign.o"),
  },
}
```

## 실패 예제

```lua
qstar.executable "bad" {
  sources = {"src/main.c", "src/foreign.source"},
}
```

외부 언어 source를 `sources`에 직접 넣지 않는다. 외부 compiler를 호출하는
`qstar.custom_target`을 만들고 `qstar.output(path, {format = "object"})` output을 consuming
target의 `sources`에 넣는다.

## 계약

- QStar는 외부 언어의 AST, module system, header semantics를 해석하지 않는다.
- 외부 compiler 호출은 `qstar.custom_target`과 `qstar.cli` argv-vector로 표현한다.
- 생성된 object는 `qstar.output(path, {format = "object"})`로 표시한다.
- Stella와 Ninja backend는 generated object artifact를 link/archive input으로 소비한다.
- 언어별 package manager, semantic import/export, compiler internal API 호출은 QStar 책임이 아니다.

## 관련 CLI

```sh
qstar --file qstar.lua dry-run //:app
qstar --file qstar.lua build //:app
qstar --file qstar.lua -G ninja build //:app
```

## 관련 diagnostic

- `this language is not a QStar compile provider`
- `qstar.output(..., {format = "object"})`

## 관련 문서

- `wiki/reference/object-artifacts.md`
- `wiki/reference/custom-target.md`
