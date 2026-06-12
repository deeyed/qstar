# Backends

QStar has two user-facing generators:

- `stella`: the default QStar executor
- `ninja`: a Ninja backend for supported C/C++/ASM project graphs

`auto` currently resolves to `stella`.

## Ninja가 지원하는 것

`-G ninja`는 다음을 lower한다.

- C/C++/ASM compile
- `qstar.staticlib`
- `qstar.executable`
- `qstar.test`
- `qstar.configure_file`
- `qstar.custom_target`
- `qstar.run_target`
- `qstar.group`

`compile_commands.json`는 `qstar.project.compile_commands` policy를 따른다.
`build.ninja`는 `build_dir/ninja/build.ninja`에 생성된다.

## Stage와 Install

`stage`와 `install`의 copy, diff, manifest 작성은 QStar가 계속 수행한다. 다만
effective generator가 `ninja`이면 참조된 artifact는 먼저 Ninja backend로 build한다.

이 구조 덕분에 packaging 정책은 QStar에 남기고, artifact 생산은 Ninja에 맡길 수 있다.

## Action Log와 Replay

Ninja edge에는 `qstar_action_id`가 기록된다. QStar는 Ninja lowering 중에도 action log를
남기므로 다음 명령을 사용할 수 있다.

```sh
qstar --file qstar.lua -G ninja build //:app
qstar --file qstar.lua action-log //:app:link:0
qstar --file qstar.lua replay //:app:link:0
```

## Deferred

`qstar.sharedlib`는 현재 Stella/Ninja 모두에서 plan/check-only다. Platform별 shared
library naming, soname/install-name, import library, rpath 정책이 정리되기 전까지는
partial lowering을 제공하지 않는다.

Cale source action도 아직 Ninja로 lower되지 않는다. Cale source를 포함하는 target은
`-G stella`를 사용한다.

## 최소 예제

```lua
qstar.executable "app" {
  sources = {"src/main.c"},
}
```

```sh
qstar --file qstar.lua -G ninja build //:app
```

## 전체 예제

```lua
qstar.staticlib "core" {
  sources = {"src/core.c"},
}

qstar.executable "app" {
  sources = {"src/main.c"},
  deps = {"//:core"},
}

qstar.group "all" {
  deps = {"//:app"},
}
```

## 실패 예제

```lua
qstar.sharedlib "plugin" {
  sources = {"src/plugin.c"},
}
```

```sh
qstar --file qstar.lua -G ninja build //:plugin
```

`sharedlib`는 아직 Ninja lowering 대상이 아니므로 stable diagnostic을 낸다.

## 관련 CLI

```sh
qstar --file qstar.lua emit-ninja //:app
qstar --file qstar.lua -G ninja build //:app
qstar --file qstar.lua -G ninja test //:unit
qstar --file qstar.lua -G ninja stage //:bundle
qstar --file qstar.lua -G ninja install //:app --prefix /tmp/qstar-install
```

## 관련 diagnostic

- `qstar: sharedlib target '//:plugin' is not lowered by the ninja backend yet`
- `qstar: ninja backend does not lower Cale source 'src/unit.cale' yet; use -G stella`
