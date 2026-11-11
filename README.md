# C7109

범용 시스템 프로그래밍 언어 **C7109**와 네이티브 컴파일러 `c7c`(C++20, LLVM 백엔드), 표준 라이브러리, 빌드 시스템.

```c7
i32 main() {
    println("Hello, C7109!");
    return 0;
}
```

```sh
make            # ./c7c 와 ./std.l7
make test       # 전체 테스트
make doccheck   # 문서의 모든 예제를 실제 컴파일러로 검증
./c7c build hello.c7 -o hello && ./hello
```

대상: **x86-64 Linux**(`llc`·`gcc` 필요)와 **x86-64 Windows**(`--target=x86_64-windows`, Linux에서 크로스 컴파일, `clang`·`lld` 필요). CI는 Linux에서 빌드·테스트하고, 크로스 빌드한 Windows 프로그램을 **실제 Windows 러너에서 실행해 검증**한다.

## 문서

| 문서 | 내용 |
|---|---|
| [The C7109 Book](docs/book.md) | 처음부터 배우는 안내서 |
| [C7109 by Example](docs/by-example.md) | 실행 가능한 예제 모음 |
| [표준 라이브러리 API](docs/stdlib.md) | `core` `Str` `StrBuf` `Vec` `HashMap` `Arena` |
| [Cmake7109.txt 빌드 시스템](docs/build-system.md) | 빌드 파일·옵션·의존성·잠금·패키지·맵 |
| [The C7109 Reference](docs/reference.md) | 언어·컴파일러 규칙, 진단 코드 |
| [Windows 대상](docs/windows.md) | x86-64 PE 크로스 컴파일 |
| [STATUS.md](STATUS.md) · [CONTRIBUTING.md](CONTRIBUTING.md) | 프로젝트 현황, 기여 가이드 |

문서의 코드 예제는 `tools/doccheck.py`가 컴파일·실행·출력 대조로 검증한다.

## 라이선스

`GPL-3.0-only`, 표준 라이브러리·런타임에는 C7109 런타임 라이브러리 예외(`GPL-3.0-only WITH C7109-runtime-library-exception`, 법률 검토 필요 — `docs/stage5-decisions.md` Q12).
