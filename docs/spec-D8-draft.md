# 규격서 부록 D.8 — 4단계에서 정한 위임 결정 (갱신 문안 초안)

> 규격서(Claude Doc)는 이 저장소에 없어 직접 수정할 수 없다. 아래를 **부록 D의 D.8로 붙여 넣고**,
> 같은 문서의 "현재 진행" 문장 중 굵게 표시된 부분(STATUS §8.1)을 일반 서식으로 되돌린다.
> 각 항목은 근거가 되는 코드·테스트 위치를 함께 적었다. 소유자 확정 항목([확정])은 하나도 바꾸지 않았다.

## D.8.1 표준 라이브러리와 암묵 `std` 프렐류드
- 표준 라이브러리는 `std` 네임스페이스에 C7로 작성하며 컴파일마다 `stdlib/*.c7`를 함께 컴파일한다(`.l7` 패키징은 5단계).
- `import` 없이 `Vec`처럼 쓸 수 있다: 이름 해석 순서는 현재 네임스페이스 → 전체 이름 → `import` 별칭 → `std::`+이름이며
  **암묵 `std`가 가장 낮은 우선순위**라 사용자 선언이 항상 이긴다. (`Sema::lookupGlobal`)

## D.8.2 `void` 페이로드 변형
- `Result<void, E>`의 `Ok`는 인수 없이 만든다(`Ok()`). 패턴 `Ok`와 `Ok()`를 모두 허용한다. (`tests/run/vec.c7`)

## D.8.3 컨테이너 API 설계 원칙과 공개 API
- 공유 코드 제네릭의 현재 한계(`T`를 값으로 포함하는 집합체는 `E0901`) 때문에 **`T`는 집합체에 값으로 담지 않고**
  `T*`·`out` 매개변수로만 다룬다. 컨테이너의 원소 타입은 소멸자가 없는 타입이어야 한다(`drop` 타입 인수는 `E0901`).
- `Vec<T>`: `init()`, `drop()`, `size()`, `capacity()`, `mut Result<void, AllocError> push(T value)`,
  `mut bool pop(out T item)`, `T* at(usize i)`(범위 밖이면 `assert` 실패), `mut void clear()`.
- `Arena`: `init(usize capacity)`, `drop()`, `capacity()`, `used()`, `mut void reset()`,
  `mut void* alloc_bytes(usize size, usize align)`(고갈 시 `null`), `mut bool create<T>(out T* slot)`,
  `mut bool create_array<T>(usize n, out T* slot)`. `T`는 `out` 인수의 타입에서 추론하고, 실패하면 `slot`이 `null`이다.
  멤버 호출의 명시적 타입 인수(`a.create<u8>()`)와 반환 타입만으로의 추론은 아직 지원하지 않는다.
- `interface Hashable { u64 hash(); bool eq(const Self* other); }`, `Hashable` 구현: `i32 i64 u32 u64 usize Str`.
- `HashMap<K : Hashable, V>`: `init()`, `drop()`, `size()`, `mut Result<void, AllocError> put(K key, V value)`(덮어쓰기 허용),
  `V* get(const K* key)`(없으면 `null`), `bool contains(const K* key)`, `mut bool remove(const K* key)`.
  열린 주소법·선형 탐사, 용량은 2의 거듭제곱, 적재율 3/4.

## D.8.4 제네릭 타입 인수의 경계 검사
- `class C<T : Iface>`의 경계는 **타입 표기와 생성 호출 양쪽에서** `E0650`으로 검사한다. 경계가 없는 타입 매개변수 `T`를
  경계가 있는 자리에 넘기는 것도 오류다. (이전에는 제네릭 함수 호출만 검사되어 `HashMap<NoHash, i32>`가 컴파일되고
  실행 시 panic했다.) 테스트: `tests/sema/{pos,neg}_typebounds.c7`.

