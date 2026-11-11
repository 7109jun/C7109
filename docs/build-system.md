# Cmake7109.txt 빌드 시스템

여러 소스·의존성·라이브러리를 가진 프로젝트를 빌드하는 방법. 선언형 빌드 파일 `Cmake7109.txt`와 `.cset7`(옵션), `.cr7`(잠금), `.l7`(패키지), `.mp7`(맵)을 다룬다. 문법의 정밀한 정의는 [Cmake7109-grammar.md](Cmake7109-grammar.md), 결정 이력은 [stage5-decisions.md](stage5-decisions.md). 코드 블록은 `tools/doccheck.py`가 실제 파서로 검증한다.

## 1. 최소 프로젝트

```
myapp/
├─ Cmake7109.txt
└─ src/main.c7
```

```buildfile
project "MyApp":
    version "1.0.0"

target executable "app":
    sources:
        "src/main.c7"
```

```sh
c7c build myapp          # → myapp/build/app
./myapp/build/app
```

`c7c`는 인수에 `.c7`이 있으면 단일 파일 모드, 없으면 프로젝트 모드(경로 기본값 `.`)다.

## 2. 문법 요약

- **들여쓰기 기반**, 공백만(탭은 `E0702`). `#`은 주석. 문자열은 `"..."`.
- 변수·조건·반복·사용자 명령은 **없다**(순수 선언형).
- 최상위 키워드: `project registry dependency config default_config target`.

```buildfile
project "Example":
    version "1.2.3"
    license "GPL-3.0-only"
    description "예제 프로젝트"

registry "../registry"
dependency "Other" "^1.2"
dependency "Local" path "../local"

config debug:
    mode "debug"
    optimization "O0"
    bounds_check true
    debug_alloc true

config release:
    mode "release"
    optimization "O2"

default_config release

target library "mylib":
    sources:
        "src/lib.c7"

target executable "app":
    sources:
        "src/main.c7"
    include_dirs:
        "include"
    defines:
        "VERSION=2"
    depends:
        "mylib"
        "Other"
    output "build/app"
    link "static"
```

### 타깃 속성

| 키 | 값 | 설명 |
|---|---|---|
| `sources:` | 파일 목록(필수) | 프로젝트 안의 `.c7` 파일 |
| `include_dirs:` | 디렉터리 목록 | `-I` |
| `defines:` | `NAME` / `NAME=VALUE` | `-D` |
| `depends:` | 이름 목록 | 같은 프로젝트의 타깃 또는 `dependency` 이름 |
| `output` | 경로 | 기본 `<빌드 디렉터리>/<이름>`(라이브러리는 `<이름>-<버전>.l7`) |
| `platform` | `"<arch>-<os>"` | 기본 호스트. 지원은 `x86_64-linux`뿐(그 밖은 `E0715`) |
| `link` | `static` / `dynamic` | |
| `std` | `none` / `default` | `none`이면 암묵 표준 라이브러리를 끈다(`--no-std`) |
| `mode`, `optimization`, `bounds_check`, `debug_alloc`, `overflow_check` | `config`와 같음 | 이 타깃에서만 `config`를 덮어씀 |

경로 규칙: `sources`, `include_dirs`, `output`은 **프로젝트 디렉터리 안**만 허용한다(절대 경로, `..`로 밖으로 나가는 경로는 `E0719`). `dependency … path`와 `registry`만 밖을 가리킬 수 있다.

## 3. 옵션 우선순위

낮음 → 높음:

> 내장 기본값 < `.cset7` < 선택된 `config` < 타깃 속성 < 명령행

스칼라는 높은 층이 이긴다. 경로 목록(`include`, `library`, `registry`)은 낮은 층부터 이어 붙이고 중복을 제거한다. `mode "debug"`는 `--debug`와 같아서 정수 오버플로 검사를 켠다. `overflow_check`를 명시하면 그 값이 이긴다.

### .cset7

프로젝트 디렉터리의 `.cset7`(또는 `--cset7 파일`)은 TOML이다.

```toml
[compiler]
requires = ">=1.0.0-alpha.1"

[target]
os = "linux"
architecture = "x86_64"
abi = "c7abi1"

[build]
mode = "debug"
optimization = "O0"
bounds_check = false
overflow_check = true
debug_alloc = false

[diagnostics]
language = "en"      # "en" | "ko"

[link]
mode = "static"      # "static" | "dynamic"

[paths]
include = []
library = []
registry = []
```

알 수 없는 키는 경고 `W0701`, 잘못된 값은 `E0719`.

## 4. 명령

