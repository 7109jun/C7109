# 표준 라이브러리 API

`std` 네임스페이스. 컴파일마다 자동으로 로드되고 `import` 없이 보인다(사용자 선언이 우선). 원본은 `stdlib/*.c7`(C7로 작성)이며 `make`가 `std.l7` 패키지로 묶는다. 예제는 `tools/doccheck.py`가 검증한다.

> 설계 제약: 공유 코드 제네릭이라 `T`를 집합체 안에 **값으로** 담지 못한다. 컨테이너는 `T*`·`out`으로 접근한다. 또한 컨테이너 요소는 **소멸자가 없는 타입**이어야 한다(`drop` 타입을 제네릭 인수로 쓰면 `E0901`).

## core — 출력과 중단

| 함수 | 설명 |
|---|---|
| `void print(Str s)` / `println(Str s)` | 문자열 출력(`println`은 줄바꿈 포함) |
| `void print_i64(i64)` `print_u64(u64)` `print_f64(f64)` | 숫자 출력(줄바꿈 없음) |
| `void print_char(Char c)` | 문자 한 개를 UTF-8로 출력 |
| `void print_bool(bool b)` | `true` / `false` |
| `void newline()` | 줄바꿈 |
| `@noreturn void panic(Str msg)` | 메시지를 stderr에 쓰고 abort(종료 코드 134) |
| `void assert(bool cond, Str msg)` | 거짓이면 `panic(msg)` |
| `@noreturn void unreachable()` | 도달하면 안 되는 지점 |
| `T bitcopy<T>(T x)` | `drop` 타입을 의도적으로 비트 복사 |
| `enum AllocError : u8 { OutOfMemory }` | 할당 실패 오류 |

```c7 run rc=134
i32 main() {
    println("before");
    panic("stop here");
    return 0;
}
```

```text
before
```

## Str — 문자열 뷰

`Str`은 UTF-8 `(ptr, len)` 뷰다(소유하지 않음). `len`은 **바이트** 수.

| 멤버 | 설명 |
|---|---|
| `s.ptr`, `s.len` | 데이터 포인터(`const u8*`)와 바이트 길이 |
| `s[i]` | 바이트 `u8` |
| `Chars chars()` | UTF-8 디코딩 이터레이터(`Option<Char>`) |
| `CStr as_cstr()` | C 문자열로 보기(널 종료는 호출자 책임) |
| `bool empty()` | 길이 0 |
| `Str slice(usize lo, usize hi)` | `[lo, hi)` 바이트 부분 뷰(경계 검사 없음 — 문자 경계는 호출자 책임) |
| `bool starts_with(Str prefix)` | 접두사 검사 |
| `a == b`, `a != b` | 바이트 단위 비교 |

```c7 run
i32 main() {
    Str s = "a한b";
    mut usize n = 0;
    for (c in s.chars()) { n += 1; }
    print_u64(s.len as u64); print(" "); print_u64(n as u64); newline();
    print_bool(s.empty()); newline();
    return 0;
}
```

```text
5 3
false
```

## StrBuf — 늘어나는 문자열 버퍼

소유 버퍼. 소멸 시 자동 해제(RAII). 항상 널 종료된다.

| 멤버 | 설명 |
|---|---|
| `StrBuf()` | 빈 버퍼 |
| `mut bool append(Str s)` | 끝에 추가. 메모리 부족이면 `false` |
| `mut bool push_char(Char c)` | UTF-8로 문자 추가. 실패 시 `false` |
| `mut bool reserve(usize extra)` | 추가로 `extra`바이트 확보 |
| `mut void clear()` | 비우기(용량 유지) |
| `usize size()` | 바이트 길이 |
| `Str as_str()` / `CStr as_cstr()` | 뷰 얻기(버퍼가 바뀌면 무효) |

```c7 run
i32 main() {
    mut StrBuf sb = StrBuf();
    sb.append("n=");
    sb.append("42");
    println(sb.as_str());
    sb.clear();
    print_u64(sb.size() as u64); newline();
    return 0;
}
```

```text
n=42
0
```

## Vec\<T\> — 동적 배열

