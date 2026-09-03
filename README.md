# 라즈베리 파이를 활용한 LED 두더지 게임 (IoT Mole Game)

라즈베리 파이의 GPIO 제어와 TCP/IP 소켓 통신을 결합해 구현한 IoT 기반 대전형 LED 두더지 게임 프로젝트입니다.  
2025년 3학년 2학기 `IoT 실습과 응용` 강의 팀 프로젝트로 진행했으며, 임베디드 제어, 논블로킹 게임 루프, 네트워크 멀티플레이 구조를 함께 다뤘습니다.

> 이 저장소는 2025년 팀 프로젝트 과제물을 바탕으로,  
> 당시 로컬 환경에 보관하던 소스코드를 **포트폴리오 및 코드 아카이브 목적**으로  
> 개인 GitHub 저장소에 옮겨 정리한 버전입니다.  
> 원 프로젝트는 Git 기반 협업으로 진행되지 않았으며, 팀원별로 각자 로컬에서 작업한 뒤 통합했습니다.

---

## 1. 프로젝트 개요

- **진행 기간**: 2025.11 ~ 2025.12
- **프로젝트 형태**: 4인 팀 프로젝트
- **개발 배경 및 목적**
  - 단순 GPIO 실습을 넘어, LED·키패드·Text LCD·부저를 하나의 시스템으로 제어해보기 위해 시작했습니다.
  - `delay()` 중심의 블로킹 방식 대신 `millis()` 기반 비동기 흐름을 적용해 실시간 입력 처리 구조를 구현하고자 했습니다.
  - 단일 기기 동작에 그치지 않고, TCP/IP 소켓 통신을 통해 여러 대의 라즈베리 파이 간 멀티플레이 구조를 구현해보고자 했습니다.

---

## 2. 기술 스택

- **개발 언어**: C, Python
- **운영체제 환경**: Raspberry Pi OS (Linux), Windows 10 / 11
- **개발 도구**: GCC (Linux CLI), Visual Studio Code
- **서버 및 네트워크**: TCP/IP Socket, Non-blocking I/O, Python Multi-threaded Server
- **데이터베이스**: Firebase Realtime Database
- **하드웨어 환경**: Raspberry Pi, LED, Keypad, Text LCD (16x2), Buzzer
- **주요 라이브러리**
  - **C (Client)**: `wiringPi.h`, `lcd.h`, `softTone.h`, `sys/socket.h`, `fcntl.h`, `arpa/inet.h`, `unistd.h`
  - **Python (Server)**: `socket`, `threading`, `time`, `random`

---

## 3. 실행 방법

### 1) 중앙 중계 서버 실행

```bash
python server.py
```

### 2) 라즈베리 파이 클라이언트 빌드 및 실행

```bash
gcc -o mole_game IoT.c -lwiringPi -lwiringPiDev -lpthread
sudo ./mole_game
```

### 3) 사전 설정

- `IoT.c` 내부 `SERVER_IP`를 실제 서버 PC 주소로 수정합니다.
- GPIO 핀맵 및 하드웨어 연결 정보는 소스코드에서 확인할 수 있습니다.
- Firebase 연동 스크립트 및 실행 환경을 확인합니다.

### 4) 게임 조작

- **메인 메뉴**: `Single Play`, `Multi Play`, `Ranking`, `Exit`
- **Single Play**: 30초 제한 시간, 난이도 상승, 콤보 점수, 시간차 스폰
- **Multi Play**: Ready 동기화 후 실시간 대전 진행
- **Ranking**: Firebase 기반 랭킹 조회

---

## 4. 시스템 아키텍처 및 핵심 포인트

### 시스템 아키텍처

- **H/W & Network**

  <img width="957" height="641" alt="image" src="https://github.com/user-attachments/assets/b8d00b21-a488-4256-a5be-6b1cc0a18eb9" />

- **S/W**

  <img width="944" height="551" alt="image" src="https://github.com/user-attachments/assets/a81e30f6-7c84-4bcb-86ab-64ed5e7c09dc" />

### 핵심 포인트