## D.8.5 디버그 할당자 (`-fdebug-alloc`)
- 켜면 `alloc`/`free`/클로저 환경 할당이 추적 할당자를 거친다.
  **누수**는 종료 시 stderr에 보고하고 **종료코드 70**, **이중 해제**·**C7 할당자가 만들지 않은 포인터의 해제**는
  해당 `free`에서 `panic`(abort)한다. 해제된 메모리는 0xDD로 채우고 4096개 격리 큐에 둔다.
- 알려진 한계: 할당 위치(소스 줄)는 기록하지 않는다. `extern "C"`로 직접 부른 `malloc`은 추적하지 않는다.

## D.8.6 `c7c test`
- 최상위 `@test void name()` 함수를 수집해 하나의 실행 파일로 빌드하고 **테스트마다 별도 프로세스**로 실행한다
  (한 테스트의 `assert`/`panic`이 나머지를 막지 않는다). 출력은 `test NAME ... ok|FAILED (exit N)`와
  `test result: P passed; F failed`, 하나라도 실패하면 종료코드 1.
- 테스트 빌드는 전처리 매크로 `C7_TEST`를 정의한다(파일이 자기 `main`을 `#ifndef C7_TEST`로 감쌀 수 있다).
  테스트 파일은 `namespace` 선언 없이 최상위에 둔다. 규격서 예제의 `std::test::assert_eq`는 제네릭 값의 `==`가
  지원될 때까지 제공하지 않는다(현재는 `assert(cond, "msg")`).

## D.8.7 ABI v1 (잠정, x86-64 Linux)
- C7 내부 호출의 호출 규약은 **LLVM `fastcc`**(`extern "C"`와 `main`은 C 규약). 근거는 `bench/abi/RESULTS.md`:
  `fastcc`는 C 규약과 ±3% 이내로 동등, `preserve_most`는 최대 1.47배 느림, `swiftcc`는 4워드(32B) 이상 값 반환·전달에서만
  1.5배 빠르고 표준 라이브러리는 그런 반환을 만들지 않는다.
- **13.6절의 레지스터 배정 초안은 구현·비교하지 못했다.** 새 타깃(AArch64, x86-32, Windows)마다 다시 측정한다.
  호출 규약은 컴파일러의 한 상수(`kC7CallConv`)로 정하므로 변경 비용은 한 줄이다.

## D.8.8 진단 코드 레지스트리 (13.4절 보강)
- 모든 진단 코드는 `compiler/frontend/diag_codes.def` 한 파일에서 정의한다. **중복은 컴파일 오류**이고, 소스에서 쓰지만
  등록되지 않은 코드는 테스트가 실패시킨다.
- 전처리기 `#`/`##`/`__VA_OPT__`를 구현하면서 `E0614`(붙인 결과가 올바른 토큰이 아님)를 추가했고, 더 이상 출력하지 않는 `W0602`는
  **폐기(재사용 금지)** 로 표시했다. 의미는 C 표준을 따르며 gcc와 토큰열을 대조해 검증한다(`#embed`만 미구현).
- 충돌 해소: 전처리기의 `#include` 중첩 과다 오류를 **`E0613`**으로 옮겼다. 의미 분석의 `E0610`(`try`의 오류 타입 변환 불가)
  `E0611` `E0612`는 그대로다. 규격서의 진단 코드 표에서 `E0610`을 전처리기 항목으로 적은 곳이 있으면 `E0613`으로 고친다.

## D.8.9 Debug 정수 오버플로 검사와 `-fbounds-check` (4.6절 구현)
> ⚠ 규격서 4.6절 원문을 보지 못하고 구현했다. ②는 소유자가 확정했고, ①(Debug를 `--debug`로 켠다)은 아직 **가정**이다.
- ① Debug 의미는 컴파일 옵션 `--debug`로 켠다(전처리기 매크로 `__C7_DEBUG__`와 같은 스위치). 꺼져 있으면 Release 의미(래핑).
- ② [확정] Debug에서 정수 `+`, `-`, `*`, 단항 `-`는 오버플로 시 `panic`("integer overflow in addition/subtraction/multiplication/negation")한다.
  **부호 있는 정수와 없는 정수 모두**이다(`u8 0 - 1`도 트랩).
