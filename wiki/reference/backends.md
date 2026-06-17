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
- `qstar.sharedlib` on macOS and Linux platform contexts

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

`qstar.sharedlib`는 macOS platform context에서 `.dylib`와 `install_name`, Linux platform context에서
`.so`와 `soname`을 생성한다. sharedlib dependency를 link하는 executable/test/sharedlib
edge에는 build-tree 실행용 rpath가 자동으로 추가된다. Stella는 실제 argv에
`$ORIGIN`/`@loader_path` rpath를 넣고, Ninja lowering도 같은 의미의 `description`/command
edge를 생성한다. Q168 regression gate는 sharedlib-linked executable/test 실행,
sharedlib stage/install artifact 처리를 함께 확인한다. Windows platform context에서는
runtime `.dll`과 import `.lib`가 같은 `link-shared` action의 outputs로 생성되고,
dependent executable/test/sharedlib edge는 import `.lib`를 link input으로 사용한다.
PDB/debug ownership과 일반 Windows runtime search path 정책은 아직 deferred다.

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

## Windows Sharedlib 예제

```lua
qstar.sharedlib "plugin" {
  sources = {"src/plugin.c"},
}

qstar.executable "plugin_app" {
  sources = {"src/main.c"},
  deps = {"//:plugin"},
}

qstar.stage "layout" {
  root = "stage/plugin",
  files = {
    qstar.stage_file(qstar.target_file("//:plugin"), "bin/plugin.dll"),
    qstar.stage_file(qstar.target_file("//:plugin", { artifact = "import_lib" }),
      "lib/plugin.lib"),
  },
}
```

```sh
qstar --file qstar.lua --qstar-internal-platform windows -G ninja emit-ninja //:plugin_app
```

Windows shared library artifact map은 runtime `.dll`을 primary artifact로 두고,
`artifact = "import_lib"` selector로 import `.lib`를 노출한다. Ninja와 Stella는 consumer
link edge에서 runtime `.dll`이 아니라 import `.lib`를 사용한다.

## 관련 CLI

```sh
qstar --file qstar.lua emit-ninja //:app
qstar --file qstar.lua -G ninja build //:app
qstar --file qstar.lua -G ninja test //:unit
qstar --file qstar.lua -G ninja stage //:bundle
qstar --file qstar.lua -G ninja install //:app --prefix /tmp/qstar-install
```

## 관련 diagnostic

- `target_file artifact selector 'pdb' is unknown for target '//:plugin'`