| 멤버 | 설명 |
|---|---|
| `Vec<T>()` | 빈 벡터(할당 없음) |
| `mut Result<void, AllocError> push(T value)` | 끝에 추가. 필요하면 용량을 2배로(최소 4) |
| `mut bool pop(out T item)` | 마지막 값을 `item`에 꺼냄. 비어 있으면 `false` |
| `T* at(usize i)` | 요소 포인터. 범위 밖이면 `assert` 실패(panic) |
| `usize size()` / `capacity()` | 개수 / 용량 |
| `mut void clear()` | 개수만 0으로(용량 유지) |

```c7 run
i32 main() {
    mut Vec<i32> v = Vec<i32>();
    for (i in 0..10) { v.push(i); }
    mut i32 top = -1;
    v.pop(out top);
    print_i64(top as i64); newline();
    print_u64(v.size() as u64); newline();
    *v.at(0) = 100;
    print_i64(*v.at(0) as i64); newline();
    return 0;
}
```

```text
9
9
100
```

`push`의 결과는 `Result`다. 메모리 부족을 처리하려면 `try`나 `match`로 확인한다.

## HashMap\<K : Hashable, V\>

열린 주소법(선형 탐사, 2의 거듭제곱 용량). 키는 `Hashable`을 구현해야 한다. 기본 제공: `i32 i64 u32 u64 usize Str`.

```c7
interface Hashable {
    u64 hash();
    bool eq(const Self* other);
}
```

| 멤버 | 설명 |
|---|---|
| `HashMap<K, V>()` | 빈 맵 |
| `mut Result<void, AllocError> put(K key, V value)` | 삽입 또는 덮어쓰기 |
| `V* get(const K* key)` | 값 포인터, 없으면 `null` |
| `bool contains(const K* key)` | 키 존재 여부 |
| `mut bool remove(const K* key)` | 제거. 있었으면 `true` |
| `usize size()` | 항목 수 |

직접 만든 타입을 키로 쓰려면 `Hashable`을 구현한다.

```c7 run
struct Id { i32 v; }

impl Hashable for Id {
    u64 hash() { return self->v as u64; }
    bool eq(const Id* other) { return self->v == other->v; }
}

i32 main() {
    mut HashMap<Id, i32> m = HashMap<Id, i32>();
    m.put(Id{ v: 7 }, 70);
    Id k = Id{ v: 7 };
    print_i64(*m.get(&k) as i64); newline();
    return 0;
}
```

```text
70
```

## Arena — 범프 할당자

블록 하나를 앞에서부터 잘라 준다. 개별 해제는 없고 `reset()`/소멸 때 한꺼번에 해제한다. **소멸자가 필요한 객체는 넣지 않는다.**

| 멤버 | 설명 |
|---|---|
| `Arena(usize capacity)` | 블록 확보(실패하면 용량 0) |
| `mut void* alloc_bytes(usize size, usize align)` | 정렬된 바이트. 소진되면 `null`. `align`은 2의 거듭제곱 |
| `mut bool create<T>(out T* slot)` | `T` 하나. `T`는 `slot` 타입에서 추론 |
| `mut bool create_array<T>(usize n, out T* slot)` | `T` n개 |
| `mut void reset()` | 전부 되돌림 |
| `usize used()` / `capacity()` | 사용량 / 총량 |

```c7 run
i32 main() {
    mut Arena a = Arena(64);
    mut i64* n = null;
    a.create(out n);
    *n = 5;
    print_u64(a.used() as u64); newline();
    a.reset();
    print_u64(a.used() as u64); newline();
    return 0;
}
```

```text
8
0
```

## 디버그 할당자

`-fdebug-alloc`로 빌드하면 `alloc`/`free`가 추적된다. 누수는 종료 시 보고하고 **종료 코드 70**, 이중/잘못된 해제는 즉시 panic. 해제된 메모리는 `0xDD`로 채우고 격리 큐(4096)에 둔다.

## 런타임 C 함수

표준 라이브러리 밑에서 `runtime/c7rt.c`의 `c7_*` 함수를 `extern "C"`로 쓴다(`c7_write`, `c7_print_i64`, `c7_panic_str` …). 직접 쓰는 프로그램도 같은 방식으로 선언할 수 있지만, 일반 코드는 위 `std` 함수를 쓰는 것이 좋다.

## 아직 없는 것

OS·파일·네트워크·스레드·수학/SIMD·암호·압축·시간·프로세스·터미널은 로드맵 8단계. `std::test::assert_eq`도 없다(현재는 `assert(cond, "msg")`).
