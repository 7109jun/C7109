# `.mp7` 명세

## 목적 및 형식
`.mp7`은 C7109 빌드 결과의 맵/리스팅 정보를 담는 사람이 읽을 수 있는 텍스트 파일이다. UTF-8을 사용한다.

## 정보 후보
다음은 수록 가능한 정보이며 전부 필수로 확정된 것은 아니다.
- 프로젝트 및 타깃
- 빌드 구성
- 입력 소스와 출력 파일
- 심볼 및 주소 맵
- 섹션 정보
- 링크된 라이브러리
- 메모리 배치
- 진단 요약

## 비규범적 예시
```text
C7109 MAP FILE
Format: draft
Target: x86_64-unknown-linux
Configuration: Release

[Inputs]
src/main.c7

[Outputs]
build/app

[Symbols]
; implementation-defined

[Sections]
; implementation-defined
```

## 구현 권고
주소와 크기를 출력할 경우 단위와 표기법을 명시한다. 최적화로 제거된 심볼을 오해하지 않도록 표시한다. 실패한 빌드의 불완전 결과물을 성공한 맵 파일처럼 남기지 않는 것을 권장한다.

## 미결정
정식 문법/버전 헤더, 필수 섹션, 주소 표기, 심볼 정렬, 소스 행과 기계 코드 주소 연결, 디버그 정보 관계, 생성 시점 및 실패 처리.

## 구현이 정한 형식 (제안 — 소유자 확인 ⚠ Q8)
`Format: 1`. 헤더(`Project`/`Target`/`Platform`/`Configuration`/`Compiler`/`Units`) 뒤에 `[Inputs] [Libraries] [Outputs] [Sections] [Symbols]`.
주소는 `0x%016x`, 크기는 바이트(10진). 심볼은 주소순, `nm` 유형 문자와 복호화된 이름. 도구가 없으면 `; unavailable: …`.
정책: 성공 뒤에만 임시 파일+rename으로 쓰고, 빌드 시작 때 이전 맵을 지운다. 자세한 내용은 `docs/stage5-decisions.md`.
