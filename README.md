# LEDspot — 랜덤 숫자 색 판정 (PC ↔ 보드 LED 동기화)

AMS 현장실습 프로젝트 (2026.11.11 ~ 11.12)

PC(C# WPF)와 STM32H563 Nucleo 보드가 시리얼(COM3, 115200)로 랜덤 숫자를 주고받고, 숫자의 자리 수로 색을 판정해 **화면 LED와 보드 LED를 같은 색으로** 켭니다. 결과는 결과표에 회차별로 기록하고 CSV · JSON으로 저장합니다.

## 동작 방식

| 방식 | 시작 | 숫자를 만드는 쪽 | 색을 판정하는 쪽 |
|---|---|---|---|
| A 방식 | 보드 USER 버튼 | 보드 (`RAND_SEND`) | PC → `LED_WRITE`로 보드에 알림 |
| B 방식 | PC 시작 버튼 | PC (`RAND_REQ`) | 보드 → LED 켜고 `LED_RESP`로 PC에 알림 |

- 반복 횟수와 간격(ms)은 PC 화면 입력값만 사용합니다. 보드는 횟수를 모르고, 간격 대기도 PC가 합니다.
- 첫 회차 전에 카운트다운: `LED_MASK` 0x07 → 0x03 → 0x01 → 0x00 (각 1초)
- 색 판정: 숫자가 0이면 초록, 0이 아니고 자리 수가 짝수면 노랑, 홀수면 빨강
- 회차마다 LED는 잠깐(100ms) 꺼졌다가 새 색으로 켜짐 (같은 색이 연속돼도 바뀐 것이 보이도록)

## 통신 프로토콜

`STX(0x02) | CMD | LEN | DATA × LEN | CRC-8 | ETX(0x03)` — 전체 길이 `LEN + 5`

- CRC-8: 다항식 0x07, 초기값 0x00, CMD부터 DATA까지
- 수신은 **LEN으로 길이를 계산해 자름** (숫자 2, 3이 STX/ETX와 같을 수 있어서 끝 바이트로 자르지 않음)
- CRC가 틀리면 NAK, 맞으면 ACK (DATA = 받은 CMD)

| CMD | 이름 | 방향 | DATA |
|---|---|---|---|
| 0x01 | LED_WRITE | PC → MCU | 색 번호 (0 끄기, 1 초록, 2 노랑, 3 빨강) |
| 0x02 | LED_MASK | PC → MCU | 비트 마스크 (bit0 초록, bit1 노랑, bit2 빨강) |
| 0x06 | ACK | 양쪽 | 받은 CMD |
| 0x15 | NAK | 양쪽 | 받은 CMD |
| 0x20 | RAND_SEND | MCU → PC | 자리마다 1바이트 (0~9) |
| 0x21 | RAND_REQ | PC → MCU | 자리마다 1바이트 (0~9) |
| 0x22 | RAND_NEXT | PC → MCU | 없음 (LEN 0) |
| 0x82 | LED_RESP | MCU → PC | 현재 색 번호 |

## 폴더

- `PC/` — C# WPF (.NET 10)
  - `Packet/` 프로토콜 상수, CRC-8 (LED02 재사용)
  - `Communication/Serial.cs` 포트 연결 · 송신 · 수신 스레드(바이트 모으기 / CRC 검사) · CRC 오류 시 NAK
  - `Models/` 결과표 한 줄(`RoundRecord`), 설정(`RunSettings`), 저장 묶음(`SavedSession`), 색 번호(`LedColor`)
  - `MainWindow.xaml(.cs)` 화면, 수신 처리, A/B 흐름, 카운트다운, 저장/되돌리기
- `Board/` — STM32 C (STM32CubeIDE, NUCLEO-H563ZI)
  - 작성한 코드는 `Core/Src/main.c`의 `USER CODE` 구역 (명령 처리, 버튼, 랜덤 숫자, LED, UART 수신 콜백)
  - `Core/Src/stm32h5xx_it.c`에 `USART3_IRQHandler` 추가

> `Drivers` 폴더는 용량 때문에 빼 두었습니다. `.ioc` 파일을 STM32CubeMX로 열어 코드를 생성하면 다시 만들어집니다.

## 저장 / 되돌리기 (LED02 방식)

- 저장하기: `settings.json`(횟수, 간격, 결과표) + `logs/yyyy-MM-dd_HH_mm_ss.csv`(결과표, 한 줄 = 한 회차)
- 되돌리기: `settings.json`을 읽어 횟수, 간격, 결과표 복원. 파일이 없거나 잘못됐으면 메시지
- 회차마다 자동 저장하지 않고 버튼을 누를 때만 동작. 파일이 다른 프로그램에서 열려 있으면 저장하지 않음

## 정해 둔 값 (명세에서 미정이던 항목)

| 항목 | 값 | 위치 |
|---|---|---|
| 카운트다운 단계 시간 | 1000 ms | PC `CountdownStepMs` |
| 응답 대기 시간 | 1000 ms | PC `ResponseTimeoutMs` |
| LED 꺼짐 시간 | 100 ms | PC `LedOffGapMs`, 보드 `LED_OFF_GAP_MS` |
| A 방식 CRC 오류(NAK) 후 | 기록 없이 실행 종료 | PC `RecordFailOnNak` |
| B 방식 성공 기준 | 보드 색 = PC 예상 색 | PC `BSuccessNeedsColorMatch` |
| 버튼 떨림 무시 | 200 ms | 보드 `BUTTON_DEBOUNCE_MS` |

## 작업 과정

1. 과제 명세와 흐름도(상세/축소 draw.io, A/B 흐름 정리)로 전체 흐름 정리
2. LED02에서 프로토콜 상수, CRC-8(`CalcCrc`만), 시리얼 송수신(`SerialLink` → `Serial`) 재사용, 새 명령(0x02, 0x20, 0x21, 0x22, 0x82) 추가
3. 수신 이벤트 구독(`PacketReceived` → `HandlePacket`)과 `Dispatcher`로 UI 스레드 처리
4. A 방식(보드 버튼 → `RAND_SEND` → 카운트다운 → `LED_WRITE` → `RAND_NEXT` 반복), B 방식(시작 버튼 → `RAND_REQ` → `LED_RESP`) 구현
5. 응답 대기는 `Thread.Sleep` 대신 `TaskCompletionSource` + `Task.WhenAny`(타임아웃 1초)
6. 보드 펌웨어: 명령 처리(`handle_packet`), USER 버튼, 랜덤 숫자, LED 제어, UART 수신 인터럽트
7. 저장/되돌리기를 LED02 방식(`settings.json`, `logs/`)으로 정리
8. LED 전환을 "껐다가 켜기"로 변경, 파일이 열려 있을 때 저장 막기

## 확인 현황

- PC: 빌드 오류 0 · 경고 0
- 보드: ARM GCC로 컴파일 · 링크 성공
- 실제 보드와 A/B 방식 통신 시험은 진행 예정
