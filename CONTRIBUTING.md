# C7109 기여 가이드 — 5단계(빌드 시스템·패키지) 남은 작업

> 이 문서 하나로 **현재 상태를 파악하고, 할 일을 골라, 검증 방법까지 알고** 기여할 수 있게 한다.
> 전체 프로젝트 현황은 `STATUS.md`, 규격서 결정은 `docs/spec-D8-draft.md`를 본다.
> **이 문서는 계획서다.** 아래 "작업 목록"의 항목은 아직 하나도 구현하지 않았다(§3의 "끝난 것"만 구현·검증됨).

목차: [1. 빠른 시작](#1-빠른-시작) · [2. 기여 규칙](#2-기여-규칙-필수) · [3. 현재 상태](#3-현재-상태) · [4. 확정된 결정과 설계 초안](#4-확정된-결정과-설계-초안) · [5. 작업 목록](#5-작업-목록) · [6. 소유자 결정이 필요한 질문](#6-소유자-결정이-필요한-질문) · [7. 진단 코드](#7-진단-코드-e07xx) · [8. 테스트·검증 전략](#8-테스트검증-전략) · [9. 리뷰 체크리스트](#9-리뷰-체크리스트) · [10. 파일 안내](#10-파일-안내)

---

## 1. 빠른 시작

**필요한 것** (Ubuntu 24.04 기준): `g++`(C++20), `make`, `llvm`(llc 18), `gcc`, `binutils`(nm·size·readelf), `python3`(≥3.11, `tomllib` 때문), `unzip`, `sha256sum`.

```bash
apt-get install -y llvm gcc g++ make python3 unzip binutils
cd c7109
make clean && make        # 경고 0건이어야 한다
make test                 # passed=110 failed=0 이어야 한다 (5단계 검증 스크립트는 아직 포함되지 않음 — W1)
make asan                 # asan sweep bad=0 이어야 한다

# 5단계에서 이미 만든 모듈의 검증 (각각 마지막 줄이 problems=0 이어야 한다)
python3 tests/check_toml.py
python3 tests/check_zip.py
python3 tests/check_semver.py
python3 tests/check_l7.py
```

`c7c`는 **프로젝트 루트에서** 실행한다(실행 파일 옆의 `runtime/c7rt.c`, `stdlib/`를 찾는다).
현재 지원하는 사용법은 `./c7c build prog.c7 -o prog`, `./c7c build a.c7 b.c7 --lib x.l7 -o prog --depfile deps.txt`, `./c7c test file.c7`,
그리고 개발용 `./c7c tool <하위명령>` 이다(§3.1).

---

## 2. 기여 규칙 (필수)

이 프로젝트가 지켜 온 원칙이다. 어기면 리뷰에서 돌려보낸다.

1. **조용히 틀리지 않는다.** 지원하지 않는 것은 진단 코드(`E0901` 등)로 **명확히 거부**한다. 추측해서 동작하는 척하지 않는다.
2. **미결정은 임의로 확정하지 않는다.** 규격서가 `미결정`으로 둔 것은 §6에 질문으로 올리고, 확정되기 전에는 "초안(⚠)"으로 표시한다. 소유자가 확정한 항목(§4.1)은 바꾸지 않는다.
3. **진단 코드는 `compiler/frontend/diag_codes.def`에만 정의한다.** 중복은 컴파일 오류, 미등록 코드 사용은 `tests/check_codes.sh`가 실패시킨다. 한번 정한 코드의 의미는 바꾸지 않는다. 새 진단은 `.def`에 먼저 추가한다(영어·한국어 메시지).
4. **모든 기능에 양성 테스트와 음성 테스트를 같이 낸다.** 가능하면 **독립된 정답 기준(오라클)** 과 대조한다(§8). "내 코드와 내 기대값이 같다"는 증거가 아니다.
5. **기대값은 사람이 손으로 검산한 값**으로 고정한다. 출력이 바뀌면 의도된 변경인지 먼저 확인하고 갱신한다.
6. **테스트가 정말 결함을 잡는지 확인한다(변이 테스트).** 구현을 일부러 망가뜨려 테스트가 실패하는지 본다. 실패하지 않으면 테스트가 약한 것이다.
7. **외부에서 온 데이터는 적대적이라고 가정한다**(`.l7`, ZIP, TOML, 빌드 파일). 경로 탈출, 크기 폭탄, 깊은 중첩, 순환을 다룬다.
8. **한 번에 하나씩 바꾸고, 바꿀 때마다 `make test`를 돌린다.** 마지막에는 `make clean && make`(경고 0건), `make test`, `make asan`을 모두 통과해야 한다.
9. **외부 라이브러리를 추가하지 않는다.** SHA-256·DEFLATE·TOML·ZIP을 직접 구현한 이유다(자체 링커·셀프 호스팅 방향, `zlib.h`가 있어도 링크 의존을 늘리지 않음).
10. **코드 주석은 영어, 문서는 한국어**로 쓴다. 헤더 주석에 "왜"를 적는다.
11. **문서를 같이 갱신한다**: `STATUS.md`(상태), 이 문서(§3, §5의 상태 열), 필요하면 `docs/spec-D8-draft.md`.
12. **함정**: 라벨·변수 이름으로 예약어(`out`, `heap`, `type`, `fn` …)를 쓰지 않는다. `cut -c`로 한글 출력을 자르면 바이트가 깨진다.

---

## 3. 현재 상태

### 3.1 끝난 것 (구현 + 자동 검증 스크립트 통과)

| 모듈 | 파일 | 하는 일 | 검증 (모두 통과) |
|---|---|---|---|
| SHA-256 | `compiler/pkg/sha256.*` | FIPS 180-4 | 19개 길이(패딩 경계 0·55·56·63·64·65바이트 등)를 Python `hashlib`과 대조 (스크립트 없이 일회성 확인 — W1에서 자동화) |
| TOML 1.0 | `compiler/pkg/toml.*` | 파서(줄:칸 진단 `E0701`) + 결정적 직렬화기 + JSON 덤프 | `tests/check_toml.py`: 유효 24개(파싱 + 직렬화 왕복을 `tomllib`과 **타입까지** 대조), 무효 40개(둘 다 거부), 명세상 엄격 2개(int64 범위 밖: `tomllib`은 받지만 명세는 거부) |
| ZIP / DEFLATE | `compiler/pkg/zip.*` | CRC-32, inflate(정지·고정·동적 Huffman), 결정적 deflate(LZ77 + 고정 Huffman), 엄격한 ZIP 읽기/쓰기 | `tests/check_zip.py`: zlib 모든 레벨×전략 400개, deflate를 zlib이 해독, 쓰기를 `zipfile`/`unzip -t`가 승인, 악성 ZIP 20여 종 거부(경로 탈출·절대경로·역슬래시·중복·대소문자 충돌·암호화·zip64·CRC 변조·크기 거짓말·257MiB 폭탄) |
| SemVer | `compiler/pkg/semver.*` | 2.0.0 버전, 요구 조건(`^ ~ >= <= > < = * 1.x`, 쉼표=AND) | `tests/check_semver.py`: 명세의 우선순위 예시 + Cargo 문서 규칙 280개 |
| `.l7` 패키지 | `compiler/pkg/package.*` | `manifest.toml` + `checksums.sha256`(sha256sum 형식) + 내용을 ZIP으로. 만들기·검증·안전 추출. 재현 가능(정렬·고정 타임스탬프) | `tests/check_l7.py`: **파이썬이 독립 구현한 정상 패키지 수용**, `sha256sum -c` 호환, 재현성, 변조 20종 거부 |
| 드라이버 확장 | `compiler/driver/main.cpp` | 여러 `.c7` 입력, `--lib x.l7`(검증 후 소스를 유닛으로), `--depfile`(읽은 파일 목록), `std.l7` 우선 로드 | **수동 스모크 테스트만 통과**(다중 파일 빌드, `--lib`, `--depfile`, 변조 `.l7` 거부). 자동 테스트 없음 — W2 |
| 진단 코드 | `compiler/frontend/diag_codes.def` | E0701–E0720, W0701–W0702 등록 | `tests/check_codes.sh` |
| 개발용 도구 | `compiler/pkg/tools.cpp` | `c7c tool sha256 / toml-json / toml-roundtrip / inflate / deflate / zip-create / zip-list / zip-cat / semver-cmp / semver-match / l7-pack / l7-verify / l7-extract` | 위 스크립트들이 사용 |

코드 규모: `compiler/pkg/` 약 1,900줄. 기존 `make test`는 110/110 통과.

### 3.2 알려진 결함·미완성 (먼저 읽을 것)

| ID | 내용 | 근거 |
|---|---|---|
| **B1** | **TOML 파서가 깊게 중첩된 입력에서 세그폴트.** 중첩 깊이 한도가 없다. 악성 `.l7`의 `manifest.toml` 하나로 컴파일러가 죽는다. | 재현: `python3 -c "print('a = ' + '['*200000)" > d.toml && ./c7c tool toml-json d.toml` → 종료코드 139. 인라인 표(`{b = {b = …`)도 동일. → W14 |
| B2 | `kCompilerVersion`(`compiler/pkg/package.cpp`)과 `--version` 문자열(`compiler/driver/main.cpp`)이 따로 하드코딩돼 있다. | 코드 확인 → W3 |
| B3 | `--depfile`에 `.l7` 파일 경로가 섞여 나온다(패키지 진단용 가짜 파일이 실제 파일로 보이기 때문). 의도한 동작이 아니다. 캐시 키는 라이브러리를 **명시적으로** 해시해야 한다. | 스모크 테스트에서 관찰 → W2/W9 |
| B4 | `std.l7`을 읽는 경로(`loadStdlib`)는 작성만 됐고 **한 번도 실행되지 않았다**(`std.l7`을 만드는 단계가 없어서 항상 `stdlib/` 폴더로 폴백). | 코드 확인 → W4 |
| B5 | `compiler/pkg/project.cpp`는 **빈 껍데기**(`isProjectCommand`가 항상 `false`). 프로젝트 모드 명령이 없다. | 코드 확인 → W10 |
| B6 | `make asan`이 `compiler/pkg`의 새 코드를 입력으로 돌려 보지 않는다. | `Makefile` → W1 |
| B7 | 검증 스크립트 4개가 `tests/run.sh`에 연결돼 있지 않아 `make test`가 잡아 주지 않는다. | → W1 |

### 3.3 아직 없는 것
`Cmake7109.txt` 파서, `.cset7` 로더, 빌드 그래프, 병렬 실행, 캐시, `.cr7` 잠금, 의존성 해결기·레지스트리, `.mp7` 생성, `std.l7` 생성, 프로젝트용 CLI. 전부 §5.

---

## 4. 확정된 결정과 설계 초안

### 4.1 소유자가 확정한 결정 (변경 금지)

| 주제 | 결정 |
|---|---|
| `Cmake7109.txt` 문법 | 명세의 개념 예시(`project "Example"` / `target executable "app":` / `sources:` 들여쓰기 목록)를 **들여쓰기 기반 문법으로 확장** |
| `.l7` 첫 구현 | **소스 포함 패키지**(`src/`)로 먼저. 타깃별 `lib/<os>-<arch>/` 바이너리 슬롯은 **형식에 예약**(`[[binary]]`) |
| 의존성 출처 | **로컬 디렉터리 레지스트리**(`<이름>-<버전>.l7` 모음) + **경로 의존성**. 중앙/Git/사내 레지스트리는 **인터페이스만** |
| 그 외 | 이전 단계에서 확정된 것(`STATUS.md` §7.1)은 모두 유지. 특히 `.l7`=ZIP+SHA-256+다중 타깃, `.cset7`/`.cr7`=TOML, SemVer, 자체 ABI |

### 4.2 이미 구현하며 정한 것 (⚠ 소유자 확인 필요 — 바꾸려면 한 곳씩 수정 가능)

- ABI 식별자 `c7abi1` (= 4단계의 ABI v1, LLVM `fastcc`). 패키지의 `abi`가 다르면 거부. 컴파일러 버전 `1.0.0-alpha.1`.
- **`.l7` 레이아웃**: `manifest.toml`(첫 항목) · 정렬된 내용 · `checksums.sha256`(마지막). 내용은 `src/**/*.c7`, 예약 `lib/<os>-<arch>/…`, 자유 형식 `include/ licenses/ dependencies/ docs/`.
  - `manifest.toml`: `format_version = 1`, `[package]`(`name` `version` `abi` 필수, `license` `description` 선택), `[compiler] requires`(선택), `[[dependency]]`(`name`, `version`=요구 조건), `[[binary]]`(`os` `architecture` `abi` `path`).
  - `checksums.sha256`: `sha256sum` 형식(`<64 hex>␣␣<경로>`), 경로순 정렬. **목록에 없는 파일, 누락, 불일치, 형식 오류, 중복은 모두 `E0710`.**
  - 알 수 없는 매니페스트 키는 경고 `W0701`(오류 아님).
  - 패키지 이름: 영문자로 시작, 영숫자·`_`·`-`, 64자 이하.
- **ZIP**: 항목 이름 UTF-8, `/` 구분, 상대 경로만. 같은 이름(대소문자 무시)·`..`·절대경로·역슬래시·드라이브 문자·제어 문자 거부. zip64·암호화·deflate/stored 이외 방식 거부. 한도: 항목당 256 MiB, 전체 1 GiB, 항목 10,000개. 고정 타임스탬프(1980-01-01)로 **재현 가능**.
- **SemVer 요구 조건**: 맨 버전은 캐럿(`1.2.3` = `^1.2.3`, Cargo 방식). `1.*`·`1.2.*`는 범위. `>1.2` = `>=1.3.0`. **프리릴리스는 요구 조건이 같은 `MAJOR.MINOR.PATCH`의 프리릴리스를 직접 적었을 때만** 매칭(npm 방식).
- **TOML**: 1.0. 정수는 64비트 부호 있음만(범위 밖은 오류). 날짜·시간은 검증만 하고 텍스트로 보관.

### 4.3 설계 초안 (⚠ 아직 구현 안 됨 — 각 작업 항목의 출발점)

**(a) `Cmake7109.txt` 문법**
```text
# 주석. 들여쓰기는 공백만 (탭은 E0702). 문자열은 "..." (\" \\ \n \t).
project "Example":            # 콜론은 선택. 속성은 들여써서: version "1.2.3" / license "..." / description "..."
    version "1.2.3"
    license "GPL-3.0-only"

registry "../registry"                       # 반복 가능
dependency "Other" "^1.2"                    # 레지스트리 의존성
dependency "Local" path "../local"           # 경로 의존성 (.l7 파일 또는 Cmake7109.txt가 있는 디렉터리)

config debug:                                # 이름 있는 빌드 구성
    mode "debug"                             # debug → --debug (오버플로 검사)
    optimization "O0"
    bounds_check true                        # → -fbounds-check
    debug_alloc true                         # → -fdebug-alloc
config release:
    mode "release"
    optimization "O2"

target library "mylib":                      # → <build>/<이름>-<버전>.l7
    sources:
        "src/lib.c7"
target executable "app":                     # → <build>/app
    sources:
        "src/main.c7"
    include_dirs:
        "include"
    defines:
        "VERSION=2"
    depends:
        "mylib"                              # 같은 프로젝트의 타깃 또는 dependency 이름
    output "build/app"                       # 기본: <빌드 디렉터리>/<이름>
    platform "x86_64-linux"                  # 기본: 호스트
    link "static"
    std "none"                               # 암묵 표준 라이브러리 끄기 (std 자신을 빌드할 때) → --no-std
```
- 최상위 키워드: `project registry dependency config target default_config`. 타깃 종류: `executable library`. 모르는 키워드는 `E0703`.
- 변수·조건·반복·사용자 명령 실행은 **미결정**이라 넣지 않는다(§6). 빌드 파일은 **순수 선언형**이다.
- **옵션 우선순위(낮음→높음)**: 내장 기본값 < `.cset7` < 선택된 `config` < 타깃 속성 < 명령행. (명세: "명령행이 파일 설정을 덮어쓴다")

**(b) `.cset7` 스키마**
```toml
[compiler]  requires = ">=1.0.0"
[target]    os = "linux"  architecture = "x86_64"  abi = "c7abi1"
[build]     mode = "debug"  optimization = "O0"  bounds_check = false  overflow_check = true  debug_alloc = false
[diagnostics] language = "en"          # "en" | "ko"
[link]      mode = "static"            # "static" | "dynamic"
[paths]     include = [] library = [] registry = []
```
알 수 없는 키 `W0701`, 잘못된 값 `E0719`. 탐색: `--cset7 파일` > 프로젝트 디렉터리의 `.cset7`. (전역 파일·상속은 §6 Q1.)

**(c) 빌드 실행 모델**
- 타깃 단위 그래프(`depends` + 의존성). 라이브러리 타깃 = `c7c check <소스> [--lib 의존성…]` 후 **인프로세스로 `.l7` 패키징**. 실행 파일 타깃 = 자식 프로세스 `c7c build <소스…> --lib <의존 .l7…> -I… -D… -O<n> [--debug] [-fbounds-check] [-fdebug-alloc] -o <출력> --depfile <목록>`.
- **증분·병렬의 단위는 타깃이다.** 컴파일러가 전체를 한 번에 분석(분리 컴파일·`.h7` 인터페이스 없음)하므로 파일 단위 증분은 불가능 → §5 L1.
- 병렬: `-jN`(기본 하드웨어 스레드 수), 의존 타깃이 끝난 뒤에만 시작, 독립 타깃은 동시에. 한 타깃이 실패하면 새 작업은 시작하지 않고 실행 중인 것은 끝까지 기다린 뒤 실패를 보고. 로그는 타깃별로 모아 **결정적인 순서**로 출력.
- 호스트/타깃 구분: 타깃 `platform`이 호스트와 다르면 지금은 `E0715`(지원 플랫폼은 x86_64-linux뿐). 표현은 유지.

**(d) 캐시** (`<프로젝트>/.c7cache/<키>/`)
- 키 = SHA-256(캐시 버전, **컴파일러 지문**(= `c7c` 실행 파일 + `runtime/c7rt.c` + `std.l7`의 해시), 타깃 종류·이름·플랫폼, 정규화된 옵션 문자열, 정렬된 (소스 상대 경로, 소스 해시), 정렬된 (라이브러리 이름, 버전, `.l7` 해시)).
- **직접 모드**: 항목에 `--depfile`이 알려 준 파일들의 해시(`deps.txt`)를 함께 저장하고, 조회 때 현재 해시와 모두 같을 때만 적중으로 본다 → `#include`된 헤더가 바뀌면 정확히 무효화.
- 저장은 임시 파일에 쓴 뒤 `rename`(원자적, 병렬 안전). 적중 시 산출물을 복사하고 `.mp7`는 다시 만든다.

**(e) `.cr7` 잠금** (`Cmake7109.cr7`, 프로젝트 디렉터리)
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
[build]
config = "release"
[[dependency]]
name = "Other"
version = "1.2.5"
source = "registry:../registry"      # 또는 "path:../local"
sha256 = "<.l7 파일의 SHA-256>"
```
`c7c lock`만 파일을 쓴다. 빌드는 읽기만 하며 **절대 자동 갱신하지 않는다**(명세). 요구 조건과 잠금이 어긋나면 `E0714`, 해시가 다르면 `E0710`.

**(f) 의존성 해결**: 모든 제약을 모아 패키지마다 **조건을 만족하는 최고 버전**을 고른다. 선택한 버전의 매니페스트 `[[dependency]]`로 제약을 확장해 고정점까지 반복. 어떤 패키지도 모든 제약을 만족하지 못하면 **자동 변경 없이** `E0713`(누가 어떤 제약을 걸었는지 나열). v1은 백트래킹 없음. 레지스트리 디렉터리의 `<이름>-<버전>.l7`은 **파일명과 매니페스트가 일치**해야 한다. 출처는 인터페이스(`PackageSource`)로 추상화: `LocalRegistrySource`, `PathSource`(구현), `CentralSource`·`GitSource`·`EnterpriseSource`(스텁 → `E0716` "not implemented").

**(g) `.mp7` 맵 파일** (`<출력>.mp7`)
```text
C7109 MAP FILE
Format: 1
Project: Example
Target: app (executable)
Platform: x86_64-linux (host: x86_64-linux)
Configuration: release (mode=release, optimization=O2)
Compiler: c7c 1.0.0-alpha.1

[Inputs]
src/main.c7  sha256=<hex>  bytes=<n>
[Libraries]
mylib 1.0.0  sha256=<hex>
[Outputs]
build/app  bytes=<n>  sha256=<hex>
[Sections]          ; 주소는 16진수(0x…), 크기는 바이트
.text addr=0x… size=…
[Symbols]           ; _C7$ 심볼·main·c7_* 만. 최적화/링커가 제거한 심볼은 나열하지 않는다
0x… <크기> T example::main
```
`nm -n -S --defined-only`와 `readelf -SW`(또는 ELF 직접 읽기)로 만든다. **임시 파일에 쓰고 성공했을 때만 `rename`**(실패한 빌드가 성공한 맵을 남기면 안 됨 — 명세 권고). 도구가 없으면 해당 섹션을 `; unavailable: …`로 표시.

---

## 5. 작업 목록

표기: **크기** S(수 시간) / M(하루 안팎) / L(며칠). **선행**은 먼저 끝나야 하는 항목. 모든 항목의 공통 완료 조건은 §2·§9(경고 0건, `make test`·`make asan` 통과, 양성+음성 테스트, 문서 갱신).

### P0 — 연결과 정리 (먼저)

**W14 — 파서·ZIP 하드닝 (B1 수정 포함)** · M · 선행 없음 · **최우선**
- 해야 할 일: TOML 파서에 **중첩 깊이 한도**(배열·인라인 표 각각, 예: 128)를 두고 한도 초과는 `E0701`로 거부. 같은 관점으로 ZIP/`.l7`/(앞으로의) 빌드 파일 파서를 점검(재귀, 거대한 단일 줄, 거대한 문자열, 항목 수).
- 산출물: 수정 + `tests/toml/invalid/`에 깊은 중첩 케이스, **퍼징 하네스**(`tests/fuzz_pkg.py`: 유효 파일을 무작위 변이해 `c7c tool …`에 넣고 **종료코드가 0 또는 1이 아니면 실패**, 시간·메모리 한도).
- 완료 조건: B1 재현 명령이 종료코드 1로 끝남. 퍼징 N회(예: 5,000) 무사고. `make asan` 통과.
- 주의: 재귀를 반복문으로 바꾸는 것보다 한도가 단순하고 안전하다. 한도는 상수 하나로.

**W1 — 검증 스크립트 연결** · S · 선행 없음
- `tests/run.sh`에 `check_toml/zip/semver/l7.py`와 SHA-256 대조(길이별 `hashlib` 비교를 `tests/check_sha256.py`로 스크립트화)를 연결. `python3`<3.11·`unzip`·`sha256sum`이 없으면 건너뛰되 **건너뛴다고 출력**. `Makefile`의 `asan` 스윕에 `tests/toml/**`, 샘플 ZIP/`.l7`을 `c7c_asan tool …`로 통과시키는 루프 추가.
- 완료 조건: `make test`가 새 검사를 포함해 통과, 의도적으로 망가뜨리면 실패(변이 테스트).

**W2 — 드라이버 확장 자동 테스트** · S · 선행 W1 권장
- 대상: 여러 `.c7` 입력, `--lib`, `--depfile`, 변조 `.l7` 거부, **유닛 간 중복 선언**(`E0211` 계열), 라이브러리 안의 `main`, `.l7` 진단 파일명(`x.l7!src/a.c7:줄:칸`) 표기, 없는 추가 소스(`E0704`), B3(depfile에 `.l7`이 섞임)을 의도한 동작으로 정리하거나 제거.
- 테스트는 `tests/run/` 방식(`*.expected`, `.rc`) 또는 새 디렉터리. `-O0`/`-O2`와 `-fdebug-alloc` 재실행이 자동으로 따라오는지 확인.

**W3 — 컴파일러 버전의 단일 출처 (B2)** · S
- 한 헤더(예: `compiler/pkg/version.hpp`)에 `kCompilerVersion`·`kAbiId`·언어 버전을 두고 `--version`, 패키지 검증, 락 파일이 모두 그것을 쓰게 한다. `c7c --version`이 새 형식(`c7c 1.0.0-alpha.1 (ABI c7abi1)`)을 출력하는 테스트.

**W4 — `std.l7` (B4)** · M · 선행 W2 (최종 형태는 W5·W8 이후)
- 1단계(임시): `Makefile` 규칙이 `./c7c tool l7-pack`으로 `std.l7`(이름 `std`, 버전 `1.0.0`, 소스 `src/*.c7`)을 만든다. `make`가 `c7c`와 `std.l7`을 같이 만들고 `make test`가 `std.l7` 경로로 돈다.
- 2단계(최종): `stdlib/Cmake7109.txt`(`target library "std"`, `std "none"`, `output "../std.l7"`)로 **자기 자신의 빌드 시스템으로** 만든다(dogfooding). 1단계 규칙을 대체.
- 완료 조건: `std.l7`이 **재현 가능**(두 번 만들어 바이트 동일), 로더 세 경로(`std.l7`, `C7_STDLIB` 폴더, `stdlib/` 폴백)를 각각 시험하는 테스트, `std.l7` 변조 시 컴파일 거부, 라이선스 문자열(`GPL-3.0-only` + 런타임 라이브러리 예외 — 정확한 SPDX 표기는 §6 Q12).

### P1 — 설정과 빌드 파일

**W5 — `Cmake7109.txt` 파서·해석** · L · 선행 없음 (W14의 한도 원칙 적용)
- 해야 할 일: 줄 단위 렉서(들여쓰기·주석·문자열·리스트), 일반 AST, 문맥별 허용 키워드 해석, 진단: `E0702`(문법·탭·들여쓰기 불일치), `E0703`(모르는 키워드), `E0704`(소스 없음), `E0705`(타깃 이름 중복), `E0708`(충돌 옵션: 같은 `defines` 이름의 다른 값, 중복 소스, 모순 속성), `E0719`(잘못된 값). **줄:칸이 정확한** 진단.
- 산출물: `compiler/pkg/buildfile.{hpp,cpp}`, **`docs/Cmake7109-grammar.md`**(EBNF + 속성 표), `tests/build/valid|invalid/*.txt` 코퍼스, 새 `c7c tool buildfile-json 파일`(해석 결과 덤프 — 테스트용).
- 완료 조건: 유효 20개·무효 30개 이상(각 진단 코드를 최소 1개씩 유발), 모든 경로는 프로젝트 디렉터리 기준 상대, **프로젝트 밖으로 나가는 경로(`../`)는 명시적으로 허용/거부 정책을 정해 테스트**(§6 Q13).

**W6 — `.cset7` 로더·병합** · M · 선행 W5 권장(옵션 모델 공유)
- 스키마(§4.3 b), 검증(`E0719`), 알 수 없는 키 `W0701`, 우선순위 병합(§4.3 a) 구현. `Options` 구조체 하나로 `CodegenOptions`와 컴파일러 플래그를 만든다(현재 `--debug`는 `po.debug`와 `cg.overflowCheck`를 같이 켠다 — 그 의미를 그대로 보존).
- 테스트: 우선순위 표 전수(기본값/`.cset7`/`config`/타깃/명령행 5단계), 잘못된 값, 알 수 없는 키.

**W7 — 빌드 그래프** · M · 선행 W5
- 타깃 노드, `depends`·`dependency` 간선, 위상 정렬, 순환 `E0706`(순환 경로를 `a -> b -> a`로 출력), 출력 경로 충돌 `E0707`(두 타깃이 같은 출력, 출력이 소스와 같음), 플랫폼 `E0715`, 호스트/타깃 구분 표현.
- 테스트: 순환 직접/간접, 다이아몬드 의존성, 출력 충돌, 정렬의 결정성(같은 입력 → 같은 순서).

### P2 — 실행

**W8 — 타깃 실행기와 병렬 스케줄러** · L · 선행 W7, W10의 일부
- 자식 `c7c` 프로세스(`posix_spawn`/`fork+exec`), 타깃별 로그 파일, 라이브러리 타깃(check → 패키징), 실행 파일 타깃, 실패 처리 `E0720`, `-jN`.
- 완료 조건: ① 의존 순서 보장(라이브러리가 먼저), ② 독립 타깃 둘이 **둘 다 시작된 뒤에** 끝남(`--verbose` 로그의 시작/종료 이벤트 순서로 검증; 단일 코어에서도 성립), ③ 한 타깃 실패 시 새 작업 중단·실행 중 대기·종료코드 1, ④ 출력 순서 결정성, ⑤ 같은 `-j`에서 반복 실행해도 결과 동일.
- 주의: 자식의 종료 대기는 `waitpid(-1)`로, 로그는 타깃별 버퍼 후 한꺼번에 출력해 뒤섞이지 않게.

**W9 — 캐시** · L · 선행 W8
- §4.3(d). 무효화 시나리오 테스트(**각각 독립 테스트**): 소스 내용 변경, 소스 추가/삭제, `#include`된 헤더 변경(직접 모드), 옵션 변경(`-O`, `defines`), 의존 라이브러리 변경, **컴파일러 바이너리 변경**, `std.l7` 변경, 플랫폼 변경 → 모두 재빌드. 변경 없음 → 적중(실제로 자식 프로세스를 띄우지 않음을 `--verbose`로 확인). 손상된 캐시 항목은 무시하고 재빌드. `--no-cache`, `c7c clean`.
- 동시 실행 안전: 같은 키를 두 프로세스가 쓸 때 깨지지 않음(임시+`rename`).
- 변이 테스트: 키에서 한 요소를 빼면(예: 컴파일러 지문) 해당 무효화 테스트가 실패해야 한다.

**W10 — 프로젝트용 CLI** · M · 선행 W5, W7 (W8과 병행)
- `compiler/pkg/project.cpp`(B5)에서 `isProjectCommand`/`runProjectCommand` 구현: `c7c build [경로] [--target 이름]… [--config 이름] [-jN] [-O<n>] [--mode …] [--cset7 파일] [--registry 디렉터리] [--cache-dir …] [--no-cache] [--out-dir …] [--verbose]`, `c7c pack`, `c7c verify`, `c7c install`, `c7c lock`, `c7c clean`. 기존 `c7c build 파일.c7` 동작은 **그대로 유지**(`.c7`로 끝나는 인수가 있으면 단일 파일 모드).
- `c7c --help`/usage 갱신, 종료 코드 규약(0 성공, 1 빌드 오류, 2 사용법 오류).

### P3 — 의존성

**W11 — 경로 의존성·레지스트리·해결기** · M · 선행 W5, W7
- 경로 의존성(`.l7` 파일 또는 하위 프로젝트 디렉터리 — 후자는 그 프로젝트의 같은 이름 라이브러리 타깃을 그래프에 추가), 로컬 레지스트리(`<이름>-<버전>.l7`, 파일명↔매니페스트 일치 검증), §4.3(f)의 해결기, `PackageSource` 인터페이스와 스텁(`E0716`).
- 테스트: 최고 버전 선택, 전이적 제약, 충돌 `E0713`(메시지에 요구자 표시), 프리릴리스 제외, 파일명/매니페스트 불일치, 변조된 레지스트리 파일 거부, 순환 의존(`E0706`).

**W12 — `.cr7` 잠금** · M · 선행 W11
- §4.3(e). `c7c lock`(쓰기), 빌드(읽기 전용), 어긋남 `E0714`, 해시 불일치 `E0710`.
- **반드시 있어야 할 테스트**: ① 빌드만으로 `.cr7` 바이트가 바뀌지 않음(읽기만으로 갱신 금지), ② 레지스트리에 더 높은 버전이 생겨도 잠금의 버전을 쓴다, ③ `c7c lock`을 다시 실행해야만 새 버전을 고른다, ④ `.cr7`와 요구 조건이 어긋나면 오류.

### P4 — 산출물

**W13 — `.mp7`** · M · 선행 W8
- §4.3(g). `_C7$경로$이름$$타입` 심볼을 `경로::이름(타입)`으로 복호화(선택, §6 Q8), 주소는 `0x` 16진수·크기 바이트 명시, 최적화·링커가 제거한 심볼 표시 문구, 임시 파일 + 성공 시 `rename`, 실패한 빌드는 맵을 남기지 않고 **이전의 맵도 낡은 채로 남기지 않음**(삭제 또는 덮어쓰기 — 정책을 정해 테스트).
- 테스트: 알려진 프로그램의 심볼이 맵에 존재, `nm` 주소와 일치, 도구가 없을 때의 폴백 문구, 맵 파일이 UTF-8/LF.

### P5 — 설치·문서·품질

**W16 — `c7c install`·호환성 검사** · M · 선행 W11
- OS·아키텍처·ABI 호환 확인. 소스 패키지는 ABI 일치만 요구, **바이너리만 있는 패키지는 일치하는 `[[binary]]`가 없으면 `E0712`**(다른 플랫폼 바이너리로 대체 금지 — 명세). 설치 레이아웃(예: `<prefix>/<이름>-<버전>/`)은 §6 Q14에서 결정. 이미 있는 대상은 덮어쓰지 않음(구현된 `extractPackage` 재사용), 심볼릭 링크 처리.

**W15 — 문서화** · S~M · 마지막에, 그러나 **각 작업과 함께 점진적으로**
- `docs/stage5-decisions.md`: 명세와 달라진 점·정한 점(이 문서 §4.2 이관)과 이유. `STATUS.md`: 5단계 상태·테스트 수·디렉터리 구조 갱신. `docs/spec-D8-draft.md`: D.8.11+로 5단계 결정 추가. `docs/specs/file-formats/*.md`: **구현이 확정한 항목을 명세에 반영하는 변경 제안**(원문은 "구버전 초안"이므로 `미결정` 항목이 해소되면 갱신하되 소유자 확인 표시).

### 나중으로 미룬 것 (이 단계에서 하지 않는다)

| ID | 내용 | 막힌 이유 |
|---|---|---|
| L1 | **분리 컴파일과 `.h7` 인터페이스** → 파일 단위 증분 빌드, 바이너리 `.l7`(`lib/<os>-<arch>/` 채우기) | 컴파일러가 전체를 한 번에 분석. 규격서 9단계(최적화·LTO) 이후 |
| L2 | `.l7` **서명 체계** (SHA-256은 발행자 인증이 아님 — 명세) | 알고리즘 미결정 |
| L3 | 실제 **크로스 컴파일** | 다른 타깃 백엔드가 없음(Windows 7단계, AArch64 10단계, x86 11단계) |
| L4 | 빌드 파일의 변수·조건·반복·사용자 명령·스크립트 보안 모델 | 명세 `미결정` — 소유자 결정 필요 |
| L5 | 중앙·Git·사내 레지스트리 | 인터페이스(스텁)만 요구됨 |
| L6 | 바이트 단위 동일한 실행 파일 재현 | 명세가 "별도 규격 사항"으로 분리(`.l7` 패키지의 재현성은 이미 구현) |
| L7 | Windows 경로·`.exe`·링커 | 7단계 |

---

## 6. 소유자 결정이 필요한 질문

작업자는 **기본 가정**대로 진행하되 코드에 `⚠ Q번호`를 남기고, 확정되면 한 곳을 고친다.

| Q | 질문 | 기본 가정 |
|---|---|---|
| Q1 | `.cset7`의 **전역 설정 파일 위치**, `include`/`inherit`, 환경 변수 치환 | 지원하지 않음. `--cset7 파일` > 프로젝트의 `.cset7`만 |
| Q2 | 알 수 없는 키 정책 (`.cset7`, `manifest.toml`, `.cr7`) | **경고**(`W0701`), 오류 아님. 잠금 파일은 오류로 할지? |
| Q3 | `mode "debug"` = `--debug`(오버플로 검사)로 켠다는 것 (4.6절 원문 미확인 가정) | 그렇다 |
| Q4 | 패키지 이름 규칙(대소문자 구분? 허용 문자)과 레지스트리 파일명 `<이름>-<버전>.l7` | 대소문자 구분, 현재 §4.2 규칙 |
| Q5 | ABI 호환성 규칙과 `[compiler] requires`의 프리릴리스 의미 (`1.0.0-alpha.1` 컴파일러는 `>=1.0.0`을 **만족하지 않는다** — 현재 npm식 규칙) | ABI 문자열 완전 일치. 프리릴리스 규칙 유지(그래서 `std.l7`은 `requires`를 쓰지 않는다) |
| Q6 | 잠금 파일 이름·위치 | `Cmake7109.cr7`, 프로젝트 디렉터리 |
| Q7 | 의존성 `source` 식별자 형식 (명세 예시는 `"official"`) | `registry:<경로>` / `path:<상대경로>` |
| Q8 | `.mp7` 필수 섹션·주소 표기·심볼 복호화 여부·라이브러리 타깃도 만들지 | §4.3(g), 복호화함, 실행 파일만 |
| Q9 | ZIP 크기 한도(항목 256 MiB / 전체 1 GiB / 10,000개) | 현재 값 |
| Q10 | 빌드 출력 `build/`, 캐시 `.c7cache/`를 **프로젝트 안**에 둘지 사용자 캐시 디렉터리에 둘지 | 프로젝트 안 |
| Q11 | 빌드 파일에서 변수·조건·사용자 명령 지원 시점과 보안 모델 | 이번에는 지원하지 않음 |
| Q12 | `std.l7`의 버전·라이선스 SPDX 표기(`GPL-3.0-only WITH C7109-runtime-library-exception`?) | 버전 `1.0.0`, 위 문자열 |
| Q13 | 빌드 파일이 **프로젝트 디렉터리 밖**을 가리키는 경로(`../x`, 절대 경로)를 허용할지 (경로 의존성·레지스트리는 필요) | 소스·include는 프로젝트 안만, `dependency … path`·`registry`만 밖을 허용 |
| Q14 | `c7c install` 설치 위치와 레이아웃 | `<prefix>/<이름>-<버전>/`, 기본 prefix는 사용자 디렉터리 |

---

## 7. 진단 코드 (E07xx)

모두 `compiler/frontend/diag_codes.def`에 **이미 등록**돼 있다. "쓰임" 열이 `미사용`인 코드는 해당 작업에서 처음 발생시킨다.

| 코드 | 의미 | 쓰임 |
|---|---|---|
| E0701 | 올바르지 않은 TOML | 사용 중 (TOML 파서) |
| E0702 | 빌드 파일 문법 오류 | 미사용 → W5 |
| E0703 | 알 수 없는 키워드 | 미사용 → W5 |
| E0704 | 소스 파일 없음 | 사용 중 (드라이버의 추가 소스) → W5도 사용 |
| E0705 | 타깃 이름 중복 | 미사용 → W5 |
| E0706 | 의존 관계 순환 | 미사용 → W7, W11 |
| E0707 | 출력 경로 충돌 | 미사용 → W7 |
| E0708 | 충돌하는 옵션 | 미사용 → W5, W6 |
| E0709 | 올바르지 않은 패키지/매니페스트 | 사용 중 (`.l7`) |
| E0710 | 체크섬 검사 실패 | 사용 중 (`.l7`) → W12 |
| E0711 | 패키지 안의 안전하지 않은 경로 | 사용 중 (`.l7`) |
| E0712 | 이 플랫폼에 쓸 수 있는 내용이 없는 패키지 | 미사용 → W16 |
| E0713 | 모든 요구를 만족하는 버전이 없음 | 미사용 → W11 |
| E0714 | 잠금 파일이 최신이 아님 | 미사용 → W12 |
| E0715 | 지원되지 않는 대상 플랫폼 | 미사용 → W7 |
| E0716 | 의존성을 찾을 수 없음 / 소스 미구현 | 미사용 → W11 |
| E0717 | 올바르지 않은 버전·요구 조건 | 사용 중 (`.l7`) |
| E0718 | ZIP 오류 | 사용 중 |
| E0719 | 잘못된 설정 값 | 미사용 → W5, W6 |
| E0720 | 타깃 빌드 실패 | 미사용 → W8 |
| W0701 | 알 수 없는 키(무시) | 사용 중 (매니페스트) → W6 |
| W0702 | 일반 경고 | 미사용 (필요할 때만) |

---

## 8. 테스트·검증 전략

| 대상 | 정답 기준(오라클) | 스크립트 |
|---|---|---|
| SHA-256 | `hashlib`, `sha256sum` | (W1에서 `tests/check_sha256.py`) |
| TOML | Python `tomllib` (파싱 결과 **타입까지**, 직렬화 왕복) | `tests/check_toml.py`, 코퍼스 `tests/toml/{valid,invalid,stricter}/` |
| DEFLATE/ZIP | `zlib`, `zipfile`, `unzip -t` | `tests/check_zip.py` |
| SemVer | SemVer 2.0.0 명세 예시, Cargo 문서 | `tests/check_semver.py` |
| `.l7` | **독립 구현**(파이썬)으로 만든 패키지 수용 + `sha256sum -c` + 변조 20종 | `tests/check_l7.py` |
| 전처리기(참고 사례) | `gcc -E` 토큰열 대조 | `tests/check_pp_gcc.sh` |
| `Cmake7109.txt`·`.cset7`·`.cr7` | 오라클 없음 → **코퍼스 + 손으로 검산한 기대 출력 + 변이 테스트** | (W5, W6, W12) |
| 빌드 그래프·캐시·병렬 | 시나리오별 독립 테스트, `--verbose` 이벤트 로그로 순서 검증 | (W7–W9) |

**변이 테스트 방법**: 구현의 한 줄을 일부러 망가뜨리고(예: `if (!listed.count(...)) → 무시`) 새 테스트가 **실패하는지** 확인한 뒤 되돌린다. 이번 단계에서 TOML 직렬화기, SemVer 와일드카드, 패키저 검증 누락이 모두 이런 대조에서 발견됐다. "테스트가 통과했다"보다 "테스트가 결함을 잡았다"가 증거다.

**오라클이 틀릴 수도 있다**: `tomllib`은 int64 범위 밖 정수를 받아 주지만 TOML 명세는 거부한다. 이런 경우는 `tests/toml/stricter/`처럼 **명세 기준**으로 분리해 기록한다.

---

## 9. 리뷰 체크리스트

- [ ] `make clean && make` 경고 0건, `make test` 전부 통과, `make asan` 이상 없음
- [ ] 새 진단 코드는 `diag_codes.def`에 등록(영·한), 사용처에서 의도대로 발생
- [ ] 양성 + 음성 테스트, 가능하면 독립 오라클 대조, 기대값은 손으로 검산
- [ ] **변이 테스트로 새 테스트가 결함을 잡는지 확인**했다
- [ ] 외부 입력(파일·ZIP·TOML·경로)의 한도·중첩·경로 탈출을 다뤘다
- [ ] 미결정 사항은 §6에 질문으로 올리고 코드에 `⚠ Q번호` 표시, 소유자 확정 사항(§4.1)은 그대로
- [ ] 결과물이 **결정적**이다(같은 입력 → 같은 바이트/순서). 타임스탬프·해시맵 순회 순서에 의존하지 않는다
- [ ] 예약어를 이름으로 쓰지 않았다, 출력이 UTF-8/LF
- [ ] `STATUS.md`와 이 문서의 상태를 갱신했다
- [ ] 변경이 한 가지 주제로 작다

---

## 10. 파일 안내

```
compiler/pkg/            5단계 모듈 (sha256, toml, zip, semver, package, tools, project[껍데기])
compiler/driver/main.cpp 드라이버 (--lib, --depfile, 다중 입력, std.l7 로더, tool/project 명령 분기)
compiler/frontend/diag_codes.def   진단 코드 레지스트리 (E07xx 포함)
tests/check_*.py         5단계 검증 스크립트        tests/toml/   TOML 코퍼스
docs/specs/file-formats/ 파일 형식 명세 원문(구버전 초안, 5개 형식 + README)
docs/spec-D8-draft.md    규격서 부록 D.8 갱신 문안
STATUS.md                프로젝트 전체 현황
bench/abi/               ABI 측정 (4단계 결정의 근거)
```
