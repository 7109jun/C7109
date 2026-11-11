# C7109 by Example

짧고 실행 가능한 예제 모음. 모든 `c7 run` 블록은 `tools/doccheck.py`가 컴파일·실행해 출력을 대조한다. 개념 설명은 [The C7109 Book](book.md), 규칙은 [Reference](reference.md).

## 1. Hello, world

```c7 run
i32 main() {
    println("Hello, C7109!");
    return 0;
}
```

```text
Hello, C7109!
```

## 2. 출력 함수

```c7 run
i32 main() {
    print("n = "); print_i64(-5); newline();
    print("u = "); print_u64(7); newline();
    print("f = "); print_f64(2.5); newline();
    print("b = "); print_bool(true); newline();
    print_char('한'); newline();
    return 0;
}
```

```text
n = -5
u = 7
f = 2.5
b = true
한
```

## 3. 피보나치 (재귀)

```c7 run
i64 fib(i64 n) {
    if (n < 2) { return n; }
    return fib(n - 1) + fib(n - 2);
}

i32 main() {
    for (i in 0..10) { print_i64(fib(i as i64)); print(" "); }
    newline();
    return 0;
}
```

```text
0 1 1 2 3 5 8 13 21 34 
```

## 4. 배열 뒤집기 (포인터와 슬라이스)

```c7 run
void reverse(i32* a, usize n) {
    mut usize i = 0;
    mut usize j = n - 1;
    while (i < j) {
        i32 t = a[i]; a[i] = a[j]; a[j] = t;
        i += 1; j -= 1;
    }
}

i32 main() {
    mut i32[5] xs = {1, 2, 3, 4, 5};
    reverse(&xs[0], 5);
    for (x in xs) { print_i64(x as i64); print(" "); }
    newline();
    return 0;
}
```

```text
5 4 3 2 1 
```

## 5. 문자열 다루기 (`Str`, `StrBuf`, UTF-8)

```c7 run
i32 main() {
    mut StrBuf sb = StrBuf();
    sb.append("héllo");
    sb.push_char(' ');
    sb.push_char('한');
    println(sb.as_str());
    print_u64(sb.size() as u64); newline();       // 바이트 수

    mut usize chars = 0;
    for (ch in sb.as_str().chars()) { chars += 1; }
    print_u64(chars as u64); newline();           // 문자 수

    Str a = "abc";
    print_bool(a == "abc"); newline();
    print_bool(a.starts_with("ab")); newline();
    println(a.slice(1, 3));
    return 0;
}
```

```text
héllo 한
10
7
true
true
bc
```

## 6. 오류 처리

```c7 run
enum MathError : u8 { DivideByZero }

Result<i32, MathError> div(i32 a, i32 b) {
    if (b == 0) { return Err(MathError::DivideByZero); }
    return Ok(a / b);
}

Result<i32, MathError> calc(i32 a, i32 b, i32 c) {
    i32 x = try div(a, b);
    i32 y = try div(x, c);
    return Ok(y);
}

void show(Result<i32, MathError> r) {
    match (r) {
        Ok(v) => { print("ok "); print_i64(v as i64); newline(); }
        Err(e) => { println("division by zero"); }
    }
}

i32 main() {
    show(calc(100, 5, 2));
    show(calc(100, 0, 2));
    return 0;
}
```

```text
ok 10
division by zero
```

## 7. `Option`

```c7 run
Option<i32> find(i32[4] xs, i32 target) {
    for (i in 0..4) { if (xs[i] == target) { return Some(i); } }
    return None;
}

i32 main() {
    i32[4] xs = {5, 6, 7, 8};
    match (find(xs, 7)) { Some(i) => print_i64(i as i64); None => print("none"); }
    newline();
    match (find(xs, 9)) { Some(i) => print_i64(i as i64); None => print("none"); }
    newline();
    return 0;
}
```

```text
2
none
```

## 8. 클래스와 RAII

```c7 run
class Counter {
    priv mut i32 hits;
    pub init() { hits = 0; }
    pub drop() { print("final: "); print_i64(hits as i64); newline(); }
    pub mut void bump() { hits += 1; }
    pub i32 get() { return hits; }
}

i32 main() {
    mut Counter c = Counter();
    c.bump(); c.bump(); c.bump();
    print_i64(c.get() as i64); newline();
    return 0;
}
```

