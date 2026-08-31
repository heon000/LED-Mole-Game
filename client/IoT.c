/*
 * ======================================================================================
 * [프로젝트] 라즈베리 파이를 활용한 LED 두더지 게임
 * * [시스템 개요]
 * 1. Hardware: WiringPi GPIO(LED/버튼), SoftTone(부저), I2C(LCD) 제어
 * 2. Logic: 시간 제한, 레벨링, 콤보 시스템, 멀티플레이 동기화
 * 3. Network/DB: TCP/IP 소켓 통신 및 Python 연동 Firebase DB 저장
 * ======================================================================================
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <wiringPi.h>
#include <lcd.h>
#include <softTone.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fcntl.h>

// --------------------------------------------------------------------------------------
// [Config] 네트워크 및 하드웨어 핀 맵핑
// --------------------------------------------------------------------------------------
#define SERVER_IP ""  // 서버 주소 (환경에 맞게 수정)
#define SERVER_PORT 9000

// GPIO 핀 배열 (WiringPi Pin 번호 기준)
// 예: LedPins[0]은 실제 물리 핀 5번(GPIO 24) 등에 연결됨
const int LedPins[8] = { 5, 6, 13, 19, 26, 12, 16, 20 };
const int KeyPins[8] = { 2, 3, 4, 17, 27, 22, 10, 9 };

// LCD(I2C or 4-bit GPIO) 및 부저 설정
#define LCD_RS  18
#define LCD_E   23
#define LCD_D4  8
#define LCD_D5  7
#define LCD_D6  24
#define LCD_D7  25
#define BUZZER_PIN 21

// 메뉴 네비게이션용 키 매핑 (0~3번 버튼 사용)
#define KEY_UP    0  
#define KEY_DOWN  1  
#define KEY_ENTER 2  
#define KEY_BACK  3  

// --------------------------------------------------------------------------------------
// [Balance] 게임 난이도 및 사운드 설정
// --------------------------------------------------------------------------------------
#define GAME_TIME_LIMIT 30   // 게임 제한 시간 (초)
#define SCORE_PER_HIT   10   // 기본 점수
#define SCORE_PENALTY   10   // 오답/놓침 감점
#define COMBO_BONUS     5    // 5콤보마다 추가 점수

// 레벨별 LED 지속 시간 (ms) - 레벨이 오를수록 시간이 짧아짐
#define LV1_TIME 1500  
#define LV2_TIME 1000  
#define LV3_TIME 700   

// 효과음 주파수 (Hz)
#define TONE_LOW   262
#define TONE_HIGH  1046
#define TONE_COMBO 1250 
#define TONE_END   523
#define TONE_BEEP  440
#define TONE_FAIL  150     

int lcdHandle;
unsigned int soundOffTime = 0; // 소리를 끌 시간을 저장하는 변수

/*
 * [Sound] 비동기(Non-Blocking) 부저 제어
 * 설명: `delay()`를 사용하면 게임 로직 전체가 멈추므로, 
 * 소리를 켠 뒤 '꺼야 할 시간'만 기록하고 메인 루프에서 체크함.
 */
void stopTone() { softToneWrite(BUZZER_PIN, 0); }

void playToneNB(int freq, int duration) { 
    softToneWrite(BUZZER_PIN, freq);    // 즉시 재생 시작
    soundOffTime = millis() + duration; // 현재시간 + 재생시간 = 종료 예정 시간
}

void checkSound() { 
    // 현재 시간이 종료 예정 시간을 지났다면 소리 끔
    if (soundOffTime > 0 && millis() > soundOffTime) { 
        stopTone(); 
        soundOffTime = 0; 
    } 
}

/*
 * [Input] 버튼 비트마스킹 처리
 * 설명: 8개의 버튼 상태를 for문으로 읽어 하나의 int 변수(비트열)로 반환.
 * 원리: 0번 버튼 눌림 -> 0번째 비트 1 (00000001)
 * 2번 버튼 눌림 -> 2번째 비트 1 (00000100)
 */
int KeypadReadState() {
    int state = 0;
    for (int i = 0; i < 8; i++) { 
        // Pull-up 저항 사용: 버튼을 누르면 LOW(0) 상태가 됨
        if (digitalRead(KeyPins[i]) == LOW) 
            state |= (1 << i); // OR 연산으로 해당 비트만 1로 설정
    }
    return state;
}

