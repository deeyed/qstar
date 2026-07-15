# C++ Language Options

QStar는 C/C++/ASM과 external object artifact flow를 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다. C++ source는
`lang.cxx` 아래에서 standard, include와 opt-in build strategy를 설정한다. 모든 strategy는
기본값이 off이고 C, ASM, external GLP action에는 적용되지 않는다.

## 최소 예제

```lua
lang = {
  cxx = {
    standard = "c++20",
  },
}
```

## 전체 예제

```lua
qstar.executable "tool" {
  sources = {
    "src/math.cppm",
    "src/math_use.cpp",
    "src/main.cpp",
  },
  lang = {
    cxx = {
      standard = "c++20",
      public_headers = {"include/tool.hpp"},
      public_include_dirs = {"include"},
      compile_options = {"-fno-exceptions"},
      defines = {"TOOL_BUILD=1"},
      precompiled_header = "include/pch.hpp",
      unity = {
        enabled = true,
        batch_size = 8,
      },
      modules = {
        enabled = true,
      },
    },
  },
}
```

`precompiled_header`는 target마다 PCH action 하나를 만든다. Clang 계열은
`-include-pch`, GCC는 `.gch` + `-include` 계약을 사용한다. PCH action과 이를 소비하는
compile action은 같은 standard, compile options, include와 usage requirement를 사용한다.

`unity.enabled = true`이면 일반 `.cc`, `.cpp`, `.cxx` implementation source만 선언 순서대로
batching한다. 생성 translation unit은 build tree 아래에 있고 원본 source ownership은 바뀌지
않는다. `.cppm`/`.ixx`, C, ASM, provider source, 다른 target source는 batch에 섞이지 않는다.
`compile_commands.json`에는 PCH, module interface, 생성 unity translation unit이 각각 실제
compiler argv로 기록된다.

`modules.enabled = true`이면 `.cppm`과 `.ixx`를 module interface로, 일반 C++ source를
implementation unit으로 구분한다. Clang에서는 interface마다 object와 `.pcm` BMI를 함께
생성한다. Interface는 source 선언 순서대로 앞선 BMI에 의존하고 implementation unit은 모든
interface BMI에 의존한다. 현재 interface 파일 basename이 BMI lookup 이름이므로 basename은
target 안에서 유일해야 하고 `src/math.cppm`은 `export module math;`를 선언해야 한다. 다른
interface를 import하는 interface는 의존 대상보다 뒤에 `sources`로 선언한다. 정확한 import
graph scanner 대신 선언 순서를 이용한 안전한 dependency superset을 사용하며, 각 BMI는
`-fmodule-file=<name>=<BMI>`로 compiler에 명시적으로 전달한다.
PCH를 함께 켠 경우에도 module interface action에는 PCH를 주입하지 않는다. 일반
implementation과 unity action은 계속 PCH를 사용한다. 이 경계는 compiler 버전에 따라 PCH
상태가 BMI 내용에 섞이는 문제를 피하기 위한 정식 계약이다.

PCH와 unity는 Clang/GCC 계열에서 지원한다. C++ module lowering은 upstream Clang과 C++20
이상을 요구한다. GCC modules-ts, Apple Clang module mode, MSVC와 compiler family를 판별할
수 없는 wrapper는 명확한 capability diagnostic으로 거절한다. Wrapper를 쓸 때는 strategy가
인식할 수 있는 직접 Clang/GCC compiler role을 toolset에 두어야 한다.

## 실패 예제

```lua
qstar.executable "bad" {
  sources = {"src/module.cppm"},
  lang = {
    cxx = {
      standard = "c++17",
      modules = { enabled = true },
    },
  },
}
```

## 관련 CLI

```sh
qstar --file qstar.lua lint //:tool
qstar --file qstar.lua dry-run //:tool
qstar --file qstar.lua build //:tool
```

## 관련 diagnostic

- `lang.cxx.modules requires standard = "c++20" or newer`
- `lang.cxx.modules requires Clang in this release`
- `C++ build strategies require a directly configured Clang or GCC compiler`
- `QSTAR044 C++ source has no cxx_standard`
- `unknown target field 'cxx_standard'`
