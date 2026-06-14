# QStar

[English](README.md) | 한국어

QStar는 Lua 기반 프로젝트 DSL, Stella 네이티브 executor, 선택형 Ninja backend를 갖춘
독립 빌드시스템이다. 목표는 deterministic build graph, 명시적 argv-vector command,
대형 프로젝트용 reusable config, 도구가 읽기 쉬운 diagnostic을 제공하는 것이다.

현재 공개 버전은 베타다. 현재 release-prep line은 `v0.7.0-beta`이며 macOS arm64와
Linux x86_64 runtime tarball을 대상으로 한다. Linux asset은 Ubuntu release workflow 또는
clean Linux x86_64 host에서 source validation, Ninja backend parity, extracted tarball smoke,
Stella/Ninja medium performance artifact collection을 통과한 산출물만 사용한다. Windows는
manual native CI alpha 단계이며 아직 public asset이나 official host support가 아니다.
`0.6.x-beta` line은 release/package/documentation hotfix용 patch line으로 남긴다.
QStar 1.0은 macOS, Linux, Windows 공식 지원이 모두 갖춰진 뒤에 올린다.

## 특징

- 진입점은 `qstar.lua` 하나
- `.qst` fragment와 `.qsm` helper module
- `qstar.import_file(...)`, `qstar.import_module(...)` 기반의 명시적 import
- executable, staticlib, sharedlib, test, custom target, configure file, group,
  stage/install 지원
- 반복 옵션을 줄이는 `qstar.config`
- Stella native executor와 C/C++/ASM/generated graph용 `-G ninja` backend
- 반복 로컬 빌드와 IDE read API를 위한 Stella daemon beta opt-in workflow
- Cale source는 Stella language-provider process action으로 지원
- `compile_commands.json`, LSP, VSCode extension, replay/action log, manpage,
  AI index 제공

## 설치

GitHub Releases에서 host에 맞는 runtime tarball을 내려받는다.

```sh
# macOS arm64
tar -xzf qstar-v0.7.0-beta-macos-arm64.tar.gz -C "$HOME/.local"
export PATH="$HOME/.local/bin:$PATH"
qstar --version

# Linux x86_64
tar -xzf qstar-v0.7.0-beta-linux-x86_64.tar.gz -C "$HOME/.local"
export PATH="$HOME/.local/bin:$PATH"
qstar --version
```

소스에서 직접 빌드하려면 다음을 사용한다.

```sh
git clone --recurse-submodules https://github.com/deeyed/qstar.git
cd qstar
make all
build/bin/qstar --version
```

이미 submodule 없이 clone했다면 vendored Lua runtime을 먼저 초기화한다.

```sh
git submodule update --init --recursive
```

Makefile은 계속 canonical bootstrap/release build path다. QStar self-host graph는
Stella/Ninja backend parity를 확인하기 위한 병행 검증 경로로 유지한다.

```sh
make qstar-self-host-tests
build/bin/qstar --file qstar.lua -B build/qstar-self build //:qstar
build/bin/qstar --file qstar.lua -B build/qstar-self-ninja -G ninja build //:qstar
```

릴리즈 담당자는 local source tree에서 만든 tarball뿐 아니라 GitHub에 업로드된 release
asset도 다음 명령으로 검증한다.

```sh
make qstar-public-beta-download-smoke
```

## 빠른 시작

```sh
qstar init c-app hello
cd hello
qstar build //:app --progress plain
./build/qstar/out/___app/app
```

최소 `qstar.lua`는 다음과 같다.

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

빌드 디렉터리와 backend는 CLI에서 바꿀 수 있다.

```sh
qstar -B build/stella -G stella build //:app
qstar -B build/ninja -G ninja build //:app
```

## Stella Daemon

QStar에는 문서화된 beta opt-in daemon workflow가 있다. 기본 `qstar build`는 여전히 일반
Stella executor를 사용하며, daemon residency는 명시적으로 켤 때만 사용한다.

```sh
qstar daemon --socket build/qstar/stella/daemon/qstar-daemon.sock --start
qstar --file qstar.lua -B build/qstar build //:app --use-daemon=auto --daemon-socket build/qstar/stella/daemon/qstar-daemon.sock
qstar --file qstar.lua -B build/qstar daemon --socket build/qstar/stella/daemon/qstar-daemon.sock --query targets.list
qstar daemon --socket build/qstar/stella/daemon/qstar-daemon.sock --stop
```

read API는 IDE/AI tooling을 위한 것이며 현재 `hello`, `workspace.info`, `targets.list`,
`diagnostics.list`, `compile_commands.path`, `build.summary`를 제공한다. Socket/pid/lock file은
local-only, owner-only 정책을 따른다. Windows named pipe는 아직 deferred다.

## 작성 예시

```lua
qstar.import_file("qstar/policies/freestanding.qst")
local paths = qstar.import_module("qstar/modules/paths")

qstar.config "common_c" {
  lang = {
    c = {
      public_include_dirs = {"include"},
      compile_options = {"-std=c23", "-Wall"},
    },
  },
}

qstar.staticlib "core" {
  configs = {"//qstar/policies:common_c"},
  sources = {"src/core.c"},
}

qstar.group "all_libs" {
  deps = {
    "//:core",
  },
}
```

외부 command는 shell string이 아니라 argv-vector로 쓴다.

```lua
qstar.custom_target "image" {
  inputs = {qstar.target_file("//:kernel")},
  outputs = {qstar.output("generated/kernel.img")},
  command = qstar.cli {
    "llvm-objcopy",
    "-O", "binary",
    qstar.input(0),
    qstar.output(0),
  },
}
```

## 주요 명령

```sh
qstar --version
qstar docs
qstar docs --path
qstar docs --ai-index
qstar docs --show reference/qstar-lua.md

qstar init c-app hello
qstar check //...
qstar lint //...
qstar fmt --check qstar.lua
qstar list-targets --format json
qstar query //:app
qstar doctor
qstar explain //:app
qstar dry-run //:app
qstar emit-ninja //:app
qstar build //:app
qstar test //...
qstar install //:app --prefix /tmp/qstar-install
qstar stage //:image --dry-run
qstar why-rebuild //:app
qstar clean --target //:app
qstar log //:app
qstar last-failure
qstar action-log <action-id>
qstar replay <action-id>
```

## 플랫폼 상태

| Host platform | 상태 |
| --- | --- |
| macOS arm64 | 0.7 beta release-prep artifact |
| Linux x86_64 | Ubuntu release workflow 또는 clean Linux host 산출 0.7 beta release-prep artifact |
| Windows | MSYS2 UCRT64 기반 manual native CI alpha, public asset 없음 |

QStar는 freestanding, firmware-style cross build graph를 표현할 수 있지만, host
지원 선언은 보수적으로 가져간다. 1.0은 macOS, Linux, Windows release artifact와 CI가
갖춰진 뒤에 올린다.

## 문서

- [GitHub Wiki](https://github.com/deeyed/qstar/wiki)
- [AI Index](wiki/AI_INDEX.md)
- [Getting Started](wiki/getting-started.md)
- [QStar Lua Reference](wiki/reference/qstar-lua.md)
- [Modules and Imports](wiki/reference/modules.md)
- [Reusable Configs](wiki/reference/configs.md)
- [CMake migration notes](wiki/migration/from-cmake.md)

## 라이선스

QStar 본체는 Apache License, Version 2.0으로 배포한다. 자세한 내용은
[LICENSE.md](LICENSE.md)를 본다.

QStar는 Lua를 submodule로 vendor한다. Lua license 전문은
[LICENSE/lua.txt](LICENSE/lua.txt)에 보존한다.