- **논블로킹 게임 루프**
  - `delay()` 대신 `millis()` 기반 시간 비교 방식 사용
  - 입력, 사운드, LCD 갱신을 분리해 처리

- **입력 처리**
  - 비트마스킹 기반 버튼 상태 관리
  - 상승 엣지 검출로 중복 입력 방지

- **멀티플레이 통신 구조**
  - 문자열 프로토콜 기반 메시지 흐름
  - `WAITING`, `REQ_START`, `START`, `HIT`, `SCORE`, `GAMEOVER`

- **C + Python 하이브리드 구조**
  - C: 하드웨어 제어 및 게임 루프
  - Python: 서버 중계 및 점수 처리

### 주요 파일

- `IoT.c`
  - 라즈베리 파이 메인 제어
  - 싱글/멀티 게임 루프
  - 입력 처리, LCD, 부저, 서버 통신 담당

- `server.py`
  - 멀티플레이 접속 처리
  - Ready 동기화
  - 점수 계산 및 게임 진행 제어 담당

---

## 5. 팀원 구성 및 역할

- **이헌영 (본인)**
  - 과제 총괄
  - 게임 흐름 통합
  - 네트워크 멀티플레이 구조 정리
  - 최종 디버깅 및 보고서 정리

- **이동우**
  - 하드웨어 결선
  - GPIO 핀맵 정리
  - 부품 단위 테스트

- **김영제**
  - 게임 로직 구현
  - LCD 메뉴 UI
  - Firebase 연동 스크립트 개발

- **소주성**
  - 게임 로직 구현
  - 랭킹 저장/조회 인터페이스 개발
  - Firebase 관련 자료조사

---

## 6. 결과 및 성과

- 라즈베리 파이 기반 GPIO 제어와 게임 로직을 하나의 시스템으로 통합했습니다.
- `Single Play`, `Multi Play`, `Ranking` 기능을 구현했습니다.
- `millis()` 기반 논블로킹 구조를 적용해 입력, 사운드, LCD 흐름을 분리했습니다.
- Python TCP/IP 서버를 이용해 멀티플레이 게임 구조를 구성했습니다.
- Firebase Realtime Database를 활용해 점수 저장 및 랭킹 조회 기능을 구현했습니다.

---

## 7. 트러블 슈팅

### 1) 로컬 2인 구조의 한계
초기에는 하나의 키트를 두 명이 나눠 쓰는 구조를 고려했지만,  
버튼 수와 물리적 간섭 문제로 인해 플레이 경험이 좋지 않았습니다.  
이후 각 플레이어가 독립된 라즈베리 파이를 사용하고, 서버가 상태를 중계하는 네트워크 멀티플레이 구조로 전환했습니다.

### 2) `delay()` 기반 블로킹 문제
`delay()`에 의존하면 입력 감지와 화면 갱신이 멈추는 문제가 있었습니다.  
이를 `millis()` 기반 시간 비교 방식으로 바꿔 입력, LCD, 사운드 처리가 계속 흐르도록 구성했습니다.

### 3) 랭킹 등록 시 부저음 지속 문제
블로킹 입력 함수 진입 시 사운드 종료 루틴이 실행되지 않아 부저가 계속 울리는 문제가 있었습니다.  
입력 단계 진입 전 `stopTone()`을 명시적으로 호출해 해결했습니다.

---

## 8. 회고

이 프로젝트를 통해 단순 GPIO 제어를 넘어,  
실시간 입력 처리, 논블로킹 설계, 소켓 통신, 기능 통합의 중요성을 직접 경험했습니다.

또한 팀 프로젝트에서 각자 작성한 코드를 하나의 동작하는 결과물로 맞추는 과정이  
구현만큼이나 중요하다는 점을 배웠습니다.

---

## 9. 라이선스

본 프로젝트는 [MIT License](LICENSE)를 따릅니다. 오픈소스 및 포트폴리오 목적으로 자유로운 수정 및 복제가 가능합니다. (자세한 라이선스 전문은 [LICENSE](LICENSE) 파일 참고)
