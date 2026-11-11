# C7109 프로젝트 현황 문서 (STATUS)

> **이 문서의 목적**: 이 문서와 같이 받은 zip만 읽고도 현재 상태를 파악하고 바로 이어서 개발할 수 있게 한다.
> **기준 시점**: 로드맵 **4단계 기능 완료·마무리 단계**(§5). 작업 트리는 **정상(green) 상태** — 클린 빌드 경고 0건, `make test` 129/129 통과 (5단계 포함), `make asan` 이상 없음.
> **이름 참고**: 이 프로젝트의 이름은 **C7109**(범용 시스템 프로그래밍 언어 + 네이티브 컴파일러 + 표준 라이브러리)다. 요청 문구에 "JetOS"라고 적혀 있었으나 이 코드베이스와 무관하며, 이전 대화에도 등장한 적이 없다.

---

## 1. 한눈에 보기

| 항목 | 상태 |
|---|---|
| 언어 | C7109 — C 계열 문법, 수동 메모리 관리, `Result`+`try` 오류 처리, 공유 코드 제네릭, 캡처 클로저 |
| 컴파일러 | C++20, 약 7,500줄. **소스 → 실행 파일**이 x86-64 Linux에서 동작 |
| 파이프라인 | 전처리 → 렉서 → 파서(AST) → 의미 분석(AST에 타입 기록) → **C7 IR** → LLVM IR(텍스트) → `llc` → `gcc` 링크 |
| 표준 라이브러리 | `stdlib/*.c7` (C7로 작성) — 현재 `core`(출력·panic·assert), `str`(Str 메서드·UTF-8 순회·`StrBuf`) |
| 런타임 | `runtime/c7rt.c` — 부트스트랩용 C 글루(출력, panic, 서술자 조회). 최종적으로는 C7로 대체 예정 |
| 테스트 | 110개(`make test`): 파서·골든 AST, 의미 분석(`tests/sema` pos/neg), 빌드·실행(`tests/run`, 프로그램마다 `-O0`/`-O2` + 디버그 할당자 + `--debug -fbounds-check` 재실행), 디버그 할당자 감지(`tests/dbg`), 오버플로·경계(`tests/chk`), 표준 라이브러리 `@test`(Debug·Release), 테스트 러너, 진단 코드 레지스트리, 전처리기(`tests/pp` 동결 출력 + gcc 대조) |
| 대상 플랫폼 | **x86-64 Linux** + **x86-64 Windows(PE, 크로스 컴파일)**. AArch64·x86-32는 미착수 |
| 외부 의존 | LLVM 18의 `llc`, `gcc`(링크용). 자체 링커는 6단계 예정이라 현재는 시스템 링커 사용 |

### 로드맵 진행표 (규격서 17장 기준)

| 단계 | 내용 | 상태 |
|---|---|---|
| 0 | 규격서·결정 확정 | ✅ 완료 |
| 1 | 프런트엔드(렉서·전처리기·파서·진단) | ✅ 완료 |
| 2 | 의미 분석 | ✅ 완료 (알려진 한계는 §7) |
| 3 | C7 IR + LLVM 코드 생성 (x86-64 Linux) | ✅ 기능 완료 / ✅ **ABI v1 = `fastcc`(잠정, x86-64 Linux 측정; §6.6)** |
| 4 | 런타임 + 표준 라이브러리 기초 | 🟡 **진행 중** (§5 참조) |
| 5 | 빌드 시스템·패키지(`Cmake7109.txt`, `.cset7`, `.cr7`, `.l7`) | 🟡 **기반 모듈 완료**(SHA-256·TOML·ZIP/DEFLATE·SemVer·`.l7`, 각각 독립 오라클로 검증). **남은 작업은 `CONTRIBUTING.md`** — 알려진 결함 B1(TOML 중첩 세그폴트) 포함 |
| 6 | 자체 링커 (ELF x86-64) | ❌ 미착수 |
| 7 | Windows x86-64 (COFF/PE/PDB) | 🟡 PE 실행 파일 생성·자립 런타임 완료, 실제 Windows CI에서 실행 검증(`docs/windows.md`). PDB·DLL import·PE 맵은 미착수 |
| 8 | 표준 라이브러리 확장(OS, 파일, 네트워크, 스레드, 암호, 압축 …) | ❌ 미착수 |
| 9 | 최적화·LTO·PGO·`@specialize`·`@multiversion` | ❌ 미착수 |
| 10 / 11 | AArch64 / x86 32-bit | ❌ 미착수 |
| 12 | 안정화·1.0 릴리스 | ❌ 미착수 |
| 13 | 셀프 호스팅 | ❌ 미착수 |

> ABI v1은 `bench/abi`의 측정으로 **`fastcc` 유지**를 잠정 확정했다(§6.6). 규격서 13.6절 레지스터 초안은 규격서가 저장소에 없고 구현도 없어 **비교하지 못했다**.

---

## 2. 빠른 시작

```bash
# 환경 (이 샌드박스에서는 apt로 설치했다. zip에는 포함되지 않음)
apt-get install -y llvm clang gcc make      # llc 18 필요

cd c7109
make            # ./c7c 빌드
make test       # 전체 110개 테스트
make asan       # ASan+UBSan으로 컴파일러를 다시 빌드해 모든 테스트 입력을 통과시킴

# 사용
./c7c build prog.c7 -o prog [-O0..3] [--emit=ir|llvm|obj|exe] [--keep] [--no-std]
./c7c check prog.c7          # 의미 분석까지만 (표준 라이브러리 포함)
./c7c test file.c7 [-O2] [-fdebug-alloc]   # @test 함수 수집·실행 (§5.4)
# 빌드 옵션 -fdebug-alloc: 추적 할당자(누수 보고 종료코드 70, 이중/잘못된 해제는 즉시 panic)
./c7c parse prog.c7          # 문법 검사만 (표준 라이브러리 없이)
./c7c ast   prog.c7          # AST 덤프
./c7c lex|pp prog.c7         # 토큰 / 전처리 결과
# 공통 옵션: -Idir -DNAME[=V] --lang=ko|en --diag-format=json --target=ARCH-OS -ffreestanding --debug
```

