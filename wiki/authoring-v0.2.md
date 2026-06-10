# QStar v0.2 작성 문법

QStar v0.2부터 새 project는 아래 target/rule API를 사용한다.

```lua
qstar.executable "app" { ... }
qstar.staticlib "core" { ... }
qstar.sharedlib "plugin" { ... }
qstar.test "unit" { ... }
qstar.custom_target "generated" { ... }
qstar.run_target "smoke" { ... }
qstar.configure_file "cfg" { ... }
```

## Executable

```lua
qstar.executable "app" {
  sources = {
    "src/main.c",
  },
  deps = {
    "//lib:core",
  },
}
```

## Static library

```lua
qstar.staticlib "core" {
  sources = {
    "lib/src/core.c",
  },
  public_headers = {
    "lib/include/core.h",
  },
  lang = {
    c = {
      public_include_dirs = {
        "lib/include",
      },
    },
  },
}
```

## C++ target

```lua
qstar.executable "tool" {
  sources = {
    "tool/src/main.cpp",
  },
  lang = {
    cxx = {
      standard = "c++20",
      include_dirs = {
        "tool/include",
      },
      compile_options = {
        "-fno-exceptions",
      },
    },
  },
}
```

## Generated file

```lua
qstar.configure_file "cfg" {
  output = qstar.output("generated/config.h"),
  defines = {
    "HAVE_CONFIG=1",
  },
}

qstar.custom_target "generated_source" {
  tool = "tools/gen-source.sh",
  outputs = {
    qstar.output("generated/value.c"),
  },
  args = {
    "generated/value.c",
  },
}

qstar.executable "app" {
  sources = {
    "src/main.c",
    qstar.output("generated/value.c"),
  },
  private_headers = {
    qstar.output("generated/config.h"),
  },
  lang = {
    c = {
      include_dirs = {
        "generated",
      },
    },
  },
}
```

## 제거된 문법

다음 이름은 더 이상 호환 alias가 아니다.

- `qstar.exe`: `qstar.executable` 사용.
- `qstar.genrule`: `qstar.custom_target` 사용.
- `qstar.config_header`: `qstar.configure_file` 사용.
- `qstar.write_config_header`: `qstar.configure_file` 사용.

Target top-level의 `include_dirs`, `public_include_dirs`, `private_include_dirs`,
`system_include_dirs`, `cflags`, `cxxflags`, `cxx_standard`도 제거됐다. 모두 `lang.*`
아래로 옮긴다.

