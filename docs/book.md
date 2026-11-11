# The C7109 Book

C7109를 처음 배우는 사람을 위한 안내서다. 이 책의 코드 예제는 `tools/doccheck.py`가 **실제 컴파일러로 전부 컴파일·실행해 출력까지 대조**한다(`make doccheck`). 문서와 컴파일러가 어긋나면 검사가 실패한다.

관련 문서: [C7109 by Example](by-example.md) · [표준 라이브러리 API](stdlib.md) · [Cmake7109.txt 빌드 시스템](build-system.md) · [The C7109 Reference](reference.md)

> 현재 상태: 대상은 **x86-64 Linux**와 **x86-64 Windows**(크로스 컴파일, [Windows 대상](windows.md)). 컴파일러(`c7c`)는 LLVM의 `llc`와 `gcc`(링크용, Windows 대상은 `clang`·`lld`)가 필요하다. 자체 링커(6단계)는 아직 없다.

---

## 1. 시작하기

```sh
apt-get install -y llvm gcc g++ make python3
make            # ./c7c 와 ./std.l7 을 만든다
make test       # 전체 테스트
```

`c7c`는 **실행 파일 옆**의 `runtime/c7rt.c`와 `std.l7`(또는 `stdlib/`)을 찾는다. 설치 없이 쓰려면 프로젝트 루트에서 `./c7c`로 실행한다.

### 1.1 Hello, world

```c7 run
i32 main() {
    println("Hello, C7109!");
    return 0;
}
```

```text
Hello, C7109!
```

```sh
./c7c build hello.c7 -o hello
./hello
```

- `main`은 `i32`를 돌려준다. 반환값이 프로그램의 종료 코드다.
- 표준 라이브러리(`std`)는 `import` 없이 바로 쓴다. `println`은 `Str`을 줄바꿈과 함께 출력한다.
- 문장은 세미콜론으로 끝낸다.

## 2. 변수와 기본 타입

변수는 기본이 **불변**이다. 바꾸려면 `mut`을 붙인다. 타입은 왼쪽에 쓴다.

```c7 run
i32 main() {
    i32 a = 7;            // 불변
    mut i32 b = a * 2;    // 가변
    b += 1;
    print_i64(b as i64);
    newline();
    return 0;
}
```

```text
15
```

```c7 error E0301
i32 main() {
    i32 a = 1;
    a = 2;
    return 0;
}
```

기본 타입: `i8 i16 i32 i64 i128`, `u8 u16 u32 u64 u128`, `isize usize`, `f32 f64`, `bool`, `Char`. `int`는 `i32`, `long`은 `i64`로 플랫폼과 무관하다.

- 정수 리터럴은 문맥이 없으면 `i32`다. 범위를 넘으면 오류.
- 값이 줄어들 수 있는 변환은 반드시 `as`로 쓴다: `x as i64`, `n as u8`.
- Debug 빌드(`--debug`)는 정수 `+ - *` 오버플로를 검사해 `panic`한다. Release는 값이 감싸진다(wrapping). 0으로 나누기는 항상 `panic`.

## 3. 제어 흐름

```c7 run
i32 main() {
    for (i in 0..3) { print_i64(i as i64); print(" "); }   // 0 1 2
    newline();
    for (i in 0..=2) { print_i64(i as i64); }              // 0 1 2 (끝 포함)
    newline();
    mut i32 k = 0;
    while (k < 3) { k += 1; }
    print_i64(k as i64);
    newline();
    return 0;
}
```

```text
0 1 2 
012
3
```

`if`/`else`, `while`, `do … while`, `for (x in 범위/배열/이터레이터)`, C식 `for`, `switch`, `match`, `break`/`continue`, 라벨과 `goto`가 있다.

`match`는 **빠짐없이** 다뤄야 한다(`E0450`). 정수에는 범위 패턴과 `_`를 쓴다.

```c7 run
i32 classify(i32 n) {
    match (n) {
        0 => return 100;
        1..=9 => { return 200; }
        _ => return 300;
    }
}

i32 main() {
    print_i64(classify(0) as i64); newline();
    print_i64(classify(5) as i64); newline();
    print_i64(classify(50) as i64); newline();
    return 0;
}
```

```text
100
200
300
```

## 4. 함수

반환 타입을 이름 앞에 쓴다. 여러 값을 돌려줄 때는 **튜플**이나 `out` 매개변수를 쓴다.

