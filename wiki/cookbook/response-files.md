# Cookbook: Response Files

QStar는 C/C++/Cale을 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다. Long command
line은 toolchain profile capability와 response file policy로 다룬다.

## 최소 예제

```toml
[profile]
name = "msvc-uefi"
response_files = true
response_style = "msvc"
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

## 실패 예제

```toml
[profile]
response_files = false
```

Command가 platform limit을 넘는데 response file을 사용할 수 없으면 stable diagnostic으로
막아야 한다.

## 관련 CLI

```sh
qstar --file qstar.lua dry-run //:boot
qstar --file qstar.lua action-log //:boot:link:0
qstar --file qstar.lua replay //:boot:link:0
```

## 관련 diagnostic

- `response_style=msvc`
- `argv_digest=...`
- `response file is not supported by this toolchain profile`
