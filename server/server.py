import socket
import threading
import time
import random

# ==========================================
# 1. 게임 설정 및 상수 정의
# ==========================================
PORT = 9000           # 서버 포트 번호
GAME_DURATION = 30    # 게임 한 판당 제한 시간 (초)

SCORE_PER_HIT = 10    # 기본 점수
SCORE_PENALTY = 10    # 오답/미입력 시 감점 점수
COMBO_BONUS = 5       # 5콤보마다 추가 점수

# 레벨별 두더지 생존 시간 (난이도 조절)
LV1_TIME = 1.5
LV2_TIME = 1.0
LV3_TIME = 0.7

# ==========================================
# 2. 전역 변수 선언 (게임 상태 관리)
# ==========================================
players = []       # 현재 접속한 클라이언트(소켓) 리스트
scores = {}        # 플레이어별 점수 {소켓: 점수}
player_names = {}  # 플레이어 식별 이름 (P1, P2...)

# 게임 실시간 상태 변수들
combos = {}        # 현재 콤보 수
levels = {}        # 현재 레벨 (1~3)
lifetimes = {}     # 현재 레벨에 따른 두더지 수명
current_moles = {} # 현재 켜져 있는 LED(두더지) 번호 리스트
mole_spawn_time = {} # 두더지가 튀어나온 시각 (타임아웃 계산용)
next_mole_time = {}  # 다음 두더지가 나올 시간

game_running = False # 게임 진행 여부 플래그
accepting = True     # 클라이언트 접속 허용 여부 플래그
score_lock = threading.RLock() # 스레드 간 데이터 충돌 방지를 위한 락(Lock)

# ==========================================
# 3. 기능 함수 정의
# ==========================================

# [기능] 점수 브로드캐스팅 (내 점수와 1등 점수를 클라이언트에 전송)
def broadcast_smart_scores():
    with score_lock: # 데이터 보호 시작
        for conn in list(players):
            if conn not in scores: continue
            
            my_s = scores[conn] # 내 점수
            
            # 나를 제외한 다른 사람들의 점수 리스트
            others = [s for c, s in scores.items() if c != conn]
            # 라이벌(1등) 점수 계산 (없으면 0점)
            top_rival = max(others) if others else 0
            
            try:
                # 클라이언트로 전송: "SCORE 내점수 1등점수"
                conn.sendall(f"SCORE {my_s} {top_rival}\n".encode())
            except: pass

# [기능] 클라이언트별 입력 처리 스레드 (점수 획득/감점 로직)
def handle_client(conn):
    global scores, combos, levels, lifetimes, current_moles
    conn.setblocking(False) # 논블로킹 모드 설정 (데이터 없어도 멈추지 않음)
    buffer = "" # 데이터 조각 모음용 버퍼
    
    while game_running:
        try:
            # 데이터 수신 (최대 1024바이트)
            data = conn.recv(1024).decode()
            if not data: break # 연결 끊김
            buffer += data
            
            # 버퍼가 너무 커지면 비움 (메모리 보호)
            if len(buffer) > 2048: buffer = "" 

            # 줄바꿈(\n) 기준으로 메시지 처리 (TCP 패킷 뭉침 방지)
            while "\n" in buffer:
                msg, buffer = buffer.split("\n", 1)
                
                # [입력 처리] 버튼을 눌렀을 때 ("HIT 번호")
                if game_running and msg.startswith("HIT"):
                    _, key_str = msg.split()
                    key = int(key_str)
                    
                    with score_lock: # 점수 계산 중 데이터 보호
                        if conn in scores:
                            # 1) 정답 로직 (켜진 두더지를 때림)
                            if key in current_moles.get(conn, []):
                                # 점수 증가 (레벨에 따라 가중치)
                                scores[conn] += (levels[conn] * 10)
                                combos[conn] += 1
                                
                                # 콤보 보너스 (5회마다)
                                if combos[conn] > 0 and combos[conn] % 5 == 0:
                                    scores[conn] += COMBO_BONUS
                                
                                # 레벨 업 시스템 (점수에 따라 난이도 상승)
                                if scores[conn] >= 300 and levels[conn] < 3:
                                    levels[conn] = 3; lifetimes[conn] = LV3_TIME
                                elif scores[conn] >= 150 and levels[conn] < 2:
                                    levels[conn] = 2; lifetimes[conn] = LV2_TIME
                                    
                                # 맞춘 두더지 제거 및 LED 끄기 신호 전송
                                current_moles[conn].remove(key)
                                try: conn.sendall(f"OFF {key}\n".encode()) 
                                except: pass
                                print(f"[HIT] Score: {scores[conn]} (Lv:{levels[conn]})")
                                
                            # 2) 오답 로직 (엉뚱한 곳을 때림)
                            else:
                                scores[conn] -= SCORE_PENALTY
                                if scores[conn] < 0: scores[conn] = 0 # 0점 미만 방지
                                combos[conn] = 0 # 콤보 초기화
                                print(f"[MISS] Penalty! Score: {scores[conn]}")

                    # 변경된 점수 전체 전송
                    broadcast_smart_scores()
                    
        except BlockingIOError:
            time.sleep(0.01) # 데이터가 없으면 잠시 대기 (CPU 과부하 방지)
        except Exception:
            break # 에러 발생 시 루프 종료