```text
3
final: 3
```

## 9. `defer`와 오류 경로 정리

```c7 run
enum E : u8 { Boom }

Result<i32, E> maybe(bool fail) {
    if (fail) { return Err(E::Boom); }
    return Ok(7);
}

Result<i32, E> work(bool fail) {
    defer println("cleanup");
    i32 v = try maybe(fail);
    return Ok(v + 1);
}

i32 main() {
    match (work(false)) { Ok(v) => print_i64(v as i64); Err(e) => print("failed"); }
    newline();
    match (work(true)) { Ok(v) => print_i64(v as i64); Err(e) => print("failed"); }
    newline();
    return 0;
}
```

```text
cleanup
8
cleanup
failed
```

## 10. `Vec`와 힙

```c7 run
struct Point { i32 x; i32 y; }

i32 main() {
    mut Vec<Point> pts = Vec<Point>();
    for (i in 0..20) { pts.push(Point{ x: i, y: i * 2 }); }
    print_u64(pts.size() as u64); newline();
    print_i64(pts.at(19)->y as i64); newline();

    mut Vec<i32>* hv = alloc Vec<i32>();
    hv->push(10); hv->push(20);
    print_i64(*hv->at(1) as i64); newline();
    free hv;
    return 0;
}
```

```text
20
38
20
```

## 11. `HashMap`

```c7 run
i32 main() {
    mut HashMap<i32, i64> m = HashMap<i32, i64>();
    for (i in 0..100) { m.put(i, (i as i64) * 7); }
    print_u64(m.size() as u64); newline();
    i32 k = 42;
    i64* p = m.get(&k);
    if (p != null) { print_i64(*p); newline(); }
    i32 missing = 500;
    print_bool(m.contains(&missing)); newline();
    print_bool(m.remove(&k)); newline();
    print_u64(m.size() as u64); newline();
    return 0;
}
```

```text
100
294
false
true
99
```

## 12. 제네릭과 인터페이스

```c7 run
interface Comparable { i32 compare(const Self* other); }

struct Point { i32 x; i32 y; }
impl Comparable for Point {
    i32 compare(const Point* other) { return (self->x + self->y) - (other->x + other->y); }
}

T max<T : Comparable>(T a, T b) { return a.compare(&b) >= 0 ? a : b; }

void swap<T>(T* a, T* b) { T t = *a; *a = *b; *b = t; }

i32 main() {
    Point m = max(Point{ x: 1, y: 2 }, Point{ x: 3, y: 4 });
    print_i64(m.x as i64); newline();
    mut i32 a = 1;
    mut i32 b = 2;
    swap(&a, &b);
    print_i64(a as i64); newline();
    return 0;
}
```

```text
3
2
```

## 13. 클로저

```c7 run
void count_to(i32 limit, closure(i32) -> void visit) {
    for (i in 0..limit) { visit(i); }
}

i32 main() {
    mut i32 total = 0;
    closure(i32) -> void acc = (i32 x) -> void { total += x; };
    count_to(5, acc);
    print_i64(total as i64); newline();     // 0+1+2+3+4
    return 0;
}
```

```text
10
```

## 14. 매크로

```c7 run
#define STR(x) #x
#define CAT(a, b) a ## b
#define DECL(name, v) i64 CAT(var_, name) = v;

i32 main() {
    DECL(a, 5)
    DECL(b, 7)
    print_i64(var_a + var_b); newline();
    println(STR(hello    world));
    return 0;
}
```

```text
12
hello world
```

## 15. 종료 코드와 panic

```c7 run rc=3
i32 main() { return 3; }
```

```text
```

0 나누기와 `assert` 실패, `Vec::at` 범위 초과는 `panic`(abort, 종료 코드 134)이다.

```c7 run rc=134
i32 main() {
    assert(1 + 1 == 3, "arithmetic is broken");
    return 0;
}
```

```text
```

## 16. 테스트

```c7 test
@test
void strings_compare() {
    Str a = "abc";
    assert(a == "abc", "equal");
    assert(a != "abd", "different");
}

@test
void hashmap_roundtrip() {
    mut HashMap<i32, i32> m = HashMap<i32, i32>();
    m.put(1, 10);
    i32 k = 1;
    assert(*m.get(&k) == 10, "value");
}
```