// 메뉴용 키 입력 대기 (Debounce 및 Rising Edge 처리 포함)
int waitForKeyPressMenu() {
    static int prevKey = -1; // 이전 키 상태 기억 (정적 변수)
    int key = -1;
    int state = KeypadReadState();
    
    // 메뉴 조작에 사용하는 상위 4개 버튼만 확인
    for (int i = 0; i < 4; i++) { if (state & (1 << i)) { key = i; break; } }
    
    // [Edge Detection] 키가 '새로' 눌린 순간만 반환
    if (key != -1 && prevKey == -1) { prevKey = key; return key; }
    
    // 키를 떼면 상태 초기화
    if (key == -1) prevKey = -1;
    
    return -1; // 입력 없음
}

// 아무 키나 눌려있는지 확인 (단순 유무 체크)
int waitForKeyPressAny() {
    int state = KeypadReadState();
    if (state != 0) return 1;
    return 0;
}

/*
 * [DB] Firebase 점수 업로드
 * 프로세스:
 * 1. 게임 종료 후 콘솔 입력 모드를 Blocking으로 전환 (scanf 대기 위해)
 * 2. 이름 입력 및 유효성 검사 (알파벳 3글자)
 * 3. system() 함수로 Python 업로드 스크립트 실행
 */
void sendScoreToFirebase(int score) {
    char playerName[10];
    char command[200];

    lcdClear(lcdHandle);
    lcdPosition(lcdHandle, 0, 0); lcdPuts(lcdHandle, "Enter Name (3A)");
    lcdPosition(lcdHandle, 0, 1); lcdPuts(lcdHandle, "Console Ready...");

    printf("\n\n*** 게임 종료! 랭킹 등록 ***\n");
    printf("최종 점수: %d\n", score);

    // [입력 모드 변경] Non-blocking -> Blocking 복구
    int stdin_flags = fcntl(0, F_GETFL, 0);
    fcntl(0, F_SETFL, stdin_flags & ~O_NONBLOCK);
    
    // 입력 버퍼 비우기 (게임 중 연타로 들어간 엔터 제거)
    while (getchar() != '\n' && !feof(stdin));

    // [이름 입력 루프]
    while (1) {
        printf("이름을 입력하세요(영어 3글자): ");
        if (scanf("%3s", playerName) == 1 && strlen(playerName) == 3) {
            int valid = 1;
            // 알파벳인지 확인하고 대문자로 변환
            for (int i = 0; i < 3; i++) { 
                if (!isalpha(playerName[i])) { valid = 0; break; } 
                playerName[i] = toupper(playerName[i]); 
            }
            if (valid) break;
        }
        printf("알파벳 3글자로 입력하세요.\n"); while (getchar() != '\n' && !feof(stdin));
    }

    // [Python 연동] DB 업로드
    printf("--- [DB] 점수 전송 중... ---\n");
    // 예: python3 upload_score.py "ABC" 150
    sprintf(command, "python3 upload_score.py \"%s\" %d", playerName, score);
    system(command);
    printf("--- [DB] 전송 완료 ---\n");

    lcdClear(lcdHandle);
    lcdPosition(lcdHandle, 0, 0); lcdPuts(lcdHandle, "Upload Success!");
    delay(2000);
}

// [System] GPIO 및 주변장치 초기화
int initSystem() {
    if (wiringPiSetupGpio() == -1) return -1;
    
    // LED는 OUTPUT, 버튼은 INPUT(Pull-up) 설정
    for (int i = 0; i < 8; i++) {
        pinMode(LedPins[i], OUTPUT); digitalWrite(LedPins[i], LOW);
        pinMode(KeyPins[i], INPUT);  pullUpDnControl(KeyPins[i], PUD_UP);
    }
    
    if (softToneCreate(BUZZER_PIN) == -1) return -1;
    
    // LCD 초기화 (16 col, 2 row)
    lcdHandle = lcdInit(2, 16, 4, LCD_RS, LCD_E, LCD_D4, LCD_D5, LCD_D6, LCD_D7, 0, 0, 0, 0);
    if (lcdHandle < 0) return -1;
    return 0;
}

