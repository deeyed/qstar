# QStar Wiki

이 wiki는 QStar를 처음 쓰는 사용자나 다른 AI가 repo의 긴 개발 문맥 없이도
QStar project를 작성할 수 있게 만드는 한국어 실사용 문서다.

QStar는 CMake처럼 build graph와 command 실행을 맡는 도구다. C, C++, Cale source를
특수하게 더 잘 지원하지만, C/C++/Cale 의미론을 직접 파싱하는 compiler는 아니다.
HCL도 QStar 입장에서는 header path일 뿐이며, HCL 해석은 Cale/HCL checker가 맡는다.
UEFI/RPi/firmware 같은 특수 artifact 흐름도 dedicated keyword가 아니라
`qstar.custom_target`, `qstar.cli`, `qstar.stage`, `qstar.run_target` 조합으로 표현한다.

## 빠른 시작

```sh
make -C qstar
qstar/build/bin/qstar init c-app /tmp/qstar-hello
qstar/build/bin/qstar --file /tmp/qstar-hello/qstar.lua build //:app
/tmp/qstar-hello/.qstar/out/___app/app
```

직접 작성할 때 package root에는 `qstar.lua`를 둔다.

```lua
qstar.project {
  name = "hello",
  version = "0.1.0",
  root = ".",
}

qstar.executable "app" {
  sources = {
    "src/main.c",
  },
}
```

검사와 탐색은 다음 명령을 쓴다.

```sh
qstar/build/bin/qstar --file qstar.lua lint //...
qstar/build/bin/qstar --file qstar.lua list-targets
qstar/build/bin/qstar --file qstar.lua explain //:app
qstar/build/bin/qstar --file qstar.lua dry-run //:app
qstar/build/bin/qstar --file qstar.lua build //:app
```

## 문서 목록

- `authoring-v0.2.md`: QStar v0.2 정본 문법과 target/rule API.
- `language-options.md`: `lang.c`, `lang.cxx`, `lang.asm`, `lang.cale` option.
- `project-layout.md`: `qstar.lua`, `<folder>.qst`, package-root/source-dir style.
- `../../docs/qstar/qstar-v0.2-release-candidate-seal.md`: v0.2 RC에서 stable로 보는
  기능과 아직 experimental인 기능.

## 현재 한계

QStar v0.2는 독립 build system으로 사용할 수 있는 개발용 surface다. C/C++ compile,
staticlib archive, exe/test link, generated action, config header, install/stage/test,
incremental rebuild, compile database, VSCode/LSP authoring 지원을 제공한다.

Round 60 기준 v0.2 RC gate는 `make -C qstar qstar-v0.2-rc-tests`다. 이 gate는 QStar
binary, sample/corpus, lint/LSP, VSCode package, executor, cache/replay, systems
firmware 흐름을 함께 검증한다.

아직 remote package fetch, Ninja generator, full sharedlib executor, standalone assembler
tool profile, Cale compiler 내부 API integration은 정식 surface가 아니다.
