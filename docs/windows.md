# Windows 대상 (x86-64 PE)

로드맵 7단계의 첫 구현이다. `c7c`가 **Linux에서 Windows용 실행 파일(PE32+, 콘솔)을 크로스 컴파일**한다. Windows SDK, MSVC, C 런타임(CRT)은 필요 없다.

## 사용법

```sh
c7c build hello.c7 --target=x86_64-windows -o hello.exe
```

필요한 도구(Ubuntu: `apt-get install llvm clang lld`): `llc`, `clang`, `llvm-dlltool`, `lld-link`.
`--target`은 `x86_64-windows`(별칭 `x86_64-win32`)와 `x86_64-linux`만 코드 생성을 지원한다. 다른 값은 `E0715`.

프로젝트 모드에서는 타깃에 `platform "x86_64-windows"`를 쓴다. 출력 기본 이름은 `<이름>.exe`이고, 같은 프로젝트에 Linux 타깃과 Windows 타깃을 함께 둘 수 있다. 캐시 키에 플랫폼이 들어 있다.

```buildfile
project "App":
    version "1.0.0"

target executable "app":
    sources:
        "src/main.c7"
    platform "x86_64-windows"
```

## 어떻게 만들어지나

1. 일반 파이프라인으로 LLVM IR을 만들되 트리플을 `x86_64-pc-windows-msvc`로 한다.
2. `llc`가 COFF 오브젝트를 만든다.
3. `runtime/c7rt_win.c`(자립형 런타임)를 `clang --target=x86_64-pc-windows-msvc -ffreestanding`으로 컴파일한다.
4. `kernel32.dll`·`ucrtbase.dll`의 import 라이브러리를 `llvm-dlltool`로 **그때그때 생성**한다(SDK의 `.lib`를 쓰지 않는다).
5. `lld-link /subsystem:console /entry:c7_start /nodefaultlib`로 PE를 링크한다.

`c7_start`가 `main`을 호출하고 stdout을 비운 뒤 `ExitProcess`한다. 호출 규약은 Linux와 같은 규칙(내부 `fastcc`, `extern "C"`는 Windows x64)이다.

## 런타임 동작

| 항목 | 동작 |
|---|---|
| 출력 | stdout은 버퍼링(종료·패닉 때 비움), stderr는 즉시. 콘솔 코드 페이지를 UTF-8로 설정 |
| `panic` / 실패한 `assert` | stderr에 `panic: …`를 쓰고 종료 코드 **134**(Linux `abort`와 동일) |
| 디버그 할당자(`-fdebug-alloc`) | Linux와 같은 설계. 누수는 종료 코드 **70** |
| 힙 | `HeapAlloc`(16바이트 정렬) |
| `f64` 출력 | `ucrtbase.dll`의 `__stdio_common_vsprintf`로 `%g` (Windows 10 이상) |
| 큰 스택 프레임 | `__chkstk`를 런타임이 제공 |

## 검증

- `make test`는 `tests/run`·`tests/dbg`의 모든 프로그램을 4가지 옵션 조합(`-O0`, `-O2`, `--debug -fbounds-check`, `-fdebug-alloc`)으로 Windows용으로 크로스 빌드하고 PE 헤더(AMD64, PE32+, 콘솔)를 확인한다(`tests/gen_windows_suite.py`).
- Wine이 설치돼 있으면 `make test`가 같은 프로그램을 Wine으로 실행해 대조한다(`C7_RUNNER=wine python3 tests/run_windows_suite.py DIR`, 없으면 건너뛴다고 출력). 59개 모두 통과한다.
- **CI는 이렇게 만든 프로그램을 실제 Windows 러너에서 실행**해 stdout·종료 코드·stderr를 Linux 테스트와 같은 기대값에 대조한다(`tests/run_windows_suite.py`).

## 알려진 한계

- `c7c` 자체는 Linux에서 돈다(CI의 MSYS2 빌드는 실험적). `c7c test`는 테스트 바이너리를 호스트에서 실행하므로 Windows 대상에서는 거부한다.
- 링크 가능한 DLL은 `kernel32`·`ucrtbase`의 고정 목록뿐이다. 사용자 DLL import, 정적/동적 CRT 선택, `.lib` 링크는 아직 없다.
- PDB·디버그 정보, 맵 파일의 PE 심볼/섹션 목록(`.mp7`에 `; unavailable`로 표시), 리소스·매니페스트·아이콘, GUI 서브시스템, DLL 출력은 없다.
- 표준 라이브러리의 OS 기능(파일, 스레드 등)은 8단계 범위다.
- ABI(`fastcc`) 재측정은 아직 하지 않았다(`STATUS.md` §ABI의 "새 타깃마다 다시 측정" 항목).
- Windows 10 미만은 지원하지 않는다(`ucrtbase.dll`).