// [UI] 메뉴 구성 및 디스플레이
#define MENU_ITEM_COUNT 4
const char* menuItems[MENU_ITEM_COUNT] = { "Single Play", "Multi Play", "Ranking", "Exit" };

void displayMenu(int cursor) {
    lcdClear(lcdHandle);
    // 2줄 LCD이므로 0~1번, 2~3번 항목을 페이지 단위로 표시
    int start_idx = (cursor / 2) * 2; 
    
    lcdPosition(lcdHandle, 0, 0);
    lcdPrintf(lcdHandle, "%c %s", (cursor == start_idx ? '>' : ' '), menuItems[start_idx]);
    
    if (start_idx + 1 < MENU_ITEM_COUNT) {
        lcdPosition(lcdHandle, 0, 1);
        lcdPrintf(lcdHandle, "%c %s", (cursor == start_idx + 1 ? '>' : ' '), menuItems[start_idx + 1]);
    }
}

// 메뉴 루프: 버튼 입력에 따라 커서 이동 및 선택값 반환
int runMenu() {
    int cursor = 0;
    displayMenu(cursor);
    while (1) {
        checkSound(); // 메뉴 이동 중에도 효과음 처리를 위해 필수 호출
        int key = waitForKeyPressMenu();
        
        if (key == KEY_UP) { 
            cursor--; if (cursor < 0) cursor = MENU_ITEM_COUNT - 1; 
            displayMenu(cursor); playToneNB(TONE_BEEP, 50); 
        }
        else if (key == KEY_DOWN) { 
            cursor++; if (cursor >= MENU_ITEM_COUNT) cursor = 0; 
            displayMenu(cursor); playToneNB(TONE_BEEP, 50); 
        }
        else if (key == KEY_ENTER) { 
            playToneNB(TONE_HIGH, 100); delay(150); return cursor; 
        }
        else if (key == KEY_BACK) { 
            playToneNB(TONE_LOW, 100); delay(150); return MENU_ITEM_COUNT - 1; 
        }
        delay(10);
    }
}

// [DB] 랭킹 조회 (DB -> Python Print -> Console)
void showRanking() {
    stopTone();
    lcdClear(lcdHandle);
    lcdPosition(lcdHandle, 0, 0); lcdPuts(lcdHandle, "Loading...");
    lcdPosition(lcdHandle, 0, 1); lcdPuts(lcdHandle, "Check Console");
    
    printf("\n\n--- [DB] 랭킹 조회 ---\n");
    system("python3 get_rank.py"); // Python 스크립트가 DB에서 가져온 내용을 print함
    printf("--- 조회 완료 ---\n엔터: 메뉴 복귀\n");
    
    while (getchar() != '\n' && !feof(stdin)); getchar();
}

/*
 * ======================================================================================
 * [Game Logic] Single Play
 * 특징: 레벨 비례 난이도 상승, 콤보 시스템, 더블 스폰(2마리 동시 출현)
 * ======================================================================================
 */
