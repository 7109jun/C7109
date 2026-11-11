# `.cr7` 명세

## 목적 및 형식
프로젝트의 의존성과 빌드 재현 정보를 고정하는 잠금 파일이다. TOML 형식을 사용한다.

## 기록해야 하는 정보
- 컴파일러 정보
- 대상 플랫폼 및 아키텍처
- 빌드 옵션
- 의존성의 정확한 버전
- 의존성 식별 및 획득에 필요한 출처 정보

의존성 충돌이 발생하면 위험한 자동 버전 변경으로 해결하지 않고 명시적인 오류를 보고해야 한다.

## 재현성
잠금 파일을 사용하면 동일한 의존성 버전과 타깃 설정을 선택해야 한다. 모든 환경에서 바이너리가 바이트 단위로 동일하다는 보장은 별도 규격 사항이다.

## 비규범적 예시
```toml
[lock]
# format_version = 1

[compiler]
# name = "C7109"
# version = "1.x.y"

[target]
# os = "linux"
# architecture = "x86_64"

[build]
# mode = "release"
# optimization = "O2"

[[dependency]]
# name = "Example"
# version = "1.2.3"
# source = "official"
# sha256 = "..."
```

## 갱신 규칙
잠금 파일을 읽는 것만으로 버전을 자동 갱신해서는 안 된다. 의존성 변경은 명시적 갱신 작업으로 수행한다.

## 미결정
정식 스키마/버전, 출처 식별, 해시 필수 여부, Git 커밋 식별, 플랫폼별 의존성, 선택적 의존성, 정규화 및 갱신 명령.

## 구현이 정한 형식 (제안 — 소유자 확인 ⚠ Q2, Q6, Q7)
파일 `Cmake7109.cr7`(프로젝트 디렉터리). `[lock] format_version = 1`, `[compiler]`, `[target]`, 선택 `[build] config`, `[[dependency]]`(`name version source sha256`).
`c7c lock`만 쓰고 빌드는 읽기 전용. 어긋남 `E0714`, 해시 불일치 `E0710`. 자세한 내용은 `docs/stage5-decisions.md`.