- `c7c`는 **실행 파일 옆**의 `runtime/c7rt.c`와 `stdlib/*.c7`를 찾는다 (`C7_RUNTIME`, `C7_STDLIB` 환경 변수로 변경 가능). 그래서 `c7c`를 프로젝트 루트에서 실행해야 한다.
- 예제 (Hello World):
  ```c7
  extern "C" { void c7_print_str(CStr s); void c7_newline(); }
  i32 main() { c7_print_str(c"Hello, C7109!"); c7_newline(); return 0; }
  ```
  또는 표준 라이브러리로 `println("Hello");`.

---

## 3. 디렉터리 구조

```
c7109/
├─ STATUS.md                  ← 이 문서
├─ CONTRIBUTING.md            5단계 남은 작업·기여 규칙·설계 초안·소유자 질문 (기여자는 여기서 시작)
├─ README.md                  짧은 요약
├─ Makefile                   임시 개발 빌드 (정식 빌드는 5단계의 Cmake7109.txt)
├─ compiler/
│  ├─ frontend/               프런트엔드 + 의미 분석 (약 4,700줄)
│  │  ├─ c7.hpp               공용 선언: Loc, Diag, Token, Node(AST+의미 분석 주석), 공개 API
│  │  ├─ diag.cpp             진단 엔진 + 영/한 메시지 표 + 정렬 출력 + JSON
│  │  ├─ lexer.cpp            규격서 3장 (리터럴, 주석, 구두점)
│  │  ├─ preproc.cpp          C 호환 전처리기 (#define/#if/#include …)
│  │  ├─ parser.cpp           재귀 하강 파서 → 균일한 Node AST
│  │  ├─ sema_core.hpp        타입(Type), Decl, Member, 변환 규칙 선언
│  │  ├─ sema_types.cpp       typeEq/subst/unify/litFits/castOk
│  │  ├─ sema_impl.hpp        Sema 클래스 선언 (모든 단계가 공유)
│  │  ├─ sema_decl.cpp        프렐류드, 선언 수집, 이름 해석(lookupGlobal), impl, import
│  │  ├─ sema_expr.cpp        변환·경로 해석·호출·오버로딩
│  │  ├─ sema_expr2.cpp       표현식 디스패처, 연산자, 멤버, 구조체 리터럴, 람다, alloc, try
│  │  └─ sema_stmt.cpp        문장, match 완전성, 반환 경로, 함수 본문, 진입점(run/analyze)
│  ├─ ir/
│  │  ├─ ir.hpp / ir.cpp      C7 IR: 타입, Builder, 텍스트 출력(.c7ir), 검증기
│  ├─ codegen/
│  │  └─ lower.cpp            AST → C7 IR 하향 (약 2,000줄, 클래스 `Lower`)
│  └─ driver/main.cpp         `c7c` CLI, stdlib 로더, llc/gcc 호출
├─ stdlib/                    C7로 작성된 표준 라이브러리 (컴파일마다 함께 로드)
│  ├─ core.c7                 extern 글루, panic/assert/unreachable/bitcopy, print*, AllocError
│  └─ str.c7                  Str 메서드, operator==/!=, Chars(UTF-8), StrBuf
├─ runtime/c7rt.c             C 부트스트랩 런타임 (출력, panic, c7_find_vtable, 디버그 할당자, c7_test_index)
├─ tests/pp/                  전처리기 기준 입력 (+ .expected 동결 출력, gcc와 토큰열 대조)
├─ docs/spec-D8-draft.md      규격서 부록 D.8에 붙여 넣을 문안
├─ bench/abi/                 ABI 측정 (run.py, *.c7, RESULTS.md, results/*.json) — `make abi-bench`
└─ tests/
   ├─ run.sh                  모든 테스트 러너
   ├─ cases/                  파서 테스트 (positive 01~05, neg_*)
   ├─ sema/                   의미 분석 테스트 (pos_*, neg_*)  — neg 파일은 `// expect: E0xxx` 주석으로 기대 코드 선언
   ├─ golden/                 파서 골든 AST (*.ast)
   └─ run/                    빌드+실행 프로그램 (*.c7 + *.expected [+ *.rc 기대 종료코드])
```

---

## 4. 구현 현황 (기능 매트릭스)

### 4.1 프런트엔드·의미 분석

| 기능 | 상태 | 비고 |
|---|---|---|
| 렉서 (정수/실수/문자/문자열/raw `r""`/C 문자열 `c""`, 이스케이프, UTF-8 검증) | ✅ | `>>`는 일부러 토큰화하지 않고 파서가 인접 `>`로 합성(중첩 제네릭용) |
| 전처리기 | 🟡 | 조건부·객체/함수 매크로·`__VA_ARGS__`·include·`#pragma once`·`__has_include`, **`#`(문자열화)·`##`(붙이기)·`__VA_OPT__`** (C11 6.10.3, C23). **`#embed`만 미지원**(`E0609`). 구현: `PP::substitute`/`stringize`/`pasteTokens`. 붙인 결과가 토큰 하나가 아니면 `E0614`, `#` 뒤에 매개변수가 없거나 `##`가 본문 끝이면 `E0606`. **gcc를 기준으로 검증**(`tests/check_pp_gcc.sh`) |
| 파서 (규격서 4~10장 문법 전체) | ✅ | 클래스·인터페이스·impl·제네릭·클로저·match·tagged union·`defer`·`try`·튜플·`out` |
| 타입 검사 / 암묵 변환(4.7절) / 리터럴 범위 | ✅ | |
| 가변성 (`E0301`/`E0302`/`E0303`) | ✅ | |
| 오버로딩 (`E0630`/`E0631`) | ✅ | 리터럴 `1`은 `i32`를 정확 일치로 취급 |
| 제네릭 제약 (`E0650`), 인터페이스 구현 완전성 (`E0640`/`E0641`) | ✅ | |
| `match` 완전성 (`E0450`), 반환 경로 (`E0410`), `defer` 규칙 (`E0470`) | ✅ | |
| `drop` 타입 이동 전용 (`E0480`) | 🟡 | 복사는 막지만 **이동 후 사용 추적은 없음** |
| `try`/`Result` (`E0610~E0612`) | ✅ | 오류 타입 변환(`Convert`)은 존재 여부만 검사 |
| 접근 제어 (`E0621`) | ✅ | `init`/`drop`은 기본 `pub` |
| 이름 해석 | ✅ | §6.3 참조 (현재 `namespace` → 전체 이름 → `import` 별칭 → 암묵 `std`) |
| `self` 표현식 | ✅ | `const T*` (mut 메서드·init·drop에서는 `T*`) |
| `goto`가 변수 범위 안으로 점프 (`E0460`) | ✅ | 소유자 결정: **모든 변수 선언**을 건너뛰면 오류. 규칙 하나 — 라벨 시점에 살아 있는 선언은 모두 `goto` 시점에도 살아 있어야 한다(`Sema::checkGotoScopes`). 변수 = 지역 변수·암묵 선언(`x = e;`)·튜플 분해·`for`/`for-in` 변수·`match` 바인딩, **그리고 `defer` 문**(소유자 결정: 건너뛴 `defer`가 스코프 끝에서 실행되는 것을 막는다; 메시지는 "scope of `defer`") |