void playSingleGame() {
    // 1. 변수 초기화
    int score = 0, consecutiveHits = 0;
    int currentLevel = 1, currentLifeTime = LV1_TIME;
    unsigned int lastOffTime = millis(); // 마지막 두더지가 사라진 시간
    int currentWaitTime = 1000;          // 다음 생성까지 대기 시간
    
    int activeTargets[2] = { -1, -1 };   // 현재 켜진 LED 인덱스 (최대 2마리)
    int activeCount = 0;
    int pendingSecondMole = -1;          // 시간차 공격용 대기 두더지 ID
    unsigned int secondMoleSpawnTime = 0;

    static int prev_key_state = 0;       // Edge Detection용 이전 키 상태
    int current_key_state = 0;
    unsigned int gameStartTime = millis();
    unsigned int ledOnTime = 0;
    unsigned int lastUiUpdate = 0;
    
    // LCD 깜빡임 방지용 캐싱 (값이 변할 때만 LCD 갱신)
    int lastDisplayedScore = -1, lastDisplayedTime = -1, lastDisplayedCombo = -1;

    // 2. 카운트다운
    srand(time(NULL));
    lcdClear(lcdHandle); lcdPosition(lcdHandle, 0, 0); lcdPuts(lcdHandle, "SINGLE PLAY");
    lcdPosition(lcdHandle, 0, 1); lcdPuts(lcdHandle, "READY...");
    playToneNB(440, 500); delay(1000);
    lcdClear(lcdHandle); lcdPosition(lcdHandle, 0, 0); lcdPuts(lcdHandle, "START!");
    playToneNB(880, 500); delay(500);
    lcdClear(lcdHandle);
    lastOffTime = millis();

    // 3. 메인 게임 루프
    while (1) {
        unsigned int currentMillis = millis();
        checkSound();
        current_key_state = KeypadReadState();

        // A. [Time] 제한시간 체크
        int elapsed = (currentMillis - gameStartTime) / 1000;
        if (elapsed >= GAME_TIME_LIMIT) break;
        int remainTime = GAME_TIME_LIMIT - elapsed;

        // B. [Level] 점수에 따른 난이도 상승
        if (score >= 300 && currentLevel < 3) {
            currentLevel = 3; currentLifeTime = LV3_TIME; playToneNB(TONE_HIGH + 500, 300);
        }
        else if (score >= 150 && currentLevel < 2) {
            currentLevel = 2; currentLifeTime = LV2_TIME; playToneNB(TONE_HIGH + 300, 300);
        }

        // C. [Spawn] 두더지 생성 로직
        // 필드에 두더지가 없고, 예약된 두더지도 없을 때
        if (activeCount == 0 && pendingSecondMole == -1) {
            if (currentMillis - lastOffTime >= currentWaitTime) {
                // 레벨별 더블 스폰 확률 (Lv1: 40%, Lv2: 80%, Lv3: 100%)
                int spawnChance = (currentLevel == 1) ? 4 : (currentLevel == 2 ? 8 : 10);
                int spawnTwo = (rand() % 10) < spawnChance ? 1 : 0;
                
                // 첫 번째 두더지
                activeTargets[0] = rand() % 8;
                digitalWrite(LedPins[activeTargets[0]], HIGH);
                activeCount = 1;
                ledOnTime = currentMillis;

                // 두 번째 두더지 (Double Spawn)
                if (spawnTwo) {
                    int temp = rand() % 7;
                    if (temp >= activeTargets[0]) temp++; // 첫 번째와 겹치지 않게
                    activeTargets[1] = temp;
                    activeCount = 2;
                    
                    // 시간차(Stagger) 공격 로직
                    int staggerDelay = rand() % 500;
                    if (staggerDelay < 50) {
                        digitalWrite(LedPins[activeTargets[1]], HIGH); // 즉시 켬
                        pendingSecondMole = -1;
                    }
                    else {
                        pendingSecondMole = activeTargets[1]; // 나중에 켜기로 예약
                        secondMoleSpawnTime = currentMillis + staggerDelay;
                    }
                }
                else { activeTargets[1] = -1; pendingSecondMole = -1; }
            }
        }
        else {
            // [Delayed Spawn] 예약된 두더지 켜기
            if (pendingSecondMole != -1 && currentMillis >= secondMoleSpawnTime) {
                digitalWrite(LedPins[pendingSecondMole], HIGH);
                ledOnTime = currentMillis; 
                pendingSecondMole = -1;
            }
            // [Timeout] 잡지 못하고 시간 초과 시
            if (currentMillis - ledOnTime > currentLifeTime) {
                int missCount = 0;
                for (int k = 0; k < 2; k++) {
                    if (activeTargets[k] != -1) {
                        digitalWrite(LedPins[activeTargets[k]], LOW);
                        activeTargets[k] = -1;
                        missCount++;
                    }
                }
                pendingSecondMole = -1; activeCount = 0;
                // 감점 처리
                if (missCount > 0) {
                    score -= (SCORE_PENALTY * missCount);
                    if (score < 0) score = 0;
                    consecutiveHits = 0; // 콤보 초기화
                    playToneNB(TONE_FAIL, 200);
                }
                lastOffTime = currentMillis;
                currentWaitTime = rand() % 1000 + 50; // 랜덤 대기 시간
            }
        }

        // D. [Input] 히트 판정
        for (int i = 0; i < 8; i++) {
            // Rising Edge: 지금 눌렸고(Current) && 전엔 안 눌렸음(Prev)
            if ((current_key_state & (1 << i)) && !(prev_key_state & (1 << i))) {
                int hitSuccess = 0;
                for (int k = 0; k < 2; k++) {
                    // 정답: 켜진 LED이고 && 예약 상태(아직 안 켜짐)가 아님
                    if (activeTargets[k] == i && (pendingSecondMole != i)) {
                        digitalWrite(LedPins[i], LOW);
                        activeTargets[k] = -1; activeCount--; hitSuccess = 1;
                        score += (currentLevel * 10);
                        consecutiveHits++;
                        
                        // 콤보 보너스
                        if (consecutiveHits > 0 && consecutiveHits % 5 == 0) {
                            score += COMBO_BONUS; playToneNB(TONE_COMBO, 150);
                        }
                        else { playToneNB(TONE_HIGH, 100); }
                        
                        // 다 잡았으면 바로 다음 대기 시작
                        if (activeCount == 0) {
                            lastOffTime = currentMillis;
                            currentWaitTime = rand() % 1000 + 50;
                        }
                        break;
                    }
                }
                // 오답 처리
                if (!hitSuccess) {
                    score -= SCORE_PENALTY;
                    if (score < 0) score = 0;
                    consecutiveHits = 0;
                    playToneNB(TONE_FAIL, 300);
                }
            }
        }
        prev_key_state = current_key_state;

        // E. [UI] LCD 업데이트 (200ms 주기)
        if (currentMillis - lastUiUpdate > 200) {
            if (remainTime != lastDisplayedTime || consecutiveHits != lastDisplayedCombo) {
                lcdPosition(lcdHandle, 0, 0); 
                lcdPrintf(lcdHandle, "TIME:%-3d Cmb:%-2d", remainTime, consecutiveHits);
                lastDisplayedTime = remainTime; lastDisplayedCombo = consecutiveHits;
            }
            if (score != lastDisplayedScore) {
                lcdPosition(lcdHandle, 0, 1); 
                lcdPrintf(lcdHandle, "SCORE:%-4d Lv:%d", score, currentLevel);
                lastDisplayedScore = score;
            }
            lastUiUpdate = currentMillis;
        }
        delay(1);
    }

    // 4. 종료 처리
    for (int k = 0; k < 2; k++) { if (activeTargets[k] != -1) digitalWrite(LedPins[activeTargets[k]], LOW); }
    lcdClear(lcdHandle); lcdPosition(lcdHandle, 0, 0); lcdPuts(lcdHandle, "GAME OVER!");
    lcdPosition(lcdHandle, 0, 1); lcdPrintf(lcdHandle, "SCORE: %d", score);
    sendScoreToFirebase(score);
    playToneNB(TONE_END, 800); delay(2000); lcdClear(lcdHandle);
}

