# The C7109 Reference

언어·컴파일러의 **규칙**을 정리한 참조 문서다. 입문은 [The C7109 Book](book.md), 예제는 [by Example](by-example.md). 이 문서는 현재 구현(x86-64 Linux)을 기준으로 하며, 구현이 아직 거부하는 것은 `E0901`로 명시한다. 코드 예제는 `tools/doccheck.py`가 검증한다.

## 1. 컴파일러 사용법

```sh
c7c build prog.c7 [more.c7…] -o prog [-O0..3] [--emit=ir|llvm|obj|exe] [--keep] [--no-std]
c7c check prog.c7        # 의미 분석까지 (표준 라이브러리 포함)
c7c test  file.c7 [-O2] [-fdebug-alloc]
c7c parse|lex|pp|ast prog.c7
c7c --version            # c7c 1.0.0-alpha.1 (ABI c7abi1)
```

| 옵션 | 설명 |
|---|---|
| `-Idir`, `-DNAME[=V]` | 전처리기 검색 경로 / 매크로 |
| `-O0..-O3` | 최적화 수준(`llc`) |
| `--debug` | Debug: 정수 오버플로 검사 켬 |
| `-foverflow-check` / `-fno-overflow-check` | 오버플로 검사 명시(`--debug`보다 우선) |
| `-fbounds-check` | 배열·슬라이스·`Str` 인덱싱/슬라이싱 경계 검사 |
| `-fdebug-alloc` | 추적 할당자(누수 종료 코드 70) |
| `--link=static\|dynamic` | `static`은 `gcc -static` |
| `--lib x.l7` | 패키지를 소스로 추가(검증 후) |
| `--depfile f` | 읽은 소스·헤더 목록 출력 |
| `--no-std` | 암묵 표준 라이브러리 끔 |
| `--lang=en\|ko`, `--diag-format=text\|json` | 진단 언어 / 형식 |
| `--target=ARCH-OS`, `-ffreestanding` | 대상(지금은 `x86_64-linux`만 코드 생성) |

환경 변수: `C7_STDLIB`(표준 라이브러리 소스 디렉터리), `C7_RUNTIME`(런타임 C 파일). 표준 라이브러리 탐색 순서는 `C7_STDLIB` > `std.l7` > `stdlib/`(실행 파일 옆).

파이프라인: 전처리 → 렉서 → 파서(AST) → 의미 분석 → C7 IR → LLVM IR(텍스트) → `llc` → `gcc` 링크(+`runtime/c7rt.c`). 여러 입력 파일은 **하나의 번역 단위 묶음**으로 분석되며 `.h7`·분리 컴파일은 없다.

## 2. 어휘

- 소스는 UTF-8이어야 한다(`E0006`). 줄 주석 `//`, 블록 주석 `/* */`(`E0001`).
- 식별자 규칙은 C와 같다. 사용자 타입은 PascalCase, 기본 타입은 소문자 관례.
- **예약어**: 규격서 3.5절의 키워드와 함께 `out static heap type fallthrough zeroed undefined fn closure operator`. `heap`, `type`, `out`, `fn` 등은 변수 이름으로 쓸 수 없다.
- 리터럴: 정수(`42`, `0x2A`, `0b101`, 구분자 `_`), 실수(`1.5`), 문자 `'a'`(`Char`), 문자열 `"…"`(`Str`), raw `r"…"`, C 문자열 `c"…"`(`CStr`). 이스케이프 `\n \t \\ \" …`(`E0004`).
- `>>`는 일부러 토큰화하지 않고 파서가 인접한 `>`로 합성한다(중첩 제네릭 `A<B<C>>`).

## 3. 타입

| 분류 | 타입 |
|---|---|
| 정수 | `i8 i16 i32 i64 i128`, `u8 u16 u32 u64 u128`, `isize usize` |
| 부동소수 | `f32 f64` |
| 기타 | `bool`, `Char`(유니코드 스칼라 값), `void` |
| 문자열 | `Str`(UTF-8 뷰), `StrBuf`, `CStr`(C 문자열) |
| 포인터 | `T*`, `const T*` |
| 배열/슬라이스 | `T[N]`, `T[]`(포인터+길이), `const T[]` |
| 튜플 | `(A, B, …)`, 요소 접근 `t.0` |
| 사용자 | `struct`, `union`, `enum`, `tagged union`, `class`, `interface` |
| 함수 | `closure(A, B) -> R`(클로저), 함수 포인터 |
| 표준 | `Option<T>`, `Result<T, E>`, `Vec<T>`, `HashMap<K, V>` … |