| 명령 | 설명 |
|---|---|
| `c7c build [경로] [옵션]` | 선택한(기본: 전체) 타깃을 빌드 |
| `c7c pack [경로]` | 라이브러리 타깃만 빌드(`.l7`) |
| `c7c verify [경로 \| 파일.l7…]` | 생성된(또는 주어진) 패키지를 검증 |
| `c7c lock [경로]` | `Cmake7109.cr7`을 (재)작성 — **파일을 쓰는 유일한 명령** |
| `c7c install [경로 \| 파일.l7…] [--prefix DIR]` | 패키지를 `<prefix>/<이름>-<버전>/`에 설치(기본 `$HOME/.c7/packages`) |
| `c7c clean [경로]` | 빌드 산출물 제거 |

`build` 옵션: `--target 이름`(반복 가능), `--config 이름`, `-jN`, `-O<n>`, `--mode=debug\|release`, `--bounds-check`, `--cset7 파일`, `--registry=DIR`, `--cache-dir DIR`, `--no-cache`, `--out-dir DIR`, `--verbose`.
종료 코드: 0 성공, 1 빌드 오류, 2 사용법 오류.

### 병렬 실행과 로그

`-jN`(기본: 하드웨어 스레드 수)으로 독립 타깃을 동시에 빌드한다. 의존 타깃이 끝난 뒤에만 시작하고, 하나가 실패하면 새 작업은 시작하지 않고 실행 중인 것을 끝까지 기다린 뒤 실패를 보고한다(`E0720`). 로그는 타깃별로 모아 **결정적 순서**(그래프 순서)로 출력한다.

## 5. 캐시

`<프로젝트>/.c7cache/<키>/`. 키는 컴파일러 지문(`c7c` + 런타임 + `std.l7`)·타깃 종류/이름/플랫폼·정규화된 옵션·소스 해시·라이브러리 해시로 정한다. `#include`한 헤더는 **직접 모드**로 따로 검증한다(헤더가 바뀌면 정확히 그 항목만 무효). 손상된 항목은 무시하고 다시 빌드한다. 동시 실행에도 안전하다(임시 파일 + `rename`). `--no-cache`로 끄고 `c7c clean`으로 지운다.

## 6. 의존성

```buildfile
project "App"
registry "../registry"
dependency "Mathlib" "^1.0"
dependency "Local" path "../local"

target executable "app":
    sources:
        "src/main.c7"
    depends:
        "Mathlib"
        "Local"
```

- **레지스트리**: 디렉터리에 `<이름>-<버전>.l7`을 모아 둔다. 파일명과 매니페스트가 일치해야 한다.
- **경로 의존성**: `.l7` 파일, 또는 `Cmake7109.txt`가 있는 디렉터리(그 프로젝트의 같은 이름 라이브러리 타깃을 먼저 빌드).
- **해결**: 모든 제약을 모아 패키지마다 조건을 만족하는 **최고 버전**을 고른다. 선택한 패키지의 `[[dependency]]`로 제약을 확장해 고정점까지 반복한다. 백트래킹은 없다. 만족하는 버전이 없으면 자동 변경 없이 `E0713`(누가 어떤 제약을 걸었는지 나열), 순환은 `E0706`, 못 찾으면 `E0716`.
- **버전 요구**: `^1.2`, `~1.2.3`, `>=1.0, <2.0`, `1.*`, `=1.2.3`. 맨 버전 `1.2.3`은 `^1.2.3`. 프리릴리스는 요구 조건이 같은 `MAJOR.MINOR.PATCH`의 프리릴리스를 직접 적었을 때만 매칭한다.
- 중앙·Git·사내 레지스트리는 인터페이스만 있고 `E0716`("not implemented")을 낸다.

## 7. 잠금 파일 `Cmake7109.cr7`

재현 가능한 빌드를 위해 해결 결과를 고정한다.

```toml
[lock]
format_version = 1

[compiler]
name = "C7109"
version = "1.0.0-alpha.1"
abi = "c7abi1"

[target]
os = "linux"
architecture = "x86_64"

[[dependency]]
name = "Mathlib"
version = "1.0.0"
source = "registry:../registry"
sha256 = "0000000000000000000000000000000000000000000000000000000000000000"
```

규칙:

1. `c7c lock`만 파일을 쓴다. 내용이 같으면 건드리지 않는다.
2. `c7c build`는 **읽기만** 한다. 레지스트리에 더 높은 버전이 생겨도 잠긴 버전을 쓴다. 새 버전을 쓰려면 `c7c lock`을 다시 실행한다.
3. 요구 조건과 잠금이 어긋나거나(없어졌거나, 더 이상 안 맞거나, 출처가 다르거나, 필요 없는 항목) 하면 `E0714`.
4. 레지스트리 파일의 해시가 잠금과 다르면 `E0710`(변조 의심).