# [기능] 전체 공지 메시지 전송
def broadcast(msg):
    for conn in list(players):
        try: conn.sendall((msg + "\n").encode())
        except: 
            # 전송 실패 시 연결 끊긴 것으로 간주하고 제거
            if conn in players: players.remove(conn)

# ==========================================
# 4. 핵심 게임 로직 (메인 루프)
# ==========================================
def start_game_logic():
    global game_running
    
    print(f"\n>>> 총 {len(players)}명 참가! 3초 후 시작!")

    time.sleep(3)
    broadcast("START") # 클라이언트에 시작 신호 전송
    game_running = True
    
    # 게임 시작 전 상태 초기화
    with score_lock:
        scores.clear(); player_names.clear()
        combos.clear(); levels.clear(); lifetimes.clear()
        current_moles.clear(); next_mole_time.clear(); mole_spawn_time.clear()

        # 각 플레이어별 초기값 설정 및 수신 스레드 시작
        for i, conn in enumerate(players):
            scores[conn] = 0
            player_names[conn] = f"P{i+1}" 
            combos[conn] = 0
            levels[conn] = 1
            lifetimes[conn] = LV1_TIME
            current_moles[conn] = []
            next_mole_time[conn] = 0
            mole_spawn_time[conn] = 0
            # 각 플레이어의 입력을 담당할 스레드 생성 (Daemon=True: 메인 종료시 함께 종료)
            threading.Thread(target=handle_client, args=(conn,), daemon=True).start()
    
    start_time = time.time()
    
    # [게임 루프] 시간 내 계속 반복
    while game_running:
        now = time.time()
        # 제한 시간 종료 체크
        if now - start_time > GAME_DURATION:
            game_running = False
            break
            
        current_players = list(players)
        for conn in current_players:
            # 1) 두더지 생성 로직 (현재 두더지가 없을 때)
            if not current_moles.get(conn):
                if now > next_mole_time.get(conn, 0): # 쿨타임 확인
                    # 레벨별 난이도(엇박자 확률) 설정
                    spawn_chance = 0
                    if levels[conn] == 1: spawn_chance = 40
                    elif levels[conn] == 2: spawn_chance = 80
                    else: spawn_chance = 100
                    
                    led1 = random.randint(0, 7) # 첫 번째 두더지 위치
                    new_moles = [led1]
                    
                    # '엇박자' 구현 (확률적으로 2마리가 시간차를 두고 나옴)
                    if random.randint(0, 99) < spawn_chance:
                        led2 = random.randint(0, 7)
                        if led2 == led1: led2 = (led1 + 1) % 8 # 위치 중복 방지
                        
                        try:
                            # 첫 번째 켜고 -> 시간차 -> 두 번째 켬
                            conn.sendall(f"LED {led1}\n".encode())
                            time.sleep(random.uniform(0.05, 0.2)) 
                            new_moles.append(led2)
                            conn.sendall(f"LED {led2}\n".encode())
                        except: pass
                    else:
                        # 한 마리만 생성
                        try: conn.sendall(f"LED {led1}\n".encode())
                        except: pass
                    
                    # 생성 정보 저장
                    with score_lock:
                        current_moles[conn] = new_moles
                        mole_spawn_time[conn] = now
            
            # 2) 타임아웃 로직 (두더지가 들어갈 때까지 못 잡음)
            else:
                if (now - mole_spawn_time[conn]) > lifetimes[conn]: # 수명 초과
                    with score_lock:
                        miss_count = len(current_moles[conn])
                        # 놓친 만큼 감점
                        if miss_count > 0:
                            scores[conn] -= (SCORE_PENALTY * miss_count)
                            if scores[conn] < 0: scores[conn] = 0
                        
                        combos[conn] = 0 # 콤보 끊김
                        current_moles[conn] = [] # 두더지 초기화
                        broadcast_smart_scores() # 점수 갱신
                    
                    # 다음 두더지 나올 때까지 랜덤 휴식 시간
                    next_mole_time[conn] = now + random.uniform(0.5, 1.0)
        
        time.sleep(0.05) # 루프 속도 조절
        
    if not scores: return

    # ==========================================
    # 5. 게임 종료 및 결과 처리
    # ==========================================
    with score_lock:
        # 점수 기준 내림차순 정렬 (랭킹 산정)
        sorted_ranking = sorted(scores.items(), key=lambda item: item[1], reverse=True)
        
        # 1등 정보 추출
        winner_conn, winner_score = sorted_ranking[0]
        winner_name = player_names.get(winner_conn, "Unknown")
        
        print(f"\n>>> GAME OVER! 1st: {winner_name} ({winner_score} pts)")

        # 각 클라이언트에게 최종 결과 전송 (포맷: 1등정보 + 내정보)
        for rank, (conn, score) in enumerate(sorted_ranking, start=1):
            try:
                msg = f"GAMEOVER {winner_name} {winner_score} {rank} {score}\n"
                conn.sendall(msg.encode())
            except: pass

    print("[System] 게임 종료. 대기실로 복귀합니다...")
    
    # 소켓 정리 (연결 끊기)
    print("[System] 소켓 정리 중...")
    for p in list(players):
        try: p.close()
        except: pass
    players.clear()

