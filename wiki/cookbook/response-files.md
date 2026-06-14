# Cookbook: Response Files

QStar는 C/C++/Cale을 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다. Long command
line은 toolchain profile capability와 response file policy로 다룬다.

## 최소 예제

```lua
qstar.profile "msvc-uefi" {
  response_files = "on",
  response_style = "msvc",
}
```

## 전체 예제

```lua
qstar.executable "boot" {
  sources = {"src/efi_main.c"},
  artifact_name = "BOOTX64.EFI",
  link_options = {
    "/subsystem:efi_application",
    "/entry:efi_main",
    "/nodefaultlib",
  },
}
```

Profile이 response file을 허용하면 QStar는 long command를 response file로 낮추고 digest와
replay 정보를 안정적으로 남긴다.

Windows/MSVC 계열 profile은 package path와 별개로 response-file quoting style만 고른다.
QStar DSL의 source/header/output path는 Windows에서도 `/`로 정규화된 package-relative
path를 쓴다. `src\\main.c`나 `C:\\SDK\\include`를 source/include path로 쓰지 않는다.
반대로 `/DWINPATH=C:\\Program Files\\SDK\\Include` 같은 Windows-style 문자열이 실제
compiler argv option인 경우에는 `compile_options`나 `link_options`에 그대로 둘 수 있고,
QStar가 `response_style = "msvc"` 규칙으로 response file에 escape한다.

```lua
qstar.profile "windows-msvc" {
  target = "x86_64-pc-windows-msvc",
  cc = "clang-cl",
  linker = "clang-cl",
  response_files = "on",
  response_style = "msvc",
}
```

## 실패 예제

```lua
qstar.profile "no-rsp" {
  response_files = "off",
}
```

Command가 platform limit을 넘는데 response file을 사용할 수 없으면 stable diagnostic으로
막아야 한다.

## 관련 CLI

```sh
qstar --file qstar.lua dry-run //:boot
qstar --file qstar.lua action-log //:boot:link:0
qstar --file qstar.lua replay //:boot:link:0
make qstar-windows-prep-tests
```

## 관련 diagnostic

- `response_style=msvc`
- `/LIBPATH:sdk/lib/um/x64`
- `"/DQUOTE=\"value\""`
- `"/DWINPATH=C:\Program Files\QStar\Include"`
- `argv_digest=...`
- `response file is not supported by this toolchain profile`