```c7 run
(i32, bool) divide(i32 a, i32 b) {
    if (b == 0) { return (0, false); }
    return (a / b, true);
}

bool twice(i32 a, out i32 result) { result = a * 2; return true; }

i32 main() {
    (i32, bool) r = divide(10, 2);
    print_i64(r.0 as i64); newline();
    mut i32 t = 0;
    twice(21, out t);
    print_i64(t as i64); newline();
    return 0;
}
```

```text
5
42
```

같은 이름에 매개변수가 다른 함수를 여럿 둘 수 있다(오버로딩). 모호하면 `E0630`/`E0631`.

## 5. 구조체, 열거형, tagged union

```c7 run
struct Point { i32 x; i32 y; }

enum Color : u8 { Red = 1, Green, Blue }

tagged union Shape { Circle(f64 radius), Rect(f64 w, f64 h), Empty, }

i32 manhattan(Point p) { return p.x + p.y; }

f64 area(Shape s) {
    match (s) {
        Circle(r) => return 3.0 * r * r;
        Rect(w, h) => return w * h;
        Empty => return 0.0;
    }
}

i32 main() {
    Point p = Point{ x: 3, y: 4 };
    print_i64(manhattan(p) as i64); newline();
    print_i64(Color::Blue as i64); newline();
    print_i64(area(Shape::Rect(3.0, 4.0)) as i64); newline();
    return 0;
}
```

```text
7
3
12
```

구조체 필드는 **선언 순서대로** 배치된다. tagged union의 변형은 선언 순서대로 0, 1, 2…이다.

## 6. 포인터, 배열, 슬라이스, 문자열

- `T*`는 원시 포인터다. 대여 검사기는 없다.
- `T[N]`은 배열, `T[]`는 슬라이스(포인터+길이)다.
- `Str`은 UTF-8 `(ptr, len)` 뷰이고, `StrBuf`는 늘어나는 소유 버퍼, `CStr`은 C 문자열이다.

```c7 run
i32 sum(const i32[] xs) {
    mut i32 t = 0;
    for (x in xs) { t += x; }
    return t;
}

void swap(i32* a, i32* b) { i32 t = *a; *a = *b; *b = t; }

i32 main() {
    i32[4] arr = {3, 9, 4, 1};
    print_i64(sum(arr) as i64); newline();
    mut i32 a = 1;
    mut i32 b = 2;
    swap(&a, &b);
    print_i64(a as i64); print(" "); print_i64(b as i64); newline();
    Str s = "héllo";
    print_u64(s.len as u64); newline();   // 바이트 수
    return 0;
}
```

```text
17
2 1
6
```

배열 경계 검사는 기본이 꺼져 있다. `-fbounds-check`로 켠다.

## 7. 메모리와 클래스

힙 메모리는 직접 관리한다: `alloc T`, `alloc T[n]`, `free p`, `free[] p`. 실패하면 `null`이다. 정리는 **`defer`** 와 **클래스의 `drop`(RAII)** 로 자동화한다.

```c7 run
class Guard {
    priv i32 id;
    pub init(i32 n) { id = n; }
    pub drop() { print("drop "); print_i64(id as i64); newline(); }
}

i32 main() {
    Guard a = Guard(1);
    defer println("deferred");
    Guard b = Guard(2);
    println("body");
    return 0;
}
```

```text
body
drop 2
deferred
drop 1
```

스코프를 나갈 때(`return`, `break`, `goto`, 오류 경로 포함) **등록의 역순**으로 `defer`와 `drop`이 실행된다. `drop`이 있는 타입은 **이동 전용**이라 복사하면 `E0480`이다. 클래스에는 상속이 없다(문법적 설탕).

```c7 run
i32 main() {
    mut i32* p = alloc i32;
    if (p == null) { return 1; }
    *p = 41;
    *p += 1;
    print_i64(*p as i64); newline();
    free p;
    return 0;
}
```

```text
42
```

## 8. 오류 처리: `Result`와 `try`

```c7 run
enum ParseError : u8 { Empty, BadDigit }

Result<i32, ParseError> parse(Str s) {
    if (s.len == 0) { return Err(ParseError::Empty); }
    mut i32 acc = 0;
    for (i in 0..s.len) {
        u8 c = s[i];
        if (c < 48 || c > 57) { return Err(ParseError::BadDigit); }
        acc = acc * 10 + (c as i32 - 48);
    }
    return Ok(acc);
}

Result<i32, ParseError> sum_two(Str a, Str b) {
    i32 x = try parse(a);     // 오류면 즉시 그대로 반환
    i32 y = try parse(b);
    return Ok(x + y);
}

i32 main() {
    match (sum_two("40", "2")) {
        Ok(v) => { print_i64(v as i64); newline(); }
        Err(e) => { println("error"); }
    }
    match (sum_two("4", "x")) {
        Ok(v) => { print_i64(v as i64); newline(); }
        Err(e) => { println("error"); }
    }
    return 0;
}
```