# ==========================================
# 6. 서버 연결 관리 함수들
# ==========================================

# [기능] 클라이언트 접속 대기
def accept_clients(server):
    server.settimeout(0.5) # 타임아웃 설정 (루프를 돌기 위함)
    while accepting:
        try:
            conn, addr = server.accept() # 접속 요청 수락
            conn.setblocking(False)      # 논블로킹 설정
            players.append(conn)         # 플레이어 목록에 추가
            print(f"[접속] {addr[0]}")
            
            # 현재 대기 인원수 전송
            cnt = len(players)
            msg = f"WAITING {cnt}\n"
            for p in players:
                try: p.sendall(msg.encode())
                except: pass
        except socket.timeout: continue # 타임아웃이면 다시 대기
        except: break

# [기능] 모든 플레이어의 준비(REQ_START) 신호 대기
def wait_for_start_signal():
    print(">>> 시작 신호(SELECT) 대기 중...")
    
    # 준비 완료한 플레이어를 기록할 집합(Set)
    ready_set = set() 

    while True:
        # 접속자가 없으면 대기
        if len(players) == 0:
            time.sleep(0.1)
            continue

        for conn in list(players):
            try:
                # 데이터 수신 확인
                data = conn.recv(1024).decode()
                
                # "REQ_START" 메시지가 오면 준비 완료 목록에 추가
                if "REQ_START" in data:
                    if conn not in ready_set:
                        ready_set.add(conn)
                        print(f"[Ready] {len(ready_set)} / {len(players)} 명 준비 완료")
                        
            except BlockingIOError: 
                pass # 데이터 없으면 패스
            except:
                # 연결 끊김 처리
                if conn in players: players.remove(conn)
                if conn in ready_set: ready_set.remove(conn)

        # [시작 조건] 접속자 수 == 준비 완료자 수
        if len(players) > 0 and len(ready_set) == len(players):
            print(f"[Start] 모든 플레이어({len(players)}명) 준비 완료! 게임 시작!")
            return True
            
        time.sleep(0.1)

# ==========================================
# 7. 메인 실행부
# ==========================================
if __name__ == '__main__':
    # TCP/IP 소켓 생성
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    # 포트 재사용 옵션 (서버 재실행 시 에러 방지)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    # IP와 포트 바인딩 (0.0.0.0은 모든 IP에서의 접속 허용)
    server.bind(('0.0.0.0', PORT))
    server.listen(10) # 접속 대기열 크기
    
    print(f"[서버 오픈] IP: {socket.gethostbyname(socket.gethostname())}")
    
    # 서버 무한 루프 (게임 판 수 반복)
    while True:
        game_running = False
        accepting = True
        
        # 접속자 받는 스레드 별도 실행
        accept_thread = threading.Thread(target=accept_clients, args=(server,), daemon=True)
        accept_thread.start()
        
        # 모든 인원이 준비될 때까지 대기 (블로킹)
        wait_for_start_signal()
        
        accepting = False # 게임 시작하면 더 이상 접속 안 받음
        accept_thread.join() # 접속 스레드 종료 대기
        
        if len(players) < 1: 
            print("참가자가 없어 대기실로 돌아갑니다.")
        else: 
            start_game_logic() # 게임 로직 시작
            
        print(">>> 한 판 종료. 대기실 재오픈 준비 중...\n")
        time.sleep(2)
        
    server.close()