`int = i32`, `long = i64`(플랫폼 독립). 변수는 기본 불변이고 `mut`으로 가변이 된다(`E0301`). `const` 포인터를 통한 변경은 `E0302`.

### 3.1 변환

- 암묵 변환은 값이 **보존되는** 경우만(예: 더 넓은 정수로, 리터럴이 범위 안일 때). 그 밖은 `as`가 필요하다(`E0221`).
- `as`는 손실 변환을 허용한다: 정수 폭 변경, 정수↔부동소수, 포인터↔정수/포인터. 부동소수→정수는 포화한다. 유효하지 않으면 `E0226`.
- 정수 리터럴은 문맥이 없으면 `i32`(범위 초과 `E0201`). 인덱스는 어떤 정수 타입이든 된다.

### 3.2 정수 연산

| 상황 | Debug(`--debug`) | Release |
|---|---|---|
| `+ - *`, 단항 `-` 오버플로 | `panic`(134) | 래핑 |
| 0으로 나누기 | `panic` | `panic` |
| 시프트량 | 비트 폭으로 마스킹 | 같음 |
| 128비트 `*`·`<<`·`MIN / -1` | 검사 없음 | 래핑 |

의도한 래핑 곱은 128비트로 넓혀 곱한 뒤 `as`로 줄이면 Debug에서도 안전하다(`std::hash_mul`).

## 4. 선언

### 4.1 변수와 대입

```c7 check
i32 main() {
    i32 a = 1;            // 불변
    mut i32 b = 2;        // 가변
    b = a + b;
    c = 5;                // 보이는 이름이 아니므로 새 불변 변수 선언
    mut i32* p = null;
    return 0;
}
```

`x = expr;`는 **현재 함수에서 보이는 이름이면 대입, 안 보이면 새 불변 변수 선언**이다. 선언에는 초기값이 필요하다(`E0305`, 초기화를 미루려면 `undefined`). 대입은 문장이며 식이 아니다(`E0102`).

### 4.2 함수

반환 타입, 이름, 매개변수 순서. 매개변수는 `out`을 붙여 출력용으로 쓸 수 있고, 호출 쪽도 `out x`로 표기한다(`E0231`). 오버로딩이 허용되며 모호하면 `E0630`, 맞는 것이 없으면 `E0631`. 값을 돌려주는 함수는 모든 경로에서 반환해야 한다(`E0410`). 속성: `@noreturn`, `@test`.

### 4.3 struct, union, enum

- `struct`: 필드는 **선언 순서대로** 배치된다(재배치 속성은 아직 없다).
- `union`: 필드가 같은 저장소를 공유.
- `enum Name : 기반타입 { A, B = 5, C }`: 정수 열거형. 값은 `Name::A`.
- `tagged union`: `Name { Variant(필드…), Other, }`. 태그는 선언 순서대로 0부터이고 기본 태그 타입은 가장 작은 부호 없는 정수(`: u8`로 지정 가능). 생성은 `Name::Variant(…)`.

### 4.4 class

```c7 check
class Account {
    priv mut i64 balance;
    pub init(i64 start) { balance = start; }
    pub drop() { }
    pub mut void deposit(i64 n) { balance += n; }
    pub i64 get() { return balance; }
}
```

- 상속·virtual 없음(문법적 설탕). 다형성은 인터페이스로.
- 접근 제어 `pub`/`priv`(`E0621`). `init`/`drop`은 기본 `pub`.
- 메서드 안에서 `self`는 포인터(`const T*`, `mut` 메서드·`init`·`drop`은 `T*`). 필드는 이름만으로 접근한다. `const` 메서드에서 필드를 바꾸면 `E0303`.
- **`drop` 타입은 이동 전용**: 복사는 `E0480`. 이름만으로 `return`하는 지역은 이동으로 보아 `drop`을 생략한다. 의도적인 비트 복사는 `bitcopy`.
- 생성: `Account(100)`, 힙: `alloc Account(100)` / `free p`. 소멸은 스코프 종료·`return`·`break`·`goto`에서 **역순**.

### 4.5 interface와 impl

```c7 check
interface Shape { f64 area(); }

struct Square { f64 side; }
impl Shape for Square {
    f64 area() { return self->side * self->side; }
}
```

인터페이스를 구현할 때 모든 메서드가 있어야 한다(`E0640`). 인터페이스가 아닌 것을 `impl … for`에 쓰면 `E0641`. `impl Type { … }`로 기존 타입(`Str` 포함)에 메서드를 붙일 수 있다. 인터페이스 **동적 디스패치**(`Shape*`)는 아직 없다(`E0901`).

### 4.6 제네릭

