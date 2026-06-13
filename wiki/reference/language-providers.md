# Language Providers

QStar는 C/C++/Cale을 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다. Language
provider는 source file을 object나 generated artifact로 바꾸는 외부 compiler/process 경계이며,
QStar는 provider 언어의 AST, module system, header semantics를 해석하지 않는다.

## 최소 예제

```lua
qstar.staticlib "core" {
  toolchain = "cale",
  sources = {"src/core.cale"},
}
```

Cale source는 Stella-only language-provider action이다. QStar는 Stella executor에서 configured
`cale` compiler를 argv-vector process로 호출한다. Ninja backend는 이번 release에서 Cale compile
wrapper rule을 만들지 않는다.

## 전체 예제

```lua
qstar.profile "default" {
  toolchain = "cale",
  cale = "cale",
}

qstar.staticlib "core" {
  sources = {
    "src/core.cale",
  },
  lang = {
    cale = {
      profile = "safe",
      compile_options = {"--profile=safe"},
      public_headers = {"include/core.hcl"},
      public_include_dirs = {"include"},
    },
  },
}
```

계약:

- `.cale`/`.cl` source는 `lang.cale` option과 `toolchain = "cale"` 또는 `cale-sol` profile로
  표현한다.
- Stella는 Cale source를 compile action으로 계획하고 `cale -c ... -o ...` 형태의 process를
  실행한다.
- Ninja는 C/C++/ASM provider action과 generated/custom action을 lower하지만, Cale source는
  stable diagnostic으로 거부한다.
- HCL은 QStar가 해석하지 않는다. `.hcl`은 `lang.cale.public_headers`,
  `lang.cale.private_headers`, include dir, install/export surface에 들어가는 header-like path다.
- Cale frontend/backend 내부 API 호출, HCL import/export semantic checking, Cale-specific
  package resolution은 QStar 책임이 아니다.

## 실패 예제

```lua
qstar.staticlib "core" {
  toolchain = "cale",
  sources = {"src/core.cale"},
}
```

```sh
qstar --file qstar.lua -G ninja build //:core
```

이 명령은 실패한다. Cale source를 Ninja에서 실행하려면 별도의 wrapper lowering 정책, depfile
계약, response-file 계약, action-log/replay 계약이 필요하므로 이번 release에서는 제공하지 않는다.

## 관련 CLI

```sh
qstar --file qstar.lua -G stella build //:core
qstar --file qstar.lua -G ninja build //:core
qstar --file qstar.lua dry-run //:core
qstar --file qstar.lua explain //:core
```

## 관련 diagnostic

- `qstar: Cale source 'src/core.cale' is a Stella-only language-provider action in this release; Ninja wrapper lowering is deferred; use -G stella`
- `qstar: Cale source 'src/core.cale' requires toolchain=cale`
- `qstar: Cale compiler 'cale' not found`
- `hcl_include_dirs is removed; use lang.cale.public_include_dirs or lang.cale.private_include_dirs`
