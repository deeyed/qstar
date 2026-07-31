# Cookbook: Response Files

QStar는 C/C++/ASM을 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다. Long command
line은 toolset의 response file policy로 다룬다.

## 최소 예제

```lua
qstar.toolset "msvc_rsp" {
  tools = {
    c = { compiler = qstar.cli {"clang-cl"} },
    cxx = { compiler = qstar.cli {"clang-cl"} },
    asm = { compiler = qstar.cli {"clang-cl"} },
    archive = qstar.cli {"llvm-lib"},
    link = qstar.cli {"clang-cl"},
  },
  response_files = "on",
  response_style = "msvc",
}
```

## 전체 예제

```lua
qstar.executable "app" {
  toolset = "//:msvc_rsp",
  sources = {"src/main.c"},
  artifact_name = "app.exe",
  link_options = {
    "/DEBUG",
    "/INCREMENTAL:NO",
  },
}
```

Toolset이 response file을 허용하면 QStar는 long command를 response file로 낮추고 digest와
replay 정보를 안정적으로 남긴다. QStar는 action마다 다음 두 표현을 구분한다.

- **logical argv**: tool role, output option, target object, objectlib object, dependency artifact,
  link option을 모두 포함한 완전한 build 의미다. Action key, local CAS key, `why-rebuild`,
  action log, replay, explain은 항상 이 값을 사용한다.
- **execution argv**: process spawn에 전달하는 표현이다. Response file을 쓰면
  `tool @build/.../rsp/<action>.rsp`처럼 짧아진다.

Response file은 logical argv를 대체하지 않는다. 같은 `.rsp` 경로를 재사용해도 object
목록이나 순서가 바뀌면 logical argv digest, response digest, action key가 모두 바뀐다.
현재 공통 materializer는 logical argv가 512 byte 이상이거나 argc가 48개를 넘으면 긴
command로 분류한다. Stella, Ninja, GLP v1/v2 final, `dry-run`, `explain`이 같은 판단을 쓴다.

단, arbitrary command에 `@file` support를 추론하지 않는다. Compiler,
archive, linker, provider final role은 toolset policy를 자동 적용하지만
`qstar.custom_target`, `qstar.transform`, `qstar.run_target`은 direct argv가
기본이다. 이 command가 response syntax를 구현한 경우에만 exact first argv
tool을 `response_file_tools`에 선언한다.

```lua
qstar.toolset "generators" {
  tools = {
    archive = qstar.cli {"ar"},
    link = qstar.cli {"cc"},
  },
  path_tools = {"python3", "rsp-aware-generator"},
  response_files = "on",
  response_style = "posix",
  response_file_tools = {"rsp-aware-generator"},
}
```

위 toolset에서 긴 `python3` action은 full argv를 받고,
`rsp-aware-generator`만 `@response-file`을 받는다.

`dry-run`과 `explain`은 `.rsp` 파일을 만들지 않으면서 실제 object 수와 순서를 낮춰 다음
metadata를 출력한다.

- `logical_argc`, `logical_bytes`, `logical_argv_digest`
- `object_count`, `input_count`
- `response_file`, `response_style`, `response_digest`
- `exec_argc`

Action log의 기존 `argv[N]`과 `argv_shell`은 full logical argv다. 따라서 `qstar replay`는
이전 build의 `.rsp`가 삭제되어도 logical command를 복원할 수 있다.

Windows/MSVC 계열 toolset은 package path와 별개로 response-file quoting style만 고른다.
QStar DSL의 source/header/output path는 Windows에서도 `/`로 정규화된 package-relative
path를 쓴다. `src\\main.c`나 `C:\\SDK\\include`를 source/include path로 쓰지 않는다.
반대로 `/DWINPATH=C:\\Program Files\\SDK\\Include` 같은 Windows-style 문자열이 실제
compiler argv option인 경우에는 `compile_options`나 `link_options`에 그대로 둘 수 있고,
QStar가 `response_style = "msvc"` 규칙으로 response file에 escape한다.

```lua
qstar.toolset "windows_msvc" {
  tools = {
    c = { compiler = qstar.cli {"clang-cl"} },
    cxx = { compiler = qstar.cli {"clang-cl"} },
    asm = { compiler = qstar.cli {"clang-cl"} },
    archive = qstar.cli {"llvm-lib"},
    link = qstar.cli {"clang-cl"},
  },
  response_files = "on",
  response_style = "msvc",
}
```

## 실패 예제

```lua
qstar.toolset "no_rsp" {
  tools = {
    c = { compiler = qstar.cli {"cc"} },
    cxx = { compiler = qstar.cli {"c++"} },
    asm = { compiler = qstar.cli {"cc"} },
    archive = qstar.cli {"ar"},
    link = qstar.cli {"cc"},
  },
  response_files = "off",
}
```

`response_files = "off"`는 긴 logical argv 자체를 거절한다는 뜻이 아니다. QStar는
platform limit 아래의 command를 full execution argv로 실행한다. 예를 들어 argc가 256개를
넘더라도 byte budget 안이면 허용한다. Limit을 넘을 때만 action label, toolset label, argc,
logical byte 수, host limit을 포함한 stable diagnostic으로 막는다.

- Windows context: `CreateProcess` quoting 후 32,767 character 범위
- POSIX context: `_SC_ARG_MAX`, 현재 environment, action environment, argv pointer와
  headroom을 함께 고려
- limit 조회가 불가능한 host: 보수적인 fallback byte budget

## 관련 CLI

```sh
qstar --file qstar.lua dry-run //:app
qstar --file qstar.lua action-log //:app:link:0
qstar --file qstar.lua replay //:app:link:0
make qstar-windows-prep-tests
```

## 관련 diagnostic

- `response_style=msvc`
- `/LIBPATH:sdk/lib/um/x64`
- `"/DQUOTE=\"value\""`
- `"/DWINPATH=C:\Program Files\QStar\Include"`
- `logical_argv_digest=...`
- `response_digest=...`
- `exec_argc=2`
- `response files are unavailable under toolset '//:no_rsp' and the host command limit is ... bytes`

## Wide Final Action 봉인

Q288 correctness corpus는 0/1/48/49/252/253/256/1,000 object 경계와
4,096-object readiness/performance case를 유지한다.

```sh
make qstar-wide-final-action-tests
make qstar-wide-final-action-safety-tests
make qstar-response-file-capability-tests
QSTAR_LARGE_FINAL_OBJECTS="1000 4096" \
  make qstar-large-project-performance-tests
```

Fake final tool은 response file을 직접 읽어 object 누락, 중복, 순서 변화,
POSIX/MSVC quoting과 output을 검사한다. Dry-run, explain, why-rebuild,
action-log, replay, state cache, local CAS, Ninja emitted command와 실제
response content는 같은 logical action을 설명해야 한다.

Direct source, `compile_context = "own"`, `"consumer"`, 여러 objectlib,
generated/prebuilt object와 GLP v1/v2 provider final 모두 같은 제한 없는
동적 argv contract를 쓴다. `response_files = "off"`는 256개에서 실패하는
정책이 아니며 host command byte limit 아래에서는 full argv로 실행한다.
QStar는 object를 몰래 archive로 묶거나 staticlib를 자동 분할하지 않는다.
