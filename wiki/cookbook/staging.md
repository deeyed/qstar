# Cookbook: Staging

QStar는 C/C++/ASM을 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다. Install은
prefix에 개발 artifact를 배치하고, stage는 release bundle이나 test fixture 같은 copy-only
package tree를 만든다.

## 최소 예제

```lua
qstar.stage "bundle" {
  root = "stage/bundle",
  description = qstar.status("Staging package"),
  files = {
    qstar.stage_file(qstar.target_file("//:app"), "bin/app"),
  },
}
```

## 전체 예제

```lua
qstar.stage "bundle" {
  root = "stage/bundle",
  description = qstar.status("Staging release bundle"),
  files = {
    qstar.stage_file("assets/config.txt", "config.txt"),
    qstar.stage_file(qstar.target_file("//:app"), "bin/app"),
    qstar.stage_file(qstar.target_file("//:package_blob"), "share/app.bin"),
    qstar.stage_file("assets/readme.txt", "README.txt"),
  },
}
```

`stage --dry-run`은 diff를 출력하고 `build/qstar/stage/<label>/manifest.json`을 남긴다.
Manifest schema는 `qstar-stage-manifest-v2`이며 각 entry는 `kind`와 `producer`를 가진다.
예를 들어 plain file은 `kind=file`, `qstar.target_file("//:app")`는 `kind=target`,
custom target output은 `kind=custom_target`으로 기록된다.

## 실패 예제

```lua
qstar.stage "bad" {
  root = "../stage",
  files = {},
}
```

Stage root와 destination은 package-relative여야 한다.
같은 stage 안에서 `bin`과 `bin/app`처럼 하나가 다른 하나의 parent directory처럼 보이는
destination도 layout conflict로 거절된다.

Stage layout을 다른 action이나 project command가 소비해야 하면 `qstar.stage_dir("//:bundle")`를
`run_target.inputs`나 `qstar.step.run.inputs`에 선언하고, command argv에서는
`qstar.input(N)`으로 참조한다. 사람이 직접 실행하는 layout export는 built-in `install`을
덮어쓰지 말고 root `qstar.command` 안에서 `qstar.step.export_stage("//:bundle", { to = ... })`로
표현한다.

## 관련 CLI

```sh
qstar --file qstar.lua stage //:bundle --dry-run
qstar --file qstar.lua stage //:bundle
qstar --file qstar.lua workflow-export --out exports/bundle
qstar --file qstar.lua last-failure
```

## 관련 diagnostic

- `stage root must be package-relative`
- `stage destination is duplicated`
- `stage destination layout conflict`
- `failure_kind=package-failure`
- `project command name 'install' is reserved`
