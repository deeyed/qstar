# 언어별 옵션

QStar는 언어 중립 build system을 목표로 한다. 그래서 include path나 compile flag를
target top-level에 두지 않고 `lang.*` namespace 아래에 둔다.

## C

```lua
lang = {
  c = {
    include_dirs = {"src"},
    public_include_dirs = {"include"},
    private_include_dirs = {"src/private"},
    system_include_dirs = {"/opt/sdk/include"},
    compile_options = {"-ffreestanding"},
    defines = {"CORE_BUILD=1"},
  },
}
```

`public_include_dirs`는 public dependency consumer에게 전파된다. `private_include_dirs`
는 owner target compile에만 쓰인다.

## C++

```lua
lang = {
  cxx = {
    standard = "c++20",
    include_dirs = {"include"},
    compile_options = {"-fno-exceptions"},
    defines = {"TOOL_BUILD=1"},
  },
}
```

C++ source가 있는데 `lang.cxx.standard`가 없으면 lint는 `QSTAR044` info를 낸다.
빌드는 compiler 기본 C++ mode로 계속 진행할 수 있다.

## Assembly

```lua
lang = {
  asm = {
    include_dirs = {"boot/include"},
    compile_options = {"-ffreestanding"},
    preprocess = true,
  },
}
```

v0.2에서 `lang.asm`은 parser/plan surface다. 실제 assembler executor는 후속 round에서
연다.

## Cale

```lua
lang = {
  cale = {
    profile = "safe",
    compile_options = {},
    hcl_include_dirs = {"include"},
  },
}
```

QStar는 `.cl`, `.cale`, `.hcl`의 언어 의미론을 해석하지 않는다. `hcl_include_dirs`는
Cale compiler/HCL checker에 전달할 계획 surface다.

## Future provider

Rust, Zig, Go 같은 future language provider는 include directory 개념을 강제로 갖지
않는다. 필요한 option은 해당 provider namespace에 새로 정의한다.

