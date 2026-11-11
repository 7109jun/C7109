# 5단계 결정 기록 (구현하며 정한 것 — 소유자 확인 필요 항목은 ⚠)

가이드(`C7109 기여`) §4.2의 항목은 그대로 유효하다. 아래는 그 뒤 구현하며 **추가로 정한 것**이다.

| 항목 | 결정 | 근거 / 영향 |
|---|---|---|
| TOML 중첩 한도 | 배열·인라인 표 각각 128단계(`kTomlMaxNesting`), 초과는 `E0701` | B1(세그폴트) 수정. `tests/toml/stricter/`에 `tomllib`보다 엄격한 사례로 기록 |
| 버전 단일 출처 | `compiler/pkg/version.hpp` (`kCompilerName/Version`, `kAbiId`, `kLanguageVersion`) | B2. `c7c --version` → `c7c 1.0.0-alpha.1 (ABI c7abi1)` |
| depfile 정책 | `.l7` 파일(`--lib`, `std.l7`)과 그 진단용 가짜 파일은 **나열하지 않는다**. 소스·`#include`만 | B3. 캐시 키는 라이브러리를 명시적으로 해시(W9) |
| `std.l7` | `make`가 `c7c`와 함께 만든다(`./c7c tool l7-pack`). 이름 `std`, 버전 `1.0.0`, 라이선스 `GPL-3.0-only WITH C7109-runtime-library-exception`(⚠ Q12), `[compiler] requires` 없음(Q5) | 로더 우선순위: `C7_STDLIB` > `std.l7` > `stdlib/`. 검증 실패는 **폴백 없이** 거부 |
| 드라이버 플래그 추가 | `-foverflow-check`, `-fno-overflow-check`, `--link=static|dynamic` (`--debug`가 켜는 오버플로 검사를 따로 끄거나 켜기 위해) | `.cset7`의 `overflow_check`·`[link]`를 정직하게 표현하려면 필요. `static`은 `gcc -static` |
| 한국어 진단 | E0703·W0701의 한국어 문구 인자 순서를 영어와 맞춤 | 기존엔 인자가 뒤바뀌어 출력됨 |
| 빌드 파일 | `docs/Cmake7109-grammar.md`. 구문 오류는 첫 번째에서 멈추고 의미 오류는 모아서 보고. `sources:`가 없거나 비면 `E0702` | 가이드는 `E0704`를 "소스 없음"에 예약 — 이 코드는 **파일이 없을 때**만 쓴다 |
| 타깃의 빌드 옵션 | 타깃 블록도 `config`와 같은 5개 키(`mode optimization bounds_check debug_alloc overflow_check`)를 가질 수 있다 (⚠ 문법 확장) | 우선순위 "타깃 속성 > config"가 의미를 가지려면 필요 |
| 옵션 우선순위 | 내장 기본값 < `.cset7` < `config` < 타깃 < 명령행. 스칼라는 높은 층이 이김, 경로 목록은 낮은→높은 순 **이어 붙이고 중복 제거**(⚠ Q1). 내장 기본값: `mode release`, `O0` (지금의 `c7c build file.c7`과 동일) | `overflow_check`는 명시값이 있으면 그것, 없으면 `mode == debug` |
| `.cset7` 검증 | `[compiler] requires`가 이 컴파일러에 안 맞거나 `[target] abi` 불일치 → `E0719` | 프리릴리스 규칙은 Q5 그대로 (`>=1.0.0`은 `1.0.0-alpha.1`에 안 맞음) |
| 빌드 그래프 | 노드 = 타깃(선언 순) + 의존성(선언 순). 위상 정렬은 "준비된 것 중 가장 앞선 선언 순"(결정적). 순환 오류 위치는 루프를 닫는 노드의 헤더. 라이브러리 기본 출력 `<빌드 디렉터리>/<이름>-<프로젝트 버전>.l7`(버전이 없으면 `0.0.0` ⚠). 출력 충돌은 타깃끼리와 "출력 = 다른 타깃의 소스" 둘 다 `E0707`. 지원 플랫폼은 `x86_64-linux`뿐, 호스트와 타깃은 따로 표현 | W7 |
| 의존성 해결·출처 | 최고 만족 버전, 고정점 확장, 백트래킹 없음, 충돌 `E0713`(요구자 나열), 순환 `E0706`, 레지스트리 파일명↔매니페스트 일치 필수. 중앙/Git/사내는 `E0716` 스텁 | W11 |
| `.cr7` 잠금 | `Cmake7109.cr7`(프로젝트 디렉터리, Q6). `c7c lock`만 쓴다(원자적 rename, 바이트가 같으면 건드리지 않음). 빌드는 읽기 전용: 잠긴 버전만 후보, 없거나 요구와 어긋나면 `E0714`, 목록에 없는/불필요한/출처가 다른 패키지도 `E0714`, `.l7` 해시 불일치 `E0710`. 잠금 파일의 알 수 없는 키는 `W0701`(Q2 기본값) | W12. 잠금 파일이 손상되면 빌드는 실패하고 자동 복구하지 않는다 |
| `.mp7` | 실행 파일 타깃만(Q8), 출력 옆 `<출력>.mp7`. 헤더 `C7109 MAP FILE / Format: 1`, 섹션 `[Inputs] [Libraries] [Outputs] [Sections] [Symbols]`. 심볼은 `_C7$…`(복호화 `a::b(T1, T2)`)·`main`·`c7_*`만, `nm -n -S`·`readelf -SW`로 생성(없으면 `; unavailable: …`). 메모리에 올라가지 않는 섹션(주소 0)은 제외. 빌드 시작 때 낡은 맵을 지우고, 성공(또는 캐시 적중) 뒤에만 임시 파일+rename으로 쓴다. 맵 쓰기 실패는 경고(빌드 실패 아님) | W13. 같은 입력 → 바이트 동일 |
| `c7c install` | `<prefix>/<이름>-<버전>/`, 기본 prefix `$HOME/.c7/packages`(Q14), 인자 없으면 프로젝트의 라이브러리 `.l7`. 전부 먼저 검증하고 하나라도 실패/이미 존재하면 **아무것도 설치하지 않음**. 임시 디렉터리에 추출 후 rename. 소스가 있는 패키지는 ABI만, **소스가 없는 패키지는 이 OS·아키텍처·ABI의 `[[binary]]`가 없으면 `E0712`**(다른 플랫폼 바이너리 대체 금지) | W16 |
| 빌드 파일 출력 경로 | `output`은 프로젝트 안만(Q13). 그래서 `stdlib/Cmake7109.txt`는 `stdlib/std.l7`을 만들고 Makefile이 `./std.l7`로 복사한다(가이드의 `../std.l7`과 다름) | W4 2단계 |
| 7단계 Windows | `--target=x86_64-windows` → COFF(llc) + 자립 런타임 `runtime/c7rt_win.c`(kernel32 + ucrtbase의 `%g`) + 생성한 import lib + `lld-link`. panic 종료 코드 134, 누수 70으로 Linux와 일치. 프로젝트 모드 `platform "x86_64-windows"`, 출력 `.exe`, `.mp7`은 PE 섹션/심볼 목록 `; unavailable`. 검증: Linux에서 크로스 빌드한 59개 프로그램을 CI의 Windows 러너에서 실행 | 7단계(`docs/windows.md`) |