`Name<T>` 공유 코드 제네릭. 함수·클래스·메서드 수준 모두 가능. 경계는 `T : Iface`이고 위반하면 `E0650`(타입 표기·생성 호출에서도 검사). 호출에서 `T`는 **인수에서만** 추론한다(반환 타입만으로는 추론하지 않음). 현재 `E0901`인 것:

- `T`를 값으로 포함하는 집합체(`(T, i32)`, `Option<T>`, `T` 필드)
- `T`에 대한 연산자, `Self`를 값으로 받거나 돌려주는 인터페이스 메서드, 정적 인터페이스 메서드
- `drop` 타입을 타입 인수로 사용, 제네릭 안의 클로저, 멤버 호출의 명시적 타입 인수(`a.f<u8>()`)

## 5. 문장과 식

| 문장 | 비고 |
|---|---|
| `if / else`, `while`, `do … while` | 조건은 `bool`(`E0224`) |
| `for (i in a..b)`, `a..=b` | 범위(끝 제외/포함) |
| `for (x in 배열/슬라이스/이터레이터)` | 이터레이터는 `next()`가 `Option<T>`를 돌려주는 타입(`E0232`) |
| `for (init; cond; step)` | C식 |
| `switch (x) { case 1, 2: …; default: … }` | |
| `match (x) { 패턴 => 문장; … }` | **완전해야 함**(`E0450`). 정수 범위 `1..=9`, `_`, 열거형·변형 패턴, `Ok(v)`/`Err(e)`, `Some(v)`/`None` |
| `break`, `continue`, `return` | 반복문 밖이면 `E0229` / 잘못된 `return`은 `E0412` |
| 라벨 · `goto` | 정의 없으면 `E0230`. 변수 선언을 **건너뛰는** 점프는 `E0460` |
| `defer 문;` | 스코프 종료 시 역순 실행. `defer` 안에서 `return` 등은 `E0470` |
| `try expr` | `Result`가 `Err`이면 즉시 반환. 함수가 `Result`를 돌려줘야 함(`E0611`), 피연산자도 `Result`(`E0612`), 오류 타입 변환 불가는 `E0610` |
| `alloc T`, `alloc T[n]`, `free p`, `free[] p` | 실패 시 `null`. `zeroed`, 생성자 인수 가능 |
| `sizeof(T)`, `alignof(T)` | |
| `cond ? a : b` | |

연산자 우선순위와 결합은 C와 같다. 연산자 오버로딩은 `bool operator==(Str a, Str b)`처럼 자유 함수 또는 멤버로 정의한다(`T`에 대한 제네릭 연산자는 아직 없음).

### 5.1 클로저

`closure(A, B) -> R`. 람다는 `(A a, B b) -> R { … }`. 캡처: 자동(불변 = 값 복사, `mut` = 주소), 명시 `[x, &y]`. `heap` 접두사는 힙 환경이며 `free_closure(c)`로 해제한다. 캡처 없는 클로저→함수 포인터 변환은 아직 `E0901`.

### 5.2 `goto`와 정리

`goto`로 스코프를 빠져나가면 빠져나가는 스코프의 `drop`/`defer`가 **역순**으로 실행된다. 같은 스코프에서 뒤로 점프하면 라벨 이후에 선언된 변수가 소멸된다.

## 6. 모듈과 전처리기

- `namespace a::b;`와 `import`로 이름을 나눈다. 이름 해석 순서: 현재 네임스페이스(바깥으로 확장) → 전체 이름 → `import` 별칭 → 암묵 `std`(가장 낮은 우선순위, 사용자 선언이 우선).
- 전처리기는 C 호환이다: `#define`(객체/함수 매크로, `#`, `##`, `__VA_ARGS__`, `__VA_OPT__`), `#if/#ifdef/#elif/#else/#endif`, `#include`, `#pragma once`, `__has_include`, `#error`. `#embed`는 아직 없다(`E0609`).
- 미리 정의된 매크로와 `-DC7_TEST`(테스트 빌드)가 있다.

## 7. `extern "C"`와 ABI

```c7 run
extern "C" {
    void c7_print_i64(i64 v);
    void c7_newline();
}

i32 main() {
    c7_print_i64(42);
    c7_newline();
    return 0;
}
```

```text
42
```

- `extern "C"` 함수와 `main`은 C 호출 규약이고 심볼 장식이 없다.
- C7 내부 호출 규약은 LLVM `fastcc`(ABI `c7abi1`, 잠정). 바꿀 수 있는 지점은 `compiler/codegen/lower.cpp`의 `kC7CallConv` 한 곳이다.
- C7 심볼 이름은 `_C7$경로$이름$$매개변수타입…`이다(람다는 `_C7$lambda$N`).
- 전역 변수는 상수 초기화만 가능하다. `thread_local`과 사용자 정의 할당자 `alloc(a)`는 아직 없다.

