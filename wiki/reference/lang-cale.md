# Cale Language Options

QStar는 C/C++/Cale을 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다. QStar는
Cale/HCL 문법을 해석하지 않고 process invocation과 header path만 다룬다.

## 최소 예제

```lua
lang = {
  cale = {
    profile = "safe",
  },
}
```

## 전체 예제

```lua
qstar.staticlib "cale_core" {
  sources = {
    "src/core.cl",
  },
  lang = {
    cale = {
      profile = "safe",
      compile_options = {"--profile=safe"},
      public_headers = {"include/core.hcl"},
      public_include_dirs = {"include"},
      modules = { enabled = false },
    },
  },
}
```

HCL도 header surface이므로 `public_headers`와 `public_include_dirs`를 쓴다. 별도
`hcl_include_dirs` option은 정본 surface가 아니다.

Cale source는 Stella-only language-provider action이다. `-G stella`는 configured `cale`
compiler를 process로 호출하지만, `-G ninja`는 Cale compile wrapper rule을 만들지 않고 stable
diagnostic으로 실패한다. 이 경계는 QStar가 Cale compiler 내부 API나 HCL 의미론을 소유하지
않기 위한 계약이다.

## 실패 예제

```lua
qstar.staticlib "bad" {
  sources = {"src/core.cl"},
  lang = {
    cale = {
      hcl_include_dirs = {"include"},
    },
  },
}
```

## 관련 CLI

```sh
qstar --file qstar.lua dry-run //:cale_core
qstar --file qstar.lua build //:cale_core
qstar --file qstar.lua -G stella build //:cale_core
qstar --file qstar.lua -G ninja build //:cale_core
qstar --file qstar.lua doctor
```

## 관련 diagnostic

- `unknown lang.cale field 'hcl_include_dirs'`
- `Cale source 'src/core.cl' is a Stella-only language-provider action`
- `Cale compiler 'cale' not found`
- `unsupported Cale source mode`