```text
42
error
```

`Option<T>`(`Some`/`None`)도 같은 방식이다. 값 없는 성공은 `Result<void, E>`와 `Ok()`로 쓴다.

## 9. 제네릭

`Name<T>` 형태이며 **공유 코드**로 컴파일된다(타입마다 복사하지 않는다). 제약은 인터페이스로 건다.

```c7 run
interface Comparable { i32 compare(const Self* other); }

impl Comparable for i32 {
    i32 compare(const i32* other) { return *self - *other; }
}

T max<T : Comparable>(T a, T b) { return a.compare(&b) >= 0 ? a : b; }
T id<T>(T x) { return x; }

i32 main() {
    print_i64(max(3, 5) as i64); newline();
    print_i64(id(42) as i64); newline();
    return 0;
}
```

```text
5
42
```

현재 한계(`E0901`로 명확히 거부): `T`를 값으로 포함하는 집합체, `T`에 대한 연산자, 인터페이스 동적 디스패치. 컨테이너는 `T*`와 `out`으로 설계한다.

## 10. 클로저

```c7 run
i32 apply(closure(i32) -> i32 f, i32 v) { return f(v); }

closure(i32) -> i32 make_adder(i32 n) {
    return heap [n](i32 x) -> i32 { return x + n; };
}

i32 main() {
    i32 base = 10;
    closure(i32) -> i32 addb = (i32 x) -> i32 { return x + base; };
    print_i64(apply(addb, 7) as i64); newline();
    closure(i32) -> i32 add5 = make_adder(5);
    print_i64(add5(1) as i64); newline();
    free_closure(add5);
    return 0;
}
```

```text
17
6
```

자동 캡처는 불변 변수를 **값 복사**, `mut` 변수를 **주소**로 잡는다. `[n]`은 명시 캡처. `heap` 클로저는 함수 밖으로 나갈 수 있고 `free_closure`로 해제한다.

## 11. 표준 라이브러리 맛보기

`Vec<T>`, `HashMap<K, V>`, `Arena`, `StrBuf`가 `import` 없이 보인다. 자세한 내용은 [표준 라이브러리 API](stdlib.md).

```c7 run
i32 main() {
    mut Vec<i32> v = Vec<i32>();
    for (i in 0..5) { v.push(i * i); }
    print_u64(v.size() as u64); newline();
    print_i64(*v.at(4) as i64); newline();
    return 0;
}
```

```text
5
16
```

## 12. 전처리기와 모듈

C 호환 전처리기(`#define`, `#if`, `#include`, `#pragma once`, `#`/`##`/`__VA_OPT__`)를 쓴다. `namespace`와 `import`로 이름을 나눈다.

```c7 run
#define SQUARE(x) ((x) * (x))
#define VERSION 3

i32 main() {
    print_i64(SQUARE(VERSION + 1) as i64); newline();
    return 0;
}
```

```text
16
```

## 13. 테스트

`@test` 함수를 모아 **테스트마다 별도 프로세스**로 실행한다. `assert`가 실패하면 그 테스트만 실패한다.

```c7 test
@test
void arithmetic() { assert(2 + 2 == 4, "math works"); }

@test
void vec_grows() {
    mut Vec<i32> v = Vec<i32>();
    for (i in 0..100) { v.push(i); }
    assert(v.size() == 100, "size");
}
```

```sh
./c7c test file.c7 [-O2] [-fdebug-alloc]
```

`-fdebug-alloc`는 누수(종료 코드 70)와 이중 해제를 잡는 추적 할당자를 켠다.

## 14. 진단

오류에는 안정적인 코드가 붙는다(`E0303` 등). 한국어 메시지는 `--lang=ko`, 기계 판독은 `--diag-format=json`. 코드 전체 목록은 [Reference](reference.md#진단-코드)에 있다.

## 15. 프로젝트 빌드

여러 파일·의존성·라이브러리를 가진 프로젝트는 `Cmake7109.txt`로 빌드한다. 단일 파일 모드는 그대로 유지된다. → [Cmake7109.txt 빌드 시스템](build-system.md)

## 16. 다음으로

- 따라 하며 배우기: [C7109 by Example](by-example.md)
- 모든 문법·규칙: [The C7109 Reference](reference.md)
- 알려진 한계와 로드맵: `STATUS.md`