### 7.1 메모리 레이아웃 요약

`bool`=`i1`, `Char`=`i32`, `Str`/슬라이스=`{ptr, len}`, 구조체=선언 순서, tagged union=`{태그, 정렬된 페이로드}`, 클로저=`{코드, 환경}`(2워드).

## 8. 진단 코드

모든 진단은 `compiler/frontend/diag_codes.def`에 영어·한국어 메시지와 함께 등록돼 있고, 한번 정한 코드의 의미는 바뀌지 않는다. `--lang=ko`, `--diag-format=json`.

| 범위 | 코드 |
|---|---|
| 어휘 | E0001 닫히지 않은 블록 주석 · E0002 닫히지 않은 문자열/문자 · E0003 예상 밖 문자 · E0004 잘못된 이스케이프 · E0005 잘못된 수 리터럴 · E0006 UTF-8 아님 · E0007 빈 문자 리터럴 |
| 구문 | E0101 기대한 토큰 · E0102 대입은 문장 · E0103 타입 기대 · E0104 식 기대 · E0105 식별자 기대 · E0107 선언 기대 · E0108 패턴 기대 · E0199 오류 과다 |
| 이름·타입 | E0201 리터럴 범위 · E0211 중복 정의 · E0212 이름 없음 · E0213 타입 없음 · E0214 멤버 없음 · E0215/E0217 초기화 필드 수·누락 · E0220 타입 불일치 · E0221 암묵 변환 불가 · E0222 인수 개수 · E0223 호출 불가 · E0224 조건은 bool · E0225 잘못된 피연산자 · E0226 잘못된 캐스트 · E0227 역참조 불가 · E0228 인덱싱 불가 · E0229 반복문 밖 · E0230 라벨 없음 · E0231 `out` 불일치 · E0232 반복 불가 |
| 가변성 | E0301 불변 대입 · E0302 const 폐기 · E0303 const self 수정 · E0305 초기화 필요 |
| 흐름 | E0410 반환 경로 · E0412 잘못된 return · E0450 match 불완전 · E0460 goto 변수 범위 · E0470 defer 안의 금지 문 · E0480 drop 타입 복사 |
| 전처리기 | E0601 `#error` · E0602 모르는 지시어 · E0603 include 없음 · E0604 닫히지 않은 `#if` · E0605 짝 없는 `#endif` · E0606 잘못된 지시어 · E0607 매크로 인수 개수 · E0608 `#if` 식 · E0609 미지원 · E0613 include 중첩 · E0614 `##` 결과 |
| 시맨틱 2 | E0610 try 오류 변환 · E0611 try 위치 · E0612 try 피연산자 · E0621 비공개 멤버 · E0630 모호한 호출 · E0631 맞는 오버로드 없음 · E0640 인터페이스 미구현 · E0641 인터페이스 아님 · E0650 제약 위반 |
| 코드 생성 | E0901 아직 미지원 · E0902 내부 오류(IR 검증) · E0903 외부 도구 실패 |
| 빌드·패키지 | E0701–E0720, W0701 → [빌드 시스템 문서](build-system.md#10-진단-코드빌드) |

미지원 기능은 **조용히 틀린 코드를 만들지 않고** `E0901`로 명확히 거부한다.

```c7 error E0450
enum Color : u8 { Red, Green }
i32 f(Color c) {
    match (c) {
        Red => return 1;
    }
}
```

```c7 error E0480
class G { pub init() { } pub drop() { } }
i32 main() {
    G a = G();
    G b = a;
    return 0;
}
```

```c7 error E0221
i32 main() {
    i64 big = 5;
    i32 small = big;
    return 0;
}
```

```c7 error E0410
i32 f(bool c) {
    if (c) { return 1; }
}
```

## 9. 로드맵과 한계

- 완료: 프런트엔드, 의미 분석, C7 IR + LLVM 코드 생성(x86-64 Linux), 런타임·표준 라이브러리 기초, 빌드 시스템·패키지(5단계).
- 예정: 자체 링커(6), Windows(7), 표준 라이브러리 확장(8), 최적화·LTO(9), AArch64(10)·x86-32(11), 1.0 안정화(12), 셀프 호스팅(13).
- 알려진 한계: 이동 후 사용 추적 없음, 흐름 민감한 초기화 검사 없음, 정수 리터럴 상수 접기는 `+ - * <<`만. 자세한 목록은 `STATUS.md` §7·§4.