- 부정된 정수 리터럴(`-128` as `i8`)은 상수로 보아 검사하지 않는다(범위는 의미 분석이 검사). 시프트는 기존대로 시프트량을 마스킹한다.
- 검사하지 않는 곳: 128비트 곱셈, `INT_MIN / -1`, 원시 포인터 산술. 의도적 래핑이 필요한 코드는 128비트로 넓혀 곱한 뒤 `as`로 자른다.
- `-fbounds-check`(기본 꺼짐, `--debug`와 독립): 배열 `T[N]`, 슬라이스 `T[]`, `Str`의 인덱싱 `a[i]`와 슬라이싱 `a[lo..hi]`를
  검사해 범위를 벗어나면 `panic`("index out of bounds", "slice end out of bounds", "slice start after end")한다.
  음수 인덱스도 잡는다. **원시 포인터는 길이를 모르므로 검사하지 않는다.**
- 테스트: `tests/chk/*`(트랩·무트랩 경계값), `tests/run` 전체를 `--debug -fbounds-check`로 재실행.

## D.8.10 `goto`의 범위 규칙 (소유자 결정)
- **`E0460`**: `goto`가 변수의 범위 안으로 들어가면 오류다. 변수에는 지역 변수, `x = expr;`로 선언되는 변수, 튜플 분해 변수,
  `for`/`for-in` 변수, `match` 바인딩, 그리고 `defer` 문이 모두 포함된다. 판정은 한 규칙이다: *라벨 위치에서 범위 안에 있는 선언은 모두 `goto`
  위치에서도 범위 안에 있어야 한다*(앞쪽 점프, 안쪽 블록으로의 점프, 반복문·`match` 갈래로의 점프를 모두 포괄).
- **정리 실행**: `goto`로 스코프를 빠져나가면 빠져나가는 스코프의 `drop`과 `defer`가 `return`/`break`처럼 등록의 역순으로 실행된다.
  같은 스코프 안에서 뒤로 점프하면 라벨 이후에 선언된 변수가 소멸된다.
- **`defer`도 대상이다**(소유자 결정): `defer` 문을 건너뛰는 `goto`는 `E0460`(메시지의 이름은 `defer`)이다. 건너뛴 `defer`가
  등록되지 않았는데도 스코프 끝에서 실행되는 것을 막기 위함이다. `defer`가 있는 스코프를 *빠져나가는* 점프와 `defer`보다 앞의
  라벨로의 점프는 허용된다.

## D.8.11 5단계: 빌드 시스템과 패키지 (구현 결정 요약)

- **프로젝트 명령**: `c7c build|pack|verify|lock|install|clean`. `.c7` 인수가 있으면 기존 단일 파일 모드. 종료 코드 0/1/2.
- **빌드 파일** `Cmake7109.txt`: 들여쓰기 기반 순수 선언형(문법 `docs/Cmake7109-grammar.md`). 옵션 우선순위: 기본값 < `.cset7` < `config` < 타깃 < 명령행.
- **패키지** `.l7`: ZIP + `manifest.toml` + `checksums.sha256`, 재현 가능. 해결기는 최고 만족 버전을 고르고 자동 변경하지 않는다.
- **잠금** `.cr7`: `c7c lock`만 쓴다. 빌드는 읽기 전용이며 어긋나면 `E0714`/`E0710`.
- **맵** `.mp7`: 실행 파일 옆 텍스트(형식 1). 실패한 빌드는 맵을 남기지 않는다.
- **캐시**: 타깃 단위, 컴파일러 지문+옵션+소스·라이브러리 해시, 직접 모드 헤더 검증.
- 세부와 미결정(⚠ Q1–Q14)은 `docs/stage5-decisions.md`.
