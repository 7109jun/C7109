# `Cmake7109.txt` 문법 (5단계 초안 ⚠)

구현: `compiler/pkg/buildfile.{hpp,cpp}`. 확인용 덤프: `c7c tool buildfile-json 파일 [--check-files] [--lang=ko]`.
코퍼스: `tests/build/valid/*.txt`(+손으로 쓴 `.json`), `tests/build/invalid/*.txt`(+`.expect` = 코드와 줄:칸). 검사: `tests/check_buildfile.py`.
빌드 파일은 **순수 선언형**이다(변수·조건·반복·사용자 명령 없음 — 가이드 Q11, 미결정).

## 1. 어휘

- 파일은 UTF-8(아니면 `E0702`), 줄 구분은 `\n` 또는 `\r\n`(그 밖의 `\r`은 `E0702`).
- 줄 한도: 16 KiB, 100,000줄, 들여쓰기 단계 8개(`kBuildMax*`). 넘으면 `E0702`.
- **들여쓰기는 공백만.** 들여쓰기 안의 탭은 `E0702`. 토큰 사이의 탭은 공백으로 취급.
- `#`부터 줄 끝까지 주석(문자열 안의 `#`은 주석이 아님). 빈 줄·주석만 있는 줄은 무시.
- 토큰: 식별자 `[A-Za-z_][A-Za-z0-9_-]*`, 문자열 `"..."`(이스케이프 `\"` `\\` `\n` `\t`만 허용, 제어 문자 금지), `:`. 그 밖의 문자는 `E0702`.
- 열 번호는 **바이트** 기준(1부터).

## 2. 구조 (들여쓰기 트리)

더 깊이 들여쓴 줄은 바로 앞 줄의 **블록**이다. 줄어들 때는 이미 열려 있는 단계의 너비와 정확히 같아야 한다(아니면 `E0702`). 블록마다 너비는 자유롭다(같은 블록 안에서만 일치).

## 3. 문법 (EBNF)

```ebnf
file        = { top } ;
top         = project | registry | dependency | config | default_config | target ;

project     = "project" STRING [":"] NL [ INDENT { pprop } DEDENT ] ;
pprop       = ("version" | "license" | "description") STRING NL ;

registry    = "registry" STRING NL ;
dependency  = "dependency" STRING ( STRING | "path" STRING ) NL ;

config      = "config" IDENT [":"] NL [ INDENT { cprop } DEDENT ] ;
cprop       = ("mode" | "optimization") STRING NL
            | ("bounds_check" | "debug_alloc" | "overflow_check") ("true" | "false") NL ;
default_config = "default_config" IDENT NL ;

target      = "target" ("executable" | "library") STRING [":"] NL INDENT { tprop } DEDENT ;
tprop       = list | ("output" | "platform" | "link" | "std") STRING NL ;
list        = ("sources" | "include_dirs" | "defines" | "depends") ":" NL [ INDENT { STRING NL } DEDENT ] ;
```

`project`는 파일에 정확히 하나. 한 키는 블록 안에서 한 번만(둘이면 `E0708`).

## 4. 속성과 값 검사

| 위치 | 키 | 값 | 검사 |
|---|---|---|---|
| project | `version` | SemVer | `E0717` |
| project | `license`, `description` | 문자열 | |
| 최상위 | `registry` | 경로 문자열(프로젝트 밖 허용) | 빈 값 `E0719` |
| 최상위 | `dependency NAME REQ` | 요구 조건(`^1.2`, `>=1.0, <2.0`, `1.*` …) | `E0717` |
| 최상위 | `dependency NAME path P` | 경로(프로젝트 밖 허용) | 빈 값 `E0719` |
| config | `mode` | `"debug"` \| `"release"` | `E0719` |
| config | `optimization` | `"O0"`…`"O3"` | `E0719` |
| config | `bounds_check` `debug_alloc` `overflow_check` | `true` \| `false` | `E0719` |
| target | `sources:` | 파일 경로 목록(필수, 비어 있으면 안 됨) | 아래 §5 |
| target | `include_dirs:` | 디렉터리 경로 목록(`"."` 허용) | §5 |
| target | `defines:` | `NAME` 또는 `NAME=VALUE` | `E0719`(이름), `E0708`(중복·충돌) |
| target | `depends:` | 타깃/의존성 이름 목록 | `E0719`, `E0708`(중복) |
| target | `output` | 경로 | §5 |
| target | `platform` | `"<arch>-<os>"`, 소문자·숫자·`_` | `E0719` (지원 여부는 W7의 `E0715`) |
| target | `link` | `"static"` \| `"dynamic"` | `E0719` |
| target | `std` | `"none"` \| `"default"` | `E0719` |

타깃·의존성 이름: 영문자로 시작, 영숫자·`_`·`-`, 64자 이하(패키지 이름 규칙과 같다, Q4). 타깃 이름은 서로, 그리고 의존성 이름과도 겹치면 `E0705`.

## 5. 경로 정책 (⚠ Q13)

`sources`, `include_dirs`, `output`은 **프로젝트 디렉터리 안**의 상대 경로만 허용한다. 어긴 것은 `E0719`:
절대 경로, 드라이브 문자(`C:`), 역슬래시, 제어 문자, 프로젝트 밖으로 나가는 `..`(정규화한 결과로 판단 — `a/../b`는 `b`). 소스는 파일이어야 한다(`"src/"`, `"."` 거부).
정규화는 어휘적(`./`, `//`, `x/..` 제거). `dependency … path`와 `registry`만 밖을 가리킬 수 있다. `--check-files`(또는 5단계 빌드 CLI)는 모든 소스가 실제 파일인지 확인하고 없으면 `E0704`.

## 6. 진단 요약

| 코드 | 이 파일에서 |
|---|---|
| E0702 | 문법: 탭 들여쓰기, 닫히지 않은 문자열, 알 수 없는 이스케이프/문자, 들여쓰기 불일치, `project` 없음, `sources:` 없음/비어 있음, 잘못된 모양(콜론·토큰 수), 한도 초과 |
| E0703 | 알 수 없는 키워드(최상위, `project`·`config`·`target` 블록, 타깃 종류) |
| E0704 | `--check-files`: 목록의 소스 파일이 없음 |
| E0705 | 타깃 이름 중복, 타깃과 의존성 이름 충돌 |
| E0708 | 같은 키 두 번, `project` 둘, 중복 소스·include·define·depends·dependency·config, 같은 define의 다른 값 |
| E0717 | 버전·요구 조건이 올바르지 않음 |
| E0719 | 값이 올바르지 않음(모드, 최적화, 불리언, 플랫폼, 링크, std, 이름, 경로 정책, 없는 `default_config`) |

구문 오류(`E0702`의 어휘/들여쓰기)는 **첫 번째에서 멈춘다.** 의미 오류는 모아서 모두 보고한다.
