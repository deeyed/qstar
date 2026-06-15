# QStar Build Model

> Current note: Round 47 이후 새 authoring syntax는
> `docs/qstar-v0.2-authoring-spec.md`와 `../wiki/`를 정본으로 본다.

이 문서는 QStar가 소비하는 workspace, package, target, module, source, header file 모델을 정리한다. header-language 자체의 문법과 declaration 의미는 QStar가 아니라 external compiler/header-language checker가 담당한다.

## Concept Hierarchy

| Concept | 의미 |
| --- | --- |
| `workspace` | 여러 package를 담는 최상위 작업 공간 |
| `package` | 배포, 버전, dependency, namespace 단위 |
| `target` | QStar가 만드는 산출물 단위 |
| `module` | external language semantic compilation unit |
| `source file` | module을 구성하는 구현 파일 |
| `item` | declaration/function/type/proto/impl/const |
| `Header file` | public/install or private/internal header path recorded in the build graph |
| `QStar fragment` | build graph declaration file |

`crate`는 공식 용어로 쓰지 않는다. `project`는 설명용으로만 쓰고, model term은 `workspace`와 `package`를 우선한다.

## Directory Roles

```txt
package/
  qstar.lua

  include/
    package/
      package.h
      feature.h
      legacy.h

  src/
    app/
      main.foreign
    core/
      arena.foreign
      log.foreign
```

역할:

- `src/`: implementation module root.
- `include/`: public header install root.
- `qstar.lua`: root build orchestration, project metadata, and in-DSL profile declarations.
- `<dirname>.qst`: subdir build graph fragment.

Round 66 기준으로 QStar는 mandatory external config file을 읽지 않는다.
Profile, target, toolchain, stdlib, linker, response-file, external-tool policy는
`qstar.lua`의 `qstar.profile` declaration으로 들어온다. Dependency resolution,
secure profile, UB policy, audit profile은 QStar core가 아니라 package/compiler
policy layer의 책임이다.

`mod.rs`, `module.foreign`, 빈 placeholder 파일은 공식 구조로 두지 않는다.

## Module Model

external language lane의 기본 컴파일 단위는 folder module이다.

```txt
src/render/renderer.foreign
src/render/pipeline.foreign
```

위 파일들은 `render` module을 구성한다. Module 내부에서는 private item이 파일 사이에 보일 수 있고, module 밖에서는 `export` item만 보인다.

파일은 선택적으로 module assertion을 가질 수 있다.

```c
module render;
```

이 선언은 module 이름을 정하지 않는다. Package resolver와 QStar module root가 계산한 module path와 일치하는지 검증한다.

## Import Model

`import`는 filesystem 상대 경로가 아니라 logical module path다.

```c
import render;
import render::vulkan;
import core::mem;
```

규칙:

- current package 내부 import는 package prefix 생략을 허용한다.
- external package import는 dependency alias가 top-level namespace가 된다.
- 이름 충돌이 있으면 명시 package prefix를 요구한다.
- `../foo` 같은 filesystem relative import는 언어 문법에 넣지 않는다.

Dependency alias는 QStar core의 필수 설정 파일에서 오지 않는다. 외부 package
resolver나 CLI-provided package alias map이 resolved package root를 전달하면,
QStar는 그 결과를 label resolution input으로 소비한다.

```c
import core::mem;
import image::png;
```

## Compiler-Owned header-language Header Model

이 섹션은 QStar가 아니라 external compiler/header-language checker가 구현해야 할 header 언어
모델을 기록한 것이다. QStar는 아래 문법을 읽거나 해석하지 않고, `.h`을
일반 header file path로만 보존한다.

header-language은 provider-owned header system이다. header-language은 module marker가 아니라 include surface다.

```txt
include/
  engine/
    engine.h
    render.h
```

header-language은 `#include`로 소비한다.

```c
#include <engine/engine.h>
```

`.h`와 `.h`의 차이:

| Extension | 의미 |
| --- | --- |
| `.h` | legacy textual C header, export filter 없음 |
| `.h` | provider-owned smart include header, `export` declaration만 includer에게 노출 |