### 4.2 코드 생성 (x86-64 Linux)

| 기능 | 상태 | 비고 |
|---|---|---|
| 함수, 재귀, 기본 타입, 전 제어문(if/while/do/for/for-in/switch/match/라벨/goto) | ✅ | |
| 포인터, 배열, 슬라이스, `Str`, 구조체, 공용체, 열거형, 튜플, `out` 매개변수 | ✅ | |
| 클래스: 메서드·`init`·`drop` RAII, 스코프 종료/`return`/`break`/**`goto`**에서 역순 `drop` | ✅ | `defer`와 같은 스택에서 역순 실행. `goto`(소유자 결정)는 빠져나가는 스코프의 정리를 실행한다: 라벨의 스코프 체인에 없는 스코프는 전부, 공통 스코프는 라벨 이후 등록분만. 앞쪽 라벨은 라벨 지점에서 채워지는 중간 블록(`GotoState`) |
| `alloc`/`free`(`zeroed`, 배열, 생성자 인수) | ✅ | 사용자 정의 할당자 `alloc(a)` ❌ |
| `extern "C"`, 전역 변수(상수 초기화) | ✅ | `thread_local` ❌ |
| **tagged union / `Option` / `Result` / `try` / 변형 `match`** | ✅ | 구체 인스턴스 레이아웃. `Ok()`(void 페이로드) 허용 |
| **제네릭 함수 (공유 코드 + 타입 서술자)** | ✅ | `T`는 주소로 표현, 동적 `alloca`+`memcpy`, 인터페이스 메서드는 vtable 간접 호출 |
| **클로저** | ✅ | `{코드, 환경}` 2워드, 스택/`heap` 환경, 자동·명시 캡처, `free_closure` |
| 이터레이터 `for (x in iter)` | ✅ | `next()`가 `Option<T>`를 돌려주는 타입 |
| 연산자 오버로딩 호출 (자유 함수/멤버) | ✅ | 의미 분석이 고른 함수를 `Binary` 노드에 기록 |
| 0 나눗셈 | ✅ | 모든 빌드에서 `panic`(abort) |
| **Debug 정수 오버플로 검사** (4.6절) | ✅ | `--debug`일 때 정수 `+ - *`·단항 `-`(부호 있는/없는 모두)가 `panic`(abort, 134). 128비트 `*`·`<<`·나눗셈 `MIN/-1`은 검사 안 함, 부정된 리터럴(`-128`)은 제외. 꺼져 있으면 래핑(Release) |
| `-fbounds-check` | ✅ | 배열·슬라이스·`Str` 인덱싱과 `a[lo..hi]` 슬라이싱(음수 인덱스 포함). **원시 포인터는 검사하지 않음**(길이를 모름), 따라서 `Vec::at`은 자체 `assert` |
| **제네릭 타입의 메서드·생성자·`drop`** (`Vec<T>`), 메서드 수준 제네릭, `alloc T[n]`, `sizeof(T)` | ✅ | IR 매개변수 `[sret][self][서술자…][인수]`. 제네릭 구조체 레이아웃은 타입 인수와 무관(`T*` 필드만 허용) |
| `T`를 값으로 포함하는 집합체(`(T,i32)`, `Option<T>`, `T` 필드) | ❌ | `E0901` — API를 포인터/`out`으로 설계해 회피 |
| `T` 연산자(`+` 등), `Self`를 값으로 받는/돌려주는 인터페이스 메서드, 정적 인터페이스 메서드 | ❌ | `E0901` |
| 인터페이스 동적 디스패치(`Shape*`) | ❌ | 팻 포인터 미구현 |
| 캡처 없는 클로저 → `fn` 포인터 변환, 제네릭 안의 클로저 | ❌ | `E0901` |
| `drop` 타입을 제네릭 타입 인수로 사용 | ❌ | 서술자 drop 미지원 → `E0901` |

> 미지원 기능은 **조용히 틀린 코드를 만들지 않고 `E0901`로 명확히 거부**한다. 이 원칙을 유지할 것.

### 4.3 표준 라이브러리 (`stdlib/`)

| 모듈 | 내용 | 상태 |
|---|---|---|
| `core.c7` | extern 글루, `panic`/`assert`/`unreachable`/`bitcopy`, `print`/`println`/`print_i64`/`print_u64`/`print_f64`/`print_char`/`print_bool`/`newline`, `AllocError` | ✅ |
| `str.c7` | `impl Str { chars, as_cstr, empty, slice, starts_with }`, `operator==`/`!=`, `Chars`(UTF-8 디코딩 이터레이터), `StrBuf`(append/push_char/reserve/clear/as_str/as_cstr, 소멸자) | ✅ (`tests/run/stdstr.c7`로 검증) |
| `vec.c7` (`Vec<T>`) | `push`(`Result<void,AllocError>`)/`pop(out)`/`at`/`size`/`capacity`/`clear`, 소멸자 | ✅ (`tests/run/vec.c7`, ASan 누수 0 확인) |
| `arena.c7` (`Arena`) | 블록 하나를 bump 할당. `alloc_bytes(size, align)`, 메서드 제네릭 `create<T>(out T*)`/`create_array<T>(n, out T*)`(`T`는 `out` 인수 타입에서 추론), `reset`/`used`/`capacity`. 소멸자 없는 객체 전용 | ✅ (`tests/run/arena.c7`) |
| `hashmap.c7` (`HashMap<K : Hashable, V>`, `interface Hashable`) | 열린 주소법·선형 탐사·2의 거듭제곱 용량. `put`/`get`(→`V*` 또는 null)/`contains`/`remove`/`size`. `Hashable` 구현: `i32 i64 u32 u64 usize Str` | ✅ (`tests/run/hashmap.c7`) |
| 디버그 할당자 (`-fdebug-alloc`) | 런타임 `c7_dbg_malloc/calloc/free`: 블록 헤더 추적, 격리 큐(4096), 해제 메모리 0xDD 채움 | ✅ (`tests/dbg/*`) |
| 테스트 실행기 (`c7c test`) | §5.4 | ✅ (`tests/std/containers.c7`, `tests/testrunner/failing.c7`) |
| OS/파일/네트워크/스레드/수학·SIMD/암호/압축/시간/프로세스/터미널 | — | ❌ (8단계) |

---

## 5. 현재 진행 중인 작업 — 4단계

### 5.1 이미 끝난 것 (작업 트리에 반영·검증됨)
1. **이름 해석 개편**: 짧은 이름을 전역 등록하지 않고 `Decl::ns` + `Sema::curNs`로 조회. `import` 별칭, 암묵 `std` 프렐류드, 다중 단위(`analyze(d, units, &mod)`).
2. **표준 라이브러리 로더**: 드라이버가 `stdlib/*.c7`를 사용자 프로그램과 함께 파싱·분석·하향. `--no-std`로 끌 수 있음.
3. **프렐류드 정리**: `StrBuf`, `panic`/`assert`/`unreachable`/`bitcopy`, `Str`/`Chars` 메서드 선언을 프렐류드에서 빼고 `stdlib`가 본문을 제공. 프렐류드 타입(`Str` 등)에 라이브러리가 `impl`로 메서드를 붙일 수 있음.
4. **연산자 오버로딩 하향**, **이터레이터 `for-in` 하향**(`Lower::iterLoop`).
5. **의미 분석 선작업**(코드는 들어갔고 소비하는 하향 코드는 아직 없음):
   - `Node::ownerTargs` — 제네릭 소유 타입의 타입 인수(`Vec<i32>` → `[i32]`)를 호출/`alloc` 노드에 기록.
   - `Ok()`처럼 **`void` 페이로드 변형은 인수 없이 생성**, 패턴 `Ok`/`Ok()`도 허용.

### 5.2 제네릭 **타입의 메서드** 하향 + `Vec<T>` — ✅ 완료
구현 내용(`lower.cpp`): `Cleanup::ty`·`callDrop`(제네릭 클래스 `drop`은 서술자를 넘겨 호출), `declareGenericFn`/`lowerGenericBody`/`emitGenericCall`/`genGenericMemberCall`(IR 매개변수 `[sret?][self?][서술자…][일반 인수…]`, tparams = 소유 타입 + 메서드), `genAlloc`(`T` 원소 크기·제네릭 생성자)·`sizeof(T)`, `namedTy`(타입 인수에 `Param`이 있으면 `decl->full` 키의 단일 레이아웃), `self` 필드 접근의 타입에 `tparamTypes` 사용.
- 계획 대비 차이: 타입 인수가 모두 구체 타입이면 **기존 레이아웃 경로(인수별 치환)를 그대로** 쓴다(회귀 방지). 단일 레이아웃은 `Param`이 섞인 경우에만 쓰며, `T`를 값으로 가진 필드는 `irTy(Param)`에서 `E0901`로 거부된다.
- `Vec<T>`(`stdlib/vec.c7`)와 `tests/run/vec.c7`(`Vec<i32>`/`Vec<Point>`/힙의 `Vec<f64>`, 소멸자 경로 포함) 추가. 클린 빌드 경고 0, `make test` 40/40, `make asan` 이상 없음, 실행 파일을 ASan+LeakSanitizer로 링크해 누수 0 확인.

### 5.2.1 `Arena`·`HashMap`·디버그 할당자·`c7c test` — ✅ 완료
- **`alignof(T)`/`sizeof(T)`**: 의미 분석이 `Sizeof`/`Alignof`의 타입 노드에 `sty`를 기록하고(`sema_expr2.cpp`), 하향은 이를 읽는다(전에는 하향에서 `T`를 다시 해석하려다 `E0213`). `alignof(T)`는 서술자의 `align` 필드.
- **타입 인수 경계 검사(`E0650`)**: 이전에는 제네릭 *함수* 호출만 검사했고 `HashMap<NoHash, i32>`는 통과해 **런타임 panic**이 났다. 이제 `resolveType`(타입 표기)과 `TypeMember` 호출(`HashMap<K,V>()`)에서 `Sema::checkTypeArgBounds`가 검사한다. `impl` 수집이 끝나기 전(`implsReady == false`)에 만난 것은 `pendingBounds`에 모아 `declareAll()` 끝에서 검사한다. 테스트: `tests/sema/{pos,neg}_typebounds.c7`.
- **`-fdebug-alloc`**: 드라이버 → `analyze(..., debugAlloc)` → `lowerToIr` → `Lower::heapFn()`이 하향의 힙 호출 5곳(`alloc` 2, `free`, 클로저 환경 할당·`free_closure`)을 `c7_dbg_*`로 바꾼다. 누수는 `atexit`에서 보고하고 **종료코드 70**, 이중·잘못된 해제는 `c7_panic`(abort, 134). 한계: 할당 위치(소스 줄)는 기록하지 않는다(블록 번호·크기·주소만). `extern "C"`로 직접 부른 `malloc`은 추적하지 않으며, 그 포인터를 C7 `free`에 넘기면 "invalid free"가 된다.

### 5.4 `c7c test` 동작
1. `file.c7`를 `-DC7_TEST`로 전처리·파싱해 **최상위** `@test` 함수 이름을 수집한다.
2. 소스 끝에 `i32 main()`을 덧붙여 `c7_test_index()`(런타임이 환경변수 `C7_TEST_INDEX`를 읽음)에 따라 해당 테스트 하나만 호출하게 하고, **한 번만 빌드**한다(진단의 파일명·줄 번호는 원본 그대로).
3. 테스트마다 별도 프로세스로 실행하므로 `assert`/`panic`(abort)이 나머지 테스트를 막지 않는다. 출력 `test NAME ... ok|FAILED (exit N)`, 요약 `test result: P passed; F failed`, 하나라도 실패하면 종료코드 1.
- 테스트 파일은 **`main`을 직접 정의하지 않거나 `#ifndef C7_TEST`로 감싼다**, 그리고 **최상위(`namespace` 선언 없이)** 에 둔다.
- `@test void name()`의 시그니처 검사는 따로 없고, 맞지 않으면 합성 `main`의 호출에서 일반 의미 분석 오류가 난다.
- 규격서 예제의 `std::test::assert_eq`는 **아직 없다**(제네릭 값의 `==`가 미지원). 현재는 `assert(cond, "msg")`를 쓴다.

### 5.2.2 남은 선택 과제 (기존 알려진 한계)
- **`drop` 타입을 제네릭 인수로** (`Vec<StrBuf>`): `descOf`가 `E0901`로 명확히 거부한다. **단순 하향 문제가 아니다**: `drop` 타입은 값 전달이 `E0480`으로 막혀 있고(이동은 `return name`만 있음) 호출 인수의 이동 추적이 없어서, `push(T value)`를 허용하면 호출자와 컨테이너가 둘 다 소멸자를 돌려 **이중 해제**가 난다. 먼저 이동 의미론(값 인수의 이동·`move` 표기·이동 후 사용 금지)을 소유자가 정해야 한다. 서술자 `drop` 슬롯과 원소 소멸자 호출은 그 다음.
- **멤버 호출의 명시적 타입 인수**(`ar.create<u8>()`): 파서가 받지 않는다(`max<f64>(…)` 같은 자유 함수 경로만 된다). 지금은 인수에서 추론되는 API(`create(out p)`)로 우회.
- **반환 타입만으로 `T` 추론**: `resolveCall`은 인수에서만 추론한다. 못 찾은 `T`는 `Err`가 되어 하향에서 `E0901`로 거부된다.

### 5.3 4단계 검증 기준 (규격서 17.4절) — ✅ 충족
표준 라이브러리 단위 테스트(`@test`)가 Debug/Release에서 통과(`tests/std/containers.c7`, `-O0 -fdebug-alloc`와 `-O2`), 디버그 할당자가 의도적 누수·이중 해제를 감지(`tests/dbg/`). 4단계의 *기능* 항목은 모두 끝났고, 남은 것은 §10의 규격서 갱신(D.8)·`E0610` 충돌·ABI v1 같은 마무리 작업이다.

---

## 6. 아키텍처 핵심

### 6.1 파이프라인과 데이터 흐름
```
소스 ─preprocess→ 토큰 ─parse→ Node AST ─Sema→ (같은 AST에 타입·결정 기록) ─Lower→ ir::Module ─print→ .ll ─llc→ .o ─gcc→ 실행 파일
                                    ▲ stdlib/*.c7도 같은 경로로 파싱되어 units[1..]로 합류
```

### 6.2 의미 분석 → 하향의 계약 (`Node` 주석, `c7.hpp`)
| 필드 | 쓰는 곳 | 의미 |
|---|---|---|
| `sty` | `Sema::expr` 래퍼 | 표현식 타입 (선언 노드는 변수 타입) |
| `cty` | `expectConv`, `defaultType`, `resolveCall` | **암묵 변환 후 타입**. 하향은 `gen()`에서 이 타입으로 `coerce` |
| `decl` / `mem` | `resolveCall`, 연산자, 변형 생성 | 선택된 함수 `Decl*` / 메서드·생성자·변형 `Member*` |
| `flag` | `assignStmt` | `AssignOrDecl`이 새 변수를 선언하면 1 |
| `targs` | `resolveCall` | 호출된 제네릭의 **메서드 수준** 추론 타입 인수 |
| `ownerTargs` | `checkCall`/`checkAlloc` | **소유 타입**의 타입 인수 |

⚠ `Member*`는 `Decl::members` 벡터 요소를 가리킨다. `declareAll()`이 끝난 뒤에 멤버를 추가하면 포인터가 깨진다.

### 6.3 이름 해석 (`Sema::lookupGlobal`)
순서: **현재 네임스페이스(바깥으로 확장) → 전체 이름 → `import` 별칭 → `std::`+이름(암묵 프렐류드, 최저 우선순위)**. 선언은 **전체 이름으로만** 등록된다(`std::Vec`). 하향도 함수마다 `S.curNs = decl->ns`를 설정한다.

### 6.4 하향(`Lower`)의 주요 개념
- 함수마다 상태(`F`, `B`, `scopes`, `loops`, `retType`, `selfPtr`, `genericDescs` …). 람다는 `saveState()/restoreState()`로 중첩 함수를 만든다.
- 로컬은 항상 `alloca` 슬롯. ⚠ 드라이버는 `llc`만 실행하고 `opt`(mem2reg/SROA 등)는 **실행하지 않는다**(§6.6, §8.2). `V{ir::Val, TypeP}`가 값, `addrOf()`가 주소.
- **제네릭 코드에서 `T` 값은 "저장소 주소"로 표현**된다. `loadVal/storeVal/elemGep/paramSize/memcpyVal`이 이 규칙을 캡슐화.
- 서술자: `{i64 size, i64 align, ptr drop, i64 n, {i64 ifaceId, ptr vtable}[n]}` — 타입이 구현한 **모든** 인터페이스의 vtable을 담는다(그대로 전달 가능). `c7_find_vtable`(C 런타임)이 선형 탐색.
- 심볼 이름: `_C7$경로$이름$$매개변수타입…`(`main`·`extern "C"`는 장식 없음), 람다는 `_C7$lambda$N`.
- 타입 표현: `bool`=`i1`, `Char`=`i32`, `Str`/슬라이스=`{ptr,i64}`, 구조체=선언 순서, 공용체=정렬된 바이트 배열 래퍼, tagged union=`{태그, [n x iA]}`, 클로저=`{ptr,ptr}`.

### 6.6 ABI v1 측정 (`bench/abi/`, 상세 표는 `bench/abi/RESULTS.md`)
- **호출 규약은 `lower.cpp`의 `kC7CallConv` 한 곳**에서 정한다(`extern "C"`·`main`은 C 규약). 값을 바꿔 `fastcc`/`swiftcc`/`preserve_mostcc` 모두 전체 테스트 67/67 통과를 확인했다.
- 측정(`make abi-bench`, 같은 IR에서 규약만 교체, 6개 프로그램, CPU 시간 5회 최소/중앙값, 출력 일치 검증): `fastcc` ≈ C ABI(±3%). `preserve_most`는 호출이 많은 코드에서 1.14~1.47배 느림. `swiftcc`는 **32바이트(4워드) 값 반환·전달에서만 1.5배 빠르고**(`fastcc`는 숨은 포인터+스택, `swiftcc`는 rax/rdx/rcx/r8) 24바이트 이하(`Result<Str,E>` 포함)는 동일.
- **결정(잠정): ABI v1 = `fastcc` 유지.** 표준 라이브러리가 값으로 반환하는 가장 큰 집합체는 `Chars`(24B)라 `swiftcc`의 이득이 해당되지 않는다. 4워드 이상 값 반환이 흔해지거나 13.6 초안과 비교할 수 있게 되면 재검토. **새 타깃(AArch64·x86-32·Windows)마다 다시 측정**할 것.
- 한계: 샌드박스 **1코어** Xeon 2.3GHz 한 대에서의 측정이라 ~3% 이하 차이는 잡음이다.
- 발견: 생성 IR에 `alloca`가 그대로 남고 `-O`는 `llc`의 코드 생성 수준일 뿐 LLVM 최적화 패스가 없다. `opt`(mem2reg·SROA)를 먼저 돌려 봤더니 프로그램에 따라 빨라지기도(`big` 0.39→0.33초) 느려지기도(`result` 0.52→0.66초) 했다. 최적화 파이프라인 도입은 9단계에서 별도로 다룬다.

### 6.5 IR (`compiler/ir`)
LLVM IR 문법의 부분집합. `Builder`로 생성, `verify()`가 타입 일치·종결 명령·호출 서명을 검사(실패 시 `E0902`). 이 검증기가 실제 버그(생성자 반환형 불일치)를 잡은 적이 있다.

---

## 7. 결정 사항 색인

전체 규격서는 별도 문서(Claude Doc, 이 zip에는 **포함되지 않음**)에 있다: `https://claude.ai/artifact/CJZgB4kaVGwJ86Cp6SwN14` (본문 + 부록 A EBNF, B 미결정 목록, C 확정 대조표, **D 위임 결정 D.1~D.7**). 아래는 개발에 필요한 핵심 요약이다.

### 7.1 소유자 확정 사항 (변경 금지)
- 변수 `mut i32 value = 10;`(기본 불변), 세미콜론 필수, 타입 추론(`auto` 없음), 함수 반환형 앞, 사용자 타입 PascalCase / 기본 타입 소문자, `int=i32`·`long=i64`(플랫폼 독립)
- 기본 타입 `i8…i128`, `u8…u128`, `isize/usize`, `f32/f64`, `bool`, `Char`
- Raw Pointer만(대여 검사기 없음) + strict aliasing + `noalias`; Debug 오버플로 검사 / Release 래핑; 손실 변환은 `as`
- `Str`/`StrBuf`/`CStr`(UTF-8 `(ptr,len)`), `T[N]`·`T[]`, 경계 검사는 기본 꺼짐(옵션), `alloc T`·`alloc T[n]`·`free p`·`free[] p`, 실패 시 `null`, 사용자 할당자, `defer`, RAII(`drop`)
- `Result<T,E>` + `try`, 튜플, `out` 매개변수, 제어문 전체, `i32 main()`, `assert/panic/unreachable`(기본 abort)
- 클래스 = 문법적 설탕(상속·virtual 없음), 인터페이스 다형성, 연산자·함수 오버로딩, `Name<T>` **공유 코드 제네릭**, 클로저(명시·자동 캡처, 스택·힙), tagged union 레이아웃 제어
- `import`+`namespace`+C 전처리기(C23), `.h7`, `extern "C"`, 인라인 어셈블리 없음
- 자체 ABI(성능 우선), C++로 구현, 자체 프런트엔드+자체 IR+LLVM 백엔드, 자체 링커, 장기 셀프 호스팅, `.c7ir`
- `Cmake7109.txt`(독자 DSL), `.cset7`/`.cr7`(TOML), `.mp7`(텍스트), `.l7`(ZIP+SHA-256+다중 타깃), 중앙 레지스트리+로컬+Git+사내, SemVer
- 진단: 영어 기본·한국어 지원·안정 코드, 라이선스 GPL v3.0, 버전 `1.x.y`(메이저 1 유지)

### 7.2 위임 결정 (규격서 부록 D.1~D.7 요약 + 4단계 신규)
| 주제 | 결정 |
|---|---|
| 라이선스 | `GPL-3.0-only` + 표준 라이브러리·런타임에 **C7109 런타임 라이브러리 예외**(GCC RLE 구조, 법률 검토 필요) |
| 구조체 레이아웃 | **선언 순서 유지**(재배치는 `@reorder` 선택) |
| 변수 선언/대입 | `x = expr;`는 **현재 함수에서 보이는 이름이면 대입, 안 보이면 새 불변 변수 선언** |
| `drop` 타입 | 이동 전용(복사 `E0480`), 이름만으로 `return`하는 지역은 이동으로 보아 `drop` 생략 |
| `init`/`drop` | 클래스에서도 기본 `pub`. 생성자 본문은 값을 반환하지 않음 |
| 정수 리터럴 | 문맥 없으면 `i32`(범위 초과 `E0201`), 인덱스는 어떤 정수 타입이든 허용 |
| 오버플로/0 나눗셈/시프트 | Debug 오버플로 트랩·Release 래핑(**둘 다 구현**, §4.2), 0 나눗셈은 항상 panic, 시프트량은 비트 폭으로 마스킹, 부동소수→정수는 포화 변환 |
| `self` | 메서드 안 포인터(`const T*`, mut/init/drop은 `T*`). 필드는 이름만으로도 접근 |
| tagged union | `{태그, 정렬된 페이로드 영역}`, 변형 인덱스는 선언 순서(`Ok`=0, `Err`=1, `Some`=0, `None`=1), 태그 기본은 가장 작은 부호 없는 정수 |
| 제네릭 호출 규약 | `[sret][서술자…][인수]`, `T`는 주소로 전달, 서술자는 모든 인터페이스 vtable 포함 |
| 클로저 | `{코드, 환경}`, 자동 캡처: **불변=값 복사, `mut`=주소**, 값 캡처는 환경 안에 살며 호출 간 유지, `heap`+`free_closure` |
| ABI v1 (잠정) | C7 내부 호출은 LLVM `fastcc`(`kC7CallConv`), 측정으로 유지 결정(§6.6). 13.6절의 레지스터 초안은 **미구현·미비교** |
| 예약어 | 규격서 3.5절 + `out static heap type fallthrough zeroed undefined fn closure operator`. **`heap`, `type`, `out`, `fn` 등은 변수 이름으로 쓸 수 없다** |
| **(4단계 신규, 규격서 미반영)** 표준 라이브러리 | `std` 네임스페이스, **암묵 `std` 프렐류드(최저 우선순위)**: `import` 없이 `Vec`처럼 쓸 수 있고 사용자 선언이 우선. 컴파일마다 `stdlib/*.c7`를 함께 컴파일(`.l7` 패키징은 5단계) |
| **(4단계 신규, 규격서 미반영)** `Ok()` | `void` 페이로드 변형은 인수 없이 생성·패턴 가능 (`Result<void,E>`) |
| **(4단계 신규, 규격서 미반영)** 컨테이너 API | `T`를 집합체에 값으로 담지 않도록 포인터·`out` 매개변수 사용(공유 코드 제네릭의 현재 한계 때문) |
| **(4단계 신규, 규격서 미반영)** 제네릭 타입 인수 경계 | `class C<T : Iface>`의 경계는 타입 표기·생성 호출에서 `E0650`으로 검사 |
| **(4단계 신규, 규격서 미반영)** 디버그 할당자 | `-fdebug-alloc`: 누수 종료코드 70, 이중/잘못된 해제 abort |
| **(4단계 후속)** Debug 검사 | ① Debug는 `--debug`로 켠다(전처리기 `__C7_DEBUG__`와 같은 스위치) — **가정, 소유자 확인 필요**. ② **부호 없는 정수의 오버플로도 트랩**(`0u8 - 1`) — **소유자 확정**. ③ 부정된 리터럴은 상수로 보아 제외. ④ `-fbounds-check`는 별도 옵션이며 `--debug`가 켜 주지 않는다. ⑤ 래핑이 필요하면 128비트 확장 곱 후 `as` |
| **(소유자 결정, 규격서 미반영)** `goto` | ① 변수 선언과 `defer` 문을 건너뛰면 `E0460`. ② 스코프를 빠져나갈 때 `drop`/`defer`를 `return`/`break`처럼 실행 |
| **(4단계 신규, 규격서 미반영)** `c7c test` | `@test` 수집, 테스트별 프로세스 실행, 실패 시 종료코드 1 (§5.4) |

> ⚠ 4단계 신규 결정 6건(+ABI v1, 진단 코드 레지스트리)은 **규격서 부록 D에 아직 기록하지 않았다.** 다음에 규격서를 갱신할 때 D.8로 추가할 것(`Arena`/`HashMap`의 공개 API도 함께).

---

## 8. 알려진 문제·한계·함정

### 8.1 버그/불일치
| 항목 | 내용 | 조치 |
|---|---|---|
| ~~`E0610` 코드 중복~~ | ✅ **해결**: 전처리기의 `#include` 중첩 오류를 `E0613`으로 옮겼다(의미 분석의 `E0610`~`E0612`는 그대로). 이전에는 표의 먼저 나온 항목만 남아 `#include` 중첩 시 `try` 메시지가 출력됐다. `diag_codes.def` 레지스트리 + `static_assert` 중복 검사 + `tests/check_codes.sh`(§9) | 회귀 테스트: `tests/cases/neg_include_depth.c7`(E0613), `tests/sema/neg_try_convert.c7`(E0610) |
| 문서 서식 | 규격서의 "현재 진행" 문장 일부가 굵게 표시됨 | 내용엔 무해 |
| ~~`goto`가 초기화를 건너뜀~~ | ✅ **해결**: `E0460`(§4.1). 이전에는 초기화되지 않은 객체의 `drop`이 실행됐다 | `tests/sema/neg_goto_*.c7`(경우마다 파일 1개), `pos_goto_scope.c7` |
| ~~`goto`로 스코프를 나갈 때 `drop`/`defer` 누락~~ | ✅ **해결**(§4.2) | `tests/run/goto_cleanup.c7` |
| ~~`defer` 문을 건너뛰는 `goto`~~ | ✅ **해결**: `defer`도 `E0460` 대상(소유자 결정). 이전에는 등록되지 않은 `defer`가 스코프 끝에서 실행됐다 | `tests/sema/neg_goto_defer.c7`, `pos_goto_scope.c7`(정상 사용) |

### 8.2 성능·정확성 한계
- 제네릭 코드는 서술자 조회(`c7_find_vtable` 선형 탐색)와 동적 크기 `memcpy`가 있다. 반복문 안의 동적 `alloca`는 스택이 계속 늘어난다. `@inline`/`@specialize`(9단계)로 해소 예정.
- 오버플로 검사의 구멍: 128비트 곱셈, `INT_MIN / -1`(x86 `idiv`가 SIGFPE로 죽음 — panic 메시지 없음), 범위의 `..=` 끝값 +1, 원시 포인터 인덱싱은 검사하지 않는다. 의도적 래핑이 필요한 코드는 128비트로 넓혀 곱한 뒤 `as`로 자른다(`std::hash_mul` 참고). 래핑 연산자/함수는 아직 정해지지 않았다.
- `usize`는 64비트 고정(x86-32는 11단계).
- 의미 분석: 이동 후 사용 추적, 흐름 민감 초기화 검사, `E0460` 없음. 정수 상수 접기는 `+ - * <<`만.
- 인터페이스 포인터(동적 디스패치), 사용자 정의 할당자, `thread_local`은 미구현.

### 8.3 개발 시 반드시 알아둘 함정
1. **`ctxs.back()` 참조를 쥔 채 식을 평가하지 말 것.** 람다가 `ctxs`를 push하면 벡터가 재할당돼 참조가 해제된다(실제로 크래시가 있었고 ASan이 잡았다). 값으로 복사해서 쓴다.
2. **AST를 읽는 하향은 `n->cty`가 있으면 그 타입으로 변환**한다. 새 의미 분석 경로를 만들 때 변환 성공 시 `cty`를 설정하지 않으면 리터럴이 잘못된 폭으로 생성된다.
3. 호출 대상이 지역 변수/전역/자기 필드인 `Path`일 때 `callee->sty`를 의미 분석이 직접 기록해야 한다(`resolvePathHit`는 `expr()`를 거치지 않는다).
4. `tests/run.sh`의 `run` 테스트는 `*.expected`를 **사람이 손으로 검산한 값**으로 고정했다. 출력이 바뀌면 먼저 의도된 변경인지 확인하고 갱신할 것.
5. 새 기능을 만들면 **긍정 테스트 + 부정 테스트(`// expect: E0xxx`)** 를 같이 추가하고 `make asan`도 돌린다.
6. 헤더(`sema_impl.hpp` 등)를 고치면 Makefile 의존성 때문에 전체 재빌드된다(느리지 않음).

---

## 9. 진단 코드 체계

**레지스트리**: 모든 코드는 `compiler/frontend/diag_codes.def`(`DIAG("E0xxx", "영어", "한국어")`)에만 정의한다. 중복은 `diag.cpp`의 `static_assert`로 **컴파일 오류**가 되고, 소스에서 쓰는데 등록되지 않은 코드는 `make test`의 `tests/check_codes.sh`가 실패시킨다(중복이 실제로 빌드를 막는지도 이 스크립트가 확인). **새 진단을 만들면 `.def`에 먼저 추가할 것.** (`.def`를 고치면 Makefile 의존성으로 재빌드된다.)

접두 `E`=오류, `W`=경고, 코드는 **한번 정하면 의미를 바꾸지 않는다**(규격서 13.4절).

| 범위 | 분류 | 코드 |
|---|---|---|
| E00xx | 렉서 | E0001~E0007 |
| E01xx | 파서 | E0101~E0108, E0199(오류 과다) |
| E02xx | 이름·타입 | E0201, E0211~E0217, E0220~E0232 |
| E03xx | 가변성 | E0301~E0303, E0305(미초기화) |
| E04xx | 제어 흐름·값 의미 | E0410, E0412, E0450, E0460, E0470, E0480 |
| E06xx | 전처리기 / try·접근·오버로딩·제약 | 전처리 E0601~E0609·**E0613**, 의미 분석 E0610~E0612·E0621·E0630~E0650 (충돌 해소됨) |
| E09xx | 코드 생성·도구 | E0901(미지원), E0902(IR 검증 실패=내부 오류), E0903(외부 도구 실패) |
| W | 경고 | W0101(PascalCase), W0106(변수 가림), W0601/W0602(전처리) |

---

## 10. 다음 작업 우선순위

1. **4단계 기능은 완료**(§5.3). 선택: §5.2.2의 한계 해소(`Vec<StrBuf>` 등 `drop` 타입 인수, 멤버 호출 명시적 타입 인수). 이후 3~4번(ABI·규격서)로 4단계를 닫는다.
2. ~~`E0610` 코드 충돌 해소 + 진단 코드 레지스트리~~ ✅ 완료(§8.1, §9).
3. ~~**3단계 잔여: ABI v1 벤치마크·확정**~~ ✅ 잠정 완료(§6.6; 13.6 초안과의 비교만 남음) — 13.6절 레지스터 초안 vs `fastcc` vs C ABI를 재귀·작은 구조체·`Result` 반환 프로그램으로 측정. 이후 모든 단계(링커, `.l7`의 `abi` 필드)가 그 위에 올라가므로 **5단계 시작 전**에 하는 것이 안전하다.
4. 규격서 갱신: **D.8 문안은 `docs/spec-D8-draft.md`에 작성됨**(8항목: 4단계 결정 6건 + ABI v1 + 진단 코드 레지스트리). 남은 일은 규격서 Claude Doc에 붙여 넣고 §8.1의 문서 서식(굵은 글씨)을 정리하는 것 — 저장소 밖 작업이라 소유자 몫.
5. 5단계: `Cmake7109.txt` 파서, `.cset7`/`.cr7`(TOML), 증분·병렬 빌드, `.l7`(ZIP+SHA-256), SemVer 해결기 — 이때 `stdlib/*.c7` 소스 동봉을 `.l7` 패키지로 대체.
6. 6단계: 자체 링커(ELF x86-64) — 그 전까지 `gcc`/`llc` 의존.
7. 그 외: 인터페이스 동적 디스패치(팻 포인터), `Str`/컨테이너 확장. (~~Debug 오버플로 검사와 `-fbounds-check`~~ ✅ 완료)

---

## 11. 이어서 개발할 때의 체크리스트

```bash
cd c7109
make clean && make        # 경고 0건이어야 함
make test                 # passed=129 failed=0 이어야 함 (5단계 검증 스크립트 포함)
make asan                 # asan sweep bad=0 이어야 함
```
- 이 세 가지가 통과한 상태에서 §10의 우선순위를 따른다.
- 한 번에 하나씩 바꾸고, 바꿀 때마다 `make test`를 돌린다. 하향을 바꿨다면 `-O0`과 `-O2` 결과가 같은지 확인(러너가 자동으로 한다).
- 규격서와 코드가 충돌하면 **규격서의 [확정] 항목은 바꾸지 않고**, 위임 결정만 근거를 남기고 수정한다(이전에 `acc = acc * 10 + …` 예제와의 충돌로 변수 선언 규칙을 정정한 선례가 있다).

---

## 12. 5단계(빌드 시스템·패키지) 진행 상황

**5단계 전 항목(W1–W16) 완료.** W14(TOML 하드닝+퍼징 `tests/fuzz_pkg.py`, `make fuzz`), W1(검증 연결), W2(`check_driver.sh`), W3(`version.hpp`),
W4(`std.l7`: 2단계로 `stdlib/Cmake7109.txt`를 `c7c pack`이 빌드, `check_std.sh`), W5(`Cmake7109.txt`, `check_buildfile.py`), W6(`.cset7`·옵션, `check_options.py`),
W7(그래프, `check_graph.py`), W8·W10(프로젝트 CLI·병렬 실행기, `check_project.py`), W9(캐시, `check_cache.py`), W11(의존성·해결기, `check_resolve.py`·`check_deps.py`),
W12(`.cr7`, `check_lock.py`), W13(`.mp7`, `check_map.py`), W16(`c7c install`, `check_install.py`).
명령: `c7c build|pack|verify|lock|install|clean`. 모든 검사는 `tests/run.sh`에 연결. `make asan`(퍼징 포함) 이상 없음.
남은 소유자 확인 항목: 가이드 §6 Q1–Q14 (기본 가정으로 구현, 코드의 `⚠ Q번호`와 `docs/stage5-decisions.md` 참고).
결정 기록: `docs/stage5-decisions.md`. 문법: `docs/Cmake7109-grammar.md`.
