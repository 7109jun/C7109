# `.cset7` 명세

## 목적 및 형식
컴파일러 및 빌드 환경 설정 파일이다. 형식은 TOML이며 UTF-8로 인코딩한다.

## 설정 범주
- 컴파일러 및 버전 요구
- 대상 OS, CPU 아키텍처, ABI
- Debug/Release 및 최적화 수준
- 코드 생성 옵션
- 진단 및 경고 설정
- 정적/동적 링크 선택
- include 및 라이브러리 경로
- 기능 및 확장 옵션

## 우선순위
명령행에서 직접 지정한 옵션은 `.cset7`의 동일 설정을 덮어쓴다. 전역/프로젝트 설정 간 탐색 및 우선순위는 미결정이다.

## 비규범적 예시
```toml
[compiler]
# version = "1.x.y"

[target]
# os = "linux"
# architecture = "x86_64"

[build]
# mode = "debug"
# optimization = "O0"

[link]
# mode = "static"
```

위 키 이름과 값은 설명용 예시이며 최종 스키마로 확정되지 않았다.

## 미결정
정식 테이블과 키, 자료형·기본값·허용 범위, 파일 탐색 경로, include/inherit, 환경 변수 치환, 스키마 버전, 알 수 없는 키의 오류/경고 정책.
