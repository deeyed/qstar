# Workspace, Project, Package

QStar는 C/C++/Cale을 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다.
Workspace, project, package는 source tree를 graph로 해석하기 위한 경계다.

## 최소 예제

```lua
qstar.project {
  name = "demo",
  version = "0.1.0",
  root = ".",
}
```

Root discovery는 현재 file에서 위로 올라가며 가장 가까운 `qstar.lua`를 찾는다. 별도
workspace marker file은 필요 없다.

## 전체 예제

```txt
demo/
├── qstar.lua
├── app/
│   └── app.qst
└── lib/
    └── lib.qst
```

```lua
qstar.project {
  name = "demo",
  version = "0.1.0",
  root = ".",
}

qstar.subdir("lib")
qstar.subdir("app")
```

`qstar.project.root`와 `QSTAR_PROJECT_ROOT`는 authoring helper에서 현재 project root를
참조할 때 사용한다.

## 실패 예제

```lua
qstar.project {
  name = "bad",
  root = "src",
}
```

v1에서 `root`는 `"."`만 허용된다.

## 관련 CLI

```sh
qstar --file qstar.lua query //app:app
qstar --file qstar.lua list-targets --format json
qstar --file app/app.qst build //app:app
```

## 관련 diagnostic

- `qstar.project root must be "." in v1`
- `target label is owned by another package`
- `package root not found`
