# Backends

QStar has two user-facing generators:

- `stella`: the default QStar executor
- `ninja`: a Ninja backend for supported C/C++/ASM project graphs

`auto` currently resolves to `stella`.

Persistent Stella daemon은 별도 generator가 아니다. Q154 기준 documented beta opt-in
candidate이며,
`qstar daemon --socket ... --start|--stop`과
`qstar build --use-daemon=auto|never|always --daemon-socket ...` 형태로 Stella executor를
보조한다. 기본 `qstar build`는 여전히 normal Stella path를 사용한다. 자세한 내용은
[Stella Daemon](stella-daemon.md)에 둔다.

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
- `qstar.sharedlib` on Darwin-like and Linux-like profiles

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

## Platform Policy

`qstar.sharedlib`는 Darwin-like profile에서 `.dylib`와 `install_name`, Linux-like profile에서
`.so`와 `soname`을 생성한다. sharedlib dependency를 link하는 executable/test/sharedlib
edge에는 build-tree 실행용 rpath가 자동으로 추가된다. Stella는 실제 argv에
`$ORIGIN`/`@loader_path` rpath를 넣고, Ninja lowering도 같은 의미의 `description`/command
edge를 생성한다. Windows-like profile의 `.dll`/import-library/PDB/install layout은 아직
deferred이며 Stella/Ninja 모두 같은 diagnostic으로 거부한다.

Cale source action은 Stella-only language-provider action이다. Ninja wrapper lowering은
이번 release에서 deferred이며, Cale source를 포함하는 target은 `-G stella`를 사용한다.
QStar는 Cale/HCL 의미론을 해석하지 않는다.

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
qstar.profile "windows" {
  target = "x86_64-pc-windows-msvc",
  toolchain = "clang",
}

qstar.sharedlib "plugin" {
  sources = {"src/plugin.c"},
}
```

```sh
qstar --file qstar.lua --profile windows -G ninja build //:plugin
```

Windows shared library policy는 아직 deferred이므로 stable diagnostic을 낸다.

## 관련 CLI

```sh
qstar --file qstar.lua emit-ninja //:app
qstar --file qstar.lua -G ninja build //:app
qstar --file qstar.lua -G ninja test //:unit
qstar --file qstar.lua -G ninja stage //:bundle
qstar --file qstar.lua -G ninja install //:app --prefix /tmp/qstar-install
```

## 관련 diagnostic

- `qstar: sharedlib target '//:plugin' supports only Darwin and Linux-like profiles in this release; Windows .dll/import-library policy is deferred`
- `qstar: Cale source 'src/unit.cale' is a Stella-only language-provider action in this release; Ninja wrapper lowering is deferred; use -G stella`