`.h`은 `#include` spelling을 쓰지만 raw textual paste가 아니다. 처리 모델은 다음과 같다.

```txt
smart preprocess
  -> parse header-language/provider-owned declarations
  -> export filter
  -> inject exported declarations into includer scope
```

header-language 내부의 private declaration은 같은 header-language facade를 구성하는 데 쓸 수 있지만, includer scope에는 주입되지 않는다. header-language은 module marker도 `import`도 아니며, package public facade/API surface다.

### header-language v1 Grammar Surface

header-language v1은 C/external declaration syntax를 재사용하되, public facade를 고정하기 위한 작은 export layer만 추가한다.

```txt
header_file                  = header_item*
header_item                  = pp_directive
                          | private_declaration
                          | export_declaration
                          | export_using_declaration
                          | export_opaque_declaration
                          | static_assert_declaration
export_declaration        = "export" declaration
export_using_declaration  = "export" "using" imported_name ";"
export_opaque_declaration = "export" "opaque" ("struct" | "union") identifier ";"
```

`private_declaration`은 header-language 내부 composition에는 사용할 수 있지만 includer에게 노출되지 않는다. `export declaration`은 C ABI-compatible public surface를 노출한다. `export using`은 이미 include/import된 declaration을 facade public API로 다시 내보낼 때 쓴다. `export opaque struct T;`는 public API가 `T *` 같은 opaque pointer를 쓰되 concrete layout을 숨겨야 할 때 사용한다.

### C Declaration Import/Export

`.h`이 다른 `.h`을 include하면 기본적으로 그 header의 exported declaration을 현재 facade composition scope에서 볼 수 있다. Umbrella header에서 다시 공개하려면 `export using` 또는 명시적인 `export` redeclaration을 사용한다.

`.h` include는 legacy C declaration import다. C declaration은 C parser와 C semantic lane으로 받아들이지만, header-language의 public re-export가 자동으로 되지는 않는다. C header에서 가져온 API를 header-language facade 밖으로 내보내려면 `export using`이나 C ABI-compatible `export` redeclaration으로 의도를 적어야 한다.

header-language v1 public export는 C ABI-compatible shape를 우선한다. `T &` 같은 external source convenience는 generated C header에서 `T *`로 낮출 수 있지만, ownership-only type, unconcretized generic, non-C ABI `class`, target ABI가 정해지지 않은 value type은 public export에서 stable diagnostic으로 막는다.

예:

```c
// include/engine/render.h
struct InternalRenderLayout {
    int hidden;
};

export struct Renderer {
    int id;
};

export void renderer_draw(Renderer &renderer);
```

Includer는 `Renderer`와 `renderer_draw`만 볼 수 있다.

### QStar Header File Graph Policy

Round 4/5 구현은 header-language parser가 아니라 graph-level file policy checker다. 이
정책은 `.h`에만 적용되는 특수 규칙이 아니라 QStar가 보는 모든 header file
path에 적용된다.

QStar는 다음을 검증한다.

- `public_headers` 항목은 package-relative path여야 한다.
- `public_headers` 항목은 `include/` 아래 있어야 한다.
- `private_headers` 항목은 package-relative path여야 한다.

`qstar explain`은 header마다 build-system file metadata만 출력한다.

```txt
header_file public path=include/pkg/api.h role=install semantic=opaque-to-qstar
header_file public path=include/pkg/api.hpp role=install semantic=opaque-to-qstar
header_file private path=src/pkg/internal.h role=internal semantic=opaque-to-qstar
```

이 출력은 QStar가 header file을 install/internal build input으로만 본다는 marker다.
QStar는 `.h`을 특수 해석하지 않는다. `export declaration`, `export using`,
`export opaque`의 실제 declaration 처리는 external compiler/header-language checker가 맡는다.

## QStar Fragment Model

QStar fragment는 target declaration을 graph에 추가하는 build file이다. Module 존재를 표시하지 않는다.

작은 package:

```txt
hello/
  qstar.lua
  src/
    app/
      main.foreign
```

큰 package:

```txt
engine/
  qstar.lua
  src/
    render/
      render.qst
      renderer.foreign
    asset/
      asset.qst
      loader.foreign
```

Root `qstar.lua`:

```lua
qstar.subdir("src/render")
qstar.subdir("src/asset")
```

Round 8부터 `qstar/tests/manual/hello`는 직접 복사해 실행해 볼 수 있는 reference
fragment 구조로 둔다. Root `qstar.lua`는 package-level target과 generated action을
선언하고, `src/foo/foo.qst`는 subdir target을 선언한다. QStar는 이 구조를 build
executor로 실행하지 않고, Graph IR/explain/dry-run surface로만 해석한다.

Round 9부터 `qstar check`는 root `qstar.lua`가 있는 directory를 package root로 보고,
source/header/generated-input path가 그 root 아래 실제 파일로 존재하는지 확인한다.
이 검사는 build-system authoring UX를 위한 것이며, C/external/header-language 파일 내용을 parse하지
않는다.

Round 10부터 target과 generated action은 declaration origin을 보존한다. Origin은
Lua source file과 line 중심이며, `qstar query`와 diagnostic 출력에 쓰인다. Field별
정밀 span은 아직 v0 범위가 아니지만, `sources`, `deps`, `public_headers` 같은
검증 단계는 field 이름을 diagnostic metadata로 남긴다.

Round 11부터 명시 source model은 literal source path, generated output path,
`qstar.files` glob 결과, Lua helper 결과를 같은 `sources` list로 정규화한다.
이 단계 이후 QStar는 source path를 하나의 canonical list로 보고 duplicate source와
unsupported extension을 검증한다.

## Target Model

Target은 QStar가 만드는 산출물 단위다. Target은 file sources, language-local
headers/include dirs, dependencies, toolchain, stdlib policy를 가진다.

```lua
qstar.target "engine" {
    kind = "staticlib",
    lang = {
        external-tool = {
            modules = qstar.modules {
                root = "src",
                include = {
                    "render",
                    "asset",
                },
            },
            public_headers = {
                "include/engine/engine.h",
            },
            public_include_dirs = {
                "include",
            },
        },
    },
    deps = {
        "//src/render:render",
        "//src/asset:asset",
    },
    toolchain = "host",
    stdlib = "system",
}
```

QStar target은 secure profile, UB category override, package version resolution을 들고 있지 않는다.

## Source Discovery Skeleton

Round 6 구현은 explicit `sources` list만 discovery 대상으로 본다. 지원 suffix는
`.c`, `.foreign`, `.foreign`, `.s`, `.S`이며 각각 `c`, `external-tool`, `external-tool`, `asm`, `asm-cpp` language로
분류된다.

```txt
source_discovery explicit=2 modules=present status=explicit-only
source_file path=src/main.c language=c tool=c-compiler provider=c output_group=objects role=compile
source_file path=src/app.foreign language=external-tool tool=external-tool-compiler provider=external-tool output_group=objects role=compile
```

`modules`는 future source discovery의 입력으로 보존하지만, Round 6에서는 directory
scan, glob expansion, generated source dependency tracking을 하지 않는다.

## Generated Output Edge Skeleton

Round 7은 generated source edge를 build graph에 기록한다.

```lua
qstar.custom_target "version" {
    inputs = {
        "tools/version.txt",
    },
    outputs = {
        qstar.output("generated/version.c"),
    },
    command = qstar.cli {
        "version-gen",
        "--out", qstar.output(0),
    },
}

qstar.executable "app" {
    sources = {
        qstar.output("generated/version.c"),
        "src/main.c",
    },
}
```

Generated output은 package-relative path여야 하며 effective
`qstar.project.generated_dir` 아래로 제한한다. 기본값은 `generated`다.
`qstar.output(path)`은 path spelling helper일 뿐이고 파일을 만들지 않는다.
Round 48부터 `qstar.custom_target`의 command는 `qstar.cli { ... }` argv-vector이며,
`qstar.output(0)`은 같은 rule의 첫 번째 output을 가리키는 command placeholder다.
`qstar explain`은 generated action과 consumer edge를 출력하지만 generator를 실행하지
않는다.