/*
 * ======================================================================================
 * [Game Logic] Multi Play
 * 방식: 클라이언트는 'Input'을 서버로 보내고, 서버의 'Command'대로 LED를 켬.
 * 통신: Non-blocking Socket을 사용하여 수신 대기 중에도 루프가 끊김 없이 동작.
 * ======================================================================================
 */
void playMultiGame() {
    int sock = 0;
    struct sockaddr_in serv_addr;
    static char rbuf[2048]; // 수신 버퍼
    static int rlen = 0;    // 버퍼 내 유효 데이터 길이
    int my_score = 0, op_score = 0;
    int active_led = -1;
    int prev_key_state_multi = 0;
    int game_running = 1;
    unsigned int game_start_time = 0;

    // 1. 소켓 연결
    lcdClear(lcdHandle);
    lcdPosition(lcdHandle, 0, 0); lcdPuts(lcdHandle, "Connecting...");
    
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) { return; }
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(SERVER_PORT);
    if (inet_pton(AF_INET, SERVER_IP, &serv_addr.sin_addr) <= 0) { return; }
    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) { return; }

    // 소켓을 Non-Blocking 모드로 전환
    // recv() 호출 시 데이터가 없으면 차단되지 않고 -1을 반환하며 즉시 복귀됨
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    memset(rbuf, 0, sizeof(rbuf));

    // 2. 대기실 (Lobby)
    lcdClear(lcdHandle);
    lcdPosition(lcdHandle, 0, 0); lcdPuts(lcdHandle, "Connected!");
    lcdPosition(lcdHandle, 0, 1); lcdPuts(lcdHandle, "Wait/Press SEL");

    int is_started = 0;
    int back_pressed = 0;

    while (!is_started) {
        checkSound();
        // 데이터 수신
        int n = recv(sock, rbuf + rlen, sizeof(rbuf) - 1 - rlen, 0);
        
        if (n > 0) {
            rlen += n; rbuf[rlen] = '\0';
            char* line_start = rbuf;
            char* newline_pos;
            
            // [Sticky Packet 처리] 개행 문자(\n) 단위로 명령어를 자름
            while ((newline_pos = strchr(line_start, '\n')) != NULL) {
                *newline_pos = '\0'; // \n을 NULL로 변경하여 문자열 끊기
                char* msg = line_start;
                
                // 명령어 파싱: WAITING <인원> / START
                if (strstr(msg, "WAITING")) {
                    int curr_cnt;
                    sscanf(strstr(msg, "WAITING") + 8, "%d", &curr_cnt);
                    lcdPosition(lcdHandle, 0, 0); lcdPrintf(lcdHandle, "Lobby: %d Users  ", curr_cnt);
                }
                else if (strstr(msg, "START")) { is_started = 1; }
                
                line_start = newline_pos + 1;
                if (is_started) break;
            }
            // 처리하고 남은 데이터는 버퍼 앞으로 이동 (memmove)
            int remaining = rlen - (line_start - rbuf);
            if (remaining > 0) memmove(rbuf, line_start, remaining);
            rlen = remaining; rbuf[rlen] = '\0';
            
            if (is_started) break;
        }
        
        // 시작 요청(ENTER) 또는 종료(BACK)
        int key = waitForKeyPressMenu();
        if (key == KEY_ENTER) {
            playToneNB(TONE_HIGH, 100);
            send(sock, "REQ_START\n", 10, 0); // 서버에 시작 의사 전송
            lcdPosition(lcdHandle, 0, 1); lcdPuts(lcdHandle, "Requesting...   ");
        }
        else if (key == KEY_BACK) { back_pressed = 1; break; }
        delay(50);
    }
    if (back_pressed) { close(sock); return; }

    // 3. 게임 플레이 루프
    game_start_time = millis();
    unsigned int last_ui_update = 0;

    while (game_running) {
        checkSound();
        // A. [Recv] 서버 명령어 수신
        int n = recv(sock, rbuf + rlen, sizeof(rbuf) - 1 - rlen, 0);
        if (n == 0) break; // 서버 연결 끊김
        if (n > 0) { rlen += n; rbuf[rlen] = '\0'; }

        if (rlen > 0) {
            char* line_start = rbuf;
            char* newline_pos;
            while ((newline_pos = strchr(line_start, '\n')) != NULL) {
                *newline_pos = '\0';
                char* msg = line_start;

                // [Protocol] 명령어 처리
                if (strcmp(msg, "START") == 0) {
                    lcdClear(lcdHandle); lcdPuts(lcdHandle, "GO! GO! GO!");
                    game_start_time = millis();
                }
                else if (strncmp(msg, "LED ", 4) == 0) { // LED 켜기 (LED <idx>)
                    int led_idx;
                    if (sscanf(msg + 4, "%d", &led_idx) == 1) {
                        if (active_led != -1) digitalWrite(LedPins[active_led], LOW);
                        digitalWrite(LedPins[led_idx], HIGH);
                        active_led = led_idx;
                    }
                }
                else if (strncmp(msg, "OFF ", 4) == 0) { // LED 끄기
                    int led_idx;
                    sscanf(msg + 4, "%d", &led_idx);
                    if (active_led == led_idx) { digitalWrite(LedPins[active_led], LOW); active_led = -1; }
                }
                else if (strncmp(msg, "SCORE ", 6) == 0) { // 점수 동기화
                    sscanf(msg + 6, "%d %d", &my_score, &op_score);
                }
                else if (strncmp(msg, "GAMEOVER ", 9) == 0) { // 게임 종료 및 결과
                    stopTone();
                    char winner_name[20];
                    int winner_score, my_rank, my_final = 0;
                    sscanf(msg + 9, "%s %d %d %d", winner_name, &winner_score, &my_rank, &my_final);
                    lcdClear(lcdHandle);
                    lcdPosition(lcdHandle, 0, 0); lcdPrintf(lcdHandle, "1st:%s(%d)", winner_name, winner_score);
                    lcdPosition(lcdHandle, 0, 1); lcdPrintf(lcdHandle, "Me:No.%d(%d)", my_rank, my_final);
                    my_score = my_final;
                    game_running = 0;
                }
                line_start = newline_pos + 1;
            }
            int remaining = rlen - (line_start - rbuf);
            if (remaining > 0) memmove(rbuf, line_start, remaining);
            rlen = remaining; rbuf[rlen] = '\0';
        }

        // B. [Send] 내 입력 전송
        int current_key = KeypadReadState();
        for (int i = 0; i < 8; i++) {
            // 내가 버튼을 누르면 서버로 'HIT <번호>' 전송
            if ((current_key & (1 << i)) && !(prev_key_state_multi & (1 << i))) {
                char msg[32];
                sprintf(msg, "HIT %d\n", i);
                send(sock, msg, strlen(msg), 0);
                
                // 로컬 피드백 (반응 속도를 위해 소리는 여기서 즉시 재생)
                if (i == active_led) {
                    digitalWrite(LedPins[active_led], LOW); active_led = -1;
                    playToneNB(TONE_HIGH, 100);
                } else { playToneNB(TONE_FAIL, 100); }
            }
        }
        prev_key_state_multi = current_key;

        // C. [UI] 점수 및 시간 업데이트
        if (millis() - last_ui_update > 200) {
            int elapsed = (millis() - game_start_time) / 1000;
            int remain = GAME_TIME_LIMIT - elapsed; if (remain < 0) remain = 0;
            lcdPosition(lcdHandle, 0, 0); lcdPrintf(lcdHandle, "Me:%-2d Top:%-2d  ", my_score, op_score);
            lcdPosition(lcdHandle, 0, 1); lcdPrintf(lcdHandle, "Time: %-3d sec  ", remain);
            last_ui_update = millis();
        }
        delay(10);
    }

    // 4. 종료 및 이름 전송
    if (active_led != -1) digitalWrite(LedPins[active_led], LOW);
    playToneNB(TONE_END, 1000); delay(100); stopTone();

    // Blocking 모드 복구 및 이름 입력
    int stdin_flags = fcntl(0, F_GETFL, 0);
    fcntl(0, F_SETFL, stdin_flags & ~O_NONBLOCK);
    
    char playerName[10] = "UNK";
    printf("\n\n*** Multi Play 종료! 랭킹 등록 ***\n");
    while (1) {
        printf("이름을 입력하세요(영어 3글자): ");
        if (scanf("%3s", playerName) == 1 && strlen(playerName) == 3) { break; }
        while(getchar() != '\n');
    }

    // 서버로 이름을 보내면 서버가 DB에 저장함
    char nameMsg[32];
    sprintf(nameMsg, "NAME %s\n", playerName);
    send(sock, nameMsg, strlen(nameMsg), 0);
    
    lcdClear(lcdHandle); lcdPuts(lcdHandle, "Sent Name!");
    delay(2000); close(sock); lcdClear(lcdHandle);
}

// [Main] 프로그램 진입점
int main(void) {
    if (initSystem() == -1) { printf("Init Failed!\n"); return 1; }
    printf("System Ready.\n");
    
    while (1) {
        int selection = runMenu();
        if (selection == 0) { stopTone(); playSingleGame(); }
        else if (selection == 1) { stopTone(); playMultiGame(); }
        else if (selection == 2) { stopTone(); showRanking(); }
        else if (selection == 3) { 
            lcdClear(lcdHandle); lcdPuts(lcdHandle, "Goodbye!"); 
            delay(1000); lcdClear(lcdHandle); break; 
        }
    }
    return 0;
}
