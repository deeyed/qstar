# Imports and Modules

QStar는 Lua `require`를 열지 않는다. 대신 DSL-native import를 두 개로 분리한다.

```lua
qstar.import_file("qstar/policies/freestanding.qst")
local policy = qstar.import_module("qstar/modules/policy")
```

## `qstar.import_file`

`qstar.import_file("path.qst")`는 package-root 기준 `.qst` file만 읽는다.

- `.qst` file은 target, config, profile, group, stage 같은 graph declaration을 만들 수 있다.
- 같은 file을 한 번 더 import하면 duplicate import error가 난다.
- circular import는 error다.
- `.qsm`이나 `qstar.lua`는 받을 수 없다.

## `qstar.import_module`

`qstar.import_module("folder/path")`는 folder path만 받는다. File path를 직접 넘기지 않는다.

```lua
local kernel = qstar.import_module("qstar/modules/kernel")
```

위 호출은 다음 file을 읽는다.

```txt
qstar/modules/kernel/kernel.qsm
```

`.qsm` file은 반드시 table을 반환한다.

```lua
local M = {}

function M.common_c()
  return {
    public_include_dirs = {"include"},
    compile_options = {"-Wall", "-Wextra"},
  }
end

return M
```

`.qsm` 안에서는 graph declaration을 할 수 없다. 즉 `qstar.project`, `qstar.profile`,
`qstar.config`, target rule, `qstar.custom_target`, `qstar.stage`, `qstar.target_family`, `qstar.subdir`,
`qstar.import_file`은 금지된다. Helper 함수, 상수, table literal, `qstar.import_module`
을 통한 다른 helper module import는 사용할 수 있다.

## 권장 배치

```txt
qstar.lua
qstar/
  policies/
    freestanding.qst
    warnings.qst
  modules/
    kernel/
      kernel.qsm
    paths/
      paths.qsm
lib/
  core/
    core.qst
```

Root `qstar.lua`는 project/profile/subdir orchestration을 담당한다.
`qstar/policies/*.qst`는 graph policy를 선언한다.
`qstar/modules/<name>/<name>.qsm`은 target을 만들지 않는 helper module을 제공한다.