## 8. 패키지 `.l7`

ZIP 컨테이너. `manifest.toml`(첫 항목) · 정렬된 내용 · `checksums.sha256`(마지막, `sha256sum` 형식). 같은 입력이면 **바이트까지 동일**(정렬 + 고정 타임스탬프). 검증은 ZIP 구조, 안전한 경로(`..`·절대 경로·역슬래시 거부), 체크섬(목록에 없는 파일·누락·불일치 → `E0710`), 매니페스트를 모두 확인한다.

```toml
format_version = 1

[package]
name = "Mathlib"
version = "1.0.0"
abi = "c7abi1"
license = "GPL-3.0-only"

[[dependency]]
name = "Other"
version = "^1.2"
```

- 내용: `src/**/*.c7`, 예약된 `lib/<os>-<arch>/…`, 자유 형식 `include/ licenses/ dependencies/ docs/`.
- 소스 패키지는 ABI만 일치하면 쓸 수 있다. 바이너리만 있는 패키지는 이 OS·아키텍처·ABI의 `[[binary]]`가 없으면 `E0712`(다른 플랫폼 바이너리로 대체하지 않는다).
- 알 수 없는 매니페스트 키는 `W0701`.
- 서명 체계는 아직 없다(SHA-256은 무결성 확인이지 발행자 인증이 아니다).

개발용 도구: `c7c tool l7-pack out.l7 NAME VERSION [--dep NAME REQ] [--bin os arch abi path] [--license L] 경로=파일…`, `l7-verify`, `l7-extract`.

## 9. 맵 파일 `.mp7`

실행 파일 타깃은 출력 옆에 `<출력>.mp7`을 만든다(빌드가 성공했거나 캐시에 적중한 뒤에만, 임시 파일 + `rename`). 실패한 빌드는 맵을 남기지 않고, 이전 맵도 빌드 시작 때 지운다. 같은 입력이면 바이트까지 같다.

```text
C7109 MAP FILE
Format: 1
Project: Demo
Target: app (executable)
Platform: x86_64-linux (host: x86_64-linux)
Configuration: (none) (mode=release, optimization=O0)
Compiler: c7c 1.0.0-alpha.1
Units: addresses are hexadecimal virtual addresses (0x...); sizes are in bytes

[Inputs]
src/main.c7  sha256=…  bytes=119

[Libraries]

[Outputs]
build/app  bytes=33064  sha256=…

[Sections]
.text addr=0x… size=…

[Symbols]
0x0000000000001240 17 t square(i64)
0x0000000000001260 23 T main
```

심볼은 `_C7$…`(복호화: `a::b(T1, T2)`)·`main`·`c7_*`만 나열한다. 최적화·링커가 제거한 심볼은 없다. `nm`/`readelf`가 없으면 해당 섹션에 `; unavailable: …`가 들어간다.

## 9.1 표준 라이브러리 `std.l7`

`stdlib/Cmake7109.txt`가 표준 라이브러리 자신의 빌드 파일이다(자기 빌드 시스템으로 빌드). `make`가 `c7c pack stdlib`을 돌려 `./std.l7`을 만든다. `c7c`는 `C7_STDLIB`(소스 디렉터리) > `std.l7` > `stdlib/` 순으로 표준 라이브러리를 찾고, `std.l7`이 변조됐으면 **폴백 없이** 거부한다.

## 10. 진단 코드(빌드)

| 코드 | 의미 |
|---|---|
| E0701 | 올바르지 않은 TOML |
| E0702 | 빌드 파일 문법 오류 |
| E0703 | 알 수 없는 키워드 |
| E0704 | 소스 파일 없음 |
| E0705 | 타깃 이름 중복 |
| E0706 | 의존 관계 순환 |
| E0707 | 출력 경로 충돌 |
| E0708 | 충돌하는 옵션(중복 키 등) |
| E0709 | 올바르지 않은 패키지/매니페스트 |
| E0710 | 체크섬 불일치 |
| E0711 | 패키지 안의 안전하지 않은 경로 |
| E0712 | 이 플랫폼에 쓸 수 있는 내용이 없는 패키지 |
| E0713 | 모든 요구를 만족하는 버전 없음 |
| E0714 | 잠금 파일이 최신이 아님 |
| E0715 | 지원되지 않는 대상 플랫폼 |
| E0716 | 의존성을 찾을 수 없음 / 출처 미구현 |
| E0717 | 올바르지 않은 버전·요구 조건 |
| E0718 | ZIP 오류 |
| E0719 | 잘못된 값 |
| E0720 | 타깃 빌드 실패 |
| W0701 | 알 수 없는 키(무시) |
