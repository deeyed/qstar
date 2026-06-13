# Cookbook: Staging

QStar는 C/C++/Cale을 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다. Install은
prefix에 개발 artifact를 배치하고, stage는 ESP/RPi 같은 package tree를 만든다.

## 최소 예제

```lua
qstar.stage "esp" {
  root = "stage/esp",
  description = qstar.status("Staging EFI package"),
  files = {
    qstar.stage_file(qstar.target_file("//:boot"), "EFI/BOOT/BOOTX64.EFI"),
  },
}
```

## 전체 예제

```lua
qstar.stage "rpi" {
  root = "stage/rpi",
  description = qstar.status("Staging firmware image"),
  files = {
    qstar.stage_file("boot/config.txt", "config.txt"),
    qstar.stage_file(qstar.target_file("//:kernel"), "kernel.elf"),
    qstar.stage_file(qstar.target_file("//:kernel_img"), "kernel8.img"),
    qstar.stage_file("boot/payload.bin", "payload.bin"),
  },
}
```

`stage --dry-run`은 diff를 출력하고 `build/qstar/stage/<label>/manifest.json`을 남긴다.
Manifest schema는 `qstar-stage-manifest-v2`이며 각 entry는 `kind`와 `producer`를 가진다.
예를 들어 plain file은 `kind=file`, `qstar.target_file("//:boot")`는 `kind=target`,
custom target output은 `kind=custom_target`으로 기록된다.

## 실패 예제

```lua
qstar.stage "bad" {
  root = "../stage",
  files = {},
}
```

Stage root와 destination은 package-relative여야 한다.
같은 stage 안에서 `EFI/BOOT`와 `EFI/BOOT/BOOTX64.EFI`처럼 하나가 다른 하나의 parent
directory처럼 보이는 destination도 layout conflict로 거절된다.

## 관련 CLI

```sh
qstar --file qstar.lua stage //:esp --dry-run
qstar --file qstar.lua stage //:esp
qstar --file qstar.lua last-failure
```

## 관련 diagnostic

- `stage root must be package-relative`
- `stage destination is duplicated`
- `stage destination layout conflict`
- `failure_kind=package-failure`
