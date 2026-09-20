# C++ MO Game Project

**파티 단위 던전 플레이를 위한 C++ 권위 서버와 Unreal 클라이언트를 만드는 게임 서버 포트폴리오 프로젝트입니다.**

엔진 독립 서버의 상태 소유권과 동시 실행 구조를 직접 설계하고, 부하·응답성·정합성을 측정으로 검증합니다.

## 프로젝트 목표

> 마을 → 파티 구성 → 던전 입장 → 전투·목표 수행 → 보상 → 마을 복귀

- **플레이 흐름 구현:** 서버가 이동·공격·피격·쿨다운·보상을 검증하는 MO 액션 RPG를 구현합니다.
- **부하 편차에 대응:** 여러 던전을 병렬 실행하면서 특정 던전의 과부하와 서버 전체의 포화를 구분해 대응합니다.
- **재현 가능한 검증:** Unreal 샘플로 플레이를 확인하고, UI 없는 콘솔 클라이언트로 실제 프로토콜의 부하와 응답성을 측정합니다.

## 개발 방향

기능 개수보다 **경계의 정확성과 측정 가능성**을 우선합니다.

| 원칙 | 의미 |
| --- | --- |
| 상태 소유권을 먼저 정의한다 | 모든 상태에 유일한 변경 주체를 둡니다. 네트워크 콜백에서 게임 상태를 직접 바꾸지 않고 명령과 완료 이벤트로만 연결합니다. |
| 의존성보다 계약을 먼저 검증한다 | transport·DB에 독립적인 실행·종료·소유권 계약을 동시성으로 검증한 뒤 외부 라이브러리를 선택합니다. |
| 측정으로 설계를 고른다 | 평균이 아니라 p99·p99.9 분포와 실패율을 봅니다. 두 설계를 같은 조건에서 돌려 비교합니다. |
| 제안과 결정을 구분한다 | 실험 전 가정치를 확정 사양처럼 쓰지 않습니다. |

## 목표 아키텍처

전투의 권위 상태는 **엔진 독립 C++ 서버**가 소유합니다. Unreal은 입력·화면 표현·예측과 보정을 담당합니다. 마을·파티·영속 데이터 처리와 던전 시뮬레이션은 책임을 분리하며, 배포 단위는 검증 결과에 따라 확장합니다.

```mermaid
flowchart LR
    UE[Unreal 클라이언트] --> Control[마을 · 파티 · 입장 조정]
    Bot[콘솔 부하 클라이언트] --> Control
    Control -->|던전 할당| Runtime[C++ 던전 Room runtime]
    UE -->|전투 입력 · 상태 동기화| Runtime
    Bot -->|동일 전투 프로토콜| Runtime
    Runtime -->|결과 비동기 전달| Persistence[보상 · 영속 데이터 처리]
```

| 설계 주제 | 접근 방식 |
| --- | --- |
| 상태 소유권 | 같은 Room의 상태 변경은 한 번에 하나의 실행 흐름만 담당합니다. |
| 실행 예약 | Room을 특정 스레드에 영구 고정하지 않고 공유 worker 풀에서 실행합니다. |
| 과부하 제어 | mailbox 크기와 한 번의 실행량을 제한합니다. 단일 Room의 계산 비용과 노드 수용량은 별도로 관리합니다. |
| 비동기 경계 | 네트워크 I/O·DB 대기가 시뮬레이션을 막지 않도록 명령과 완료 이벤트로 연결합니다. |
| 콘텐츠 제작 | 공통 원본에서 서버·클라이언트 데이터를 생성하고 schema·참조·버전을 검증합니다. |
| 성능 검증 | tick 지연, 입력 응답 p99, 큐 포화, 종료·장애 시 정합성을 확인합니다. |

## 프로젝트 구조

현재는 크로스 플랫폼 빌드 구성만 있습니다.

```text
mo-game-project/
  CMakeLists.txt
  CMakePresets.json         # macOS/Windows/Linux 구성과 sanitizer 프리셋
  cmake/                    # 공통 빌드·toolchain 확인
```

예정 구조입니다.

```text
  server/
    runtime/                # Room 실행기, mailbox, 지연 측정
    apps/edge-control/      # 계정·파티·입장 조정
    apps/world-worker/      # 마을·던전 실행 노드
    domain/                 # party, entry, run 정책
    persistence/            # transaction, 원장, outbox
  shared/
    core/                   # ID, clock, 공통 오류
    protocol/               # 메시지 schema, codec, 버전
    transport/              # transport adapter, session
    simulation/             # 엔진 독립 전투·목표
  clients/unreal/           # Unreal 샘플 클라이언트
  tools/load-client/        # 실제 프로토콜 부하 발생기
  tests/ data/ schemas/
```

`simulation`은 transport·DB·Unreal 헤더에 의존하지 않습니다. 네트워크 DTO와 시뮬레이션 명령은 경계에서 변환합니다. 서버는 CMake, Unreal은 Unreal Build Tool을 사용하며 모노레포 안에서 독립 빌드가 가능해야 합니다.

## 빌드

macOS에서 C++20을 지원하는 Apple Clang과 CMake 3.25 이상을 설치한 뒤 실행합니다.

```sh
cmake --preset macos-debug
cmake --build --preset macos-debug
```

`cmake/CheckToolchain.cmake`가 구성 단계에서 C++20 컴파일과 링크를 확인합니다. Windows는 `windows-msvc` 구성에 `windows-debug`/`windows-release` 빌드, Linux는 `linux-debug`/`linux-release`를 사용합니다.

실행 중 버그를 잡는 sanitizer 구성도 함께 준비했습니다.

| 프리셋 | 도구 | 잡는 문제 |
| --- | --- | --- |
| `macos-asan` | AddressSanitizer + UndefinedBehaviorSanitizer | 버퍼 오버런, 해제 후 사용, 메모리 누수, 정의되지 않은 동작 |
| `macos-tsan` | ThreadSanitizer | 두 스레드가 동기화 없이 같은 메모리에 접근하는 데이터 레이스 |

멀티스레드 코드의 데이터 레이스는 일반 테스트에서 우연히 통과하는 경우가 많습니다. ThreadSanitizer는 충돌이 실제로 발생하지 않아도 동기화가 빠진 접근 자체를 찾아내므로, Room 실행기를 만드는 동안 상시로 돌립니다.

## 아직 확정하지 않은 것

확정된 것은 엔진 독립 C++ 서버 + Unreal 클라이언트 구조와 개발 환경입니다. 아래는 측정·결정 전의 제안입니다.

- 네트워크 라이브러리(Asio / GameNetworkingSockets)와 패킷 직렬화 방식
- 파티 인원(4인 PvE 제안), tick rate(30/60 Hz 대조 예정), snapshot 주기
- 영속 계층(PostgreSQL 제안), 장애 시 던전 중단·입장 자원 복구 정책
- Room 실행기의 최종 구현 — 공유 worker 풀을 기준으로 후보를 비교할 예정
