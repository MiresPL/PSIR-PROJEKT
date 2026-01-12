#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>

// --- KONFIGURACJA ---
#define PORT 8000
#define NODE_COUNT 4
#define GRID_SIZE 40        
#define NODE_GRID_SIZE 20   
#define CHUNK_SIZE 10       // 1 znak na raz dla precyzji (można zwiększyć)
#define MAX_RETRIES 30      // Było 5 -> dajmy 20
#define TIMEOUT_USEC 1000000 // 300ms timeout

#define MAX_L_SYSTEM_SIZE 1000000 
#define MY_PI 3.14159265358979323846

#define ALP_VERSION 1
#define MSG_REGISTER 0x1
#define MSG_ASSIGN   0x2
#define MSG_DATA     0x3
#define MSG_ACK      0x4
#define MSG_REQUEST  0x5
#define MSG_RESPONSE 0x6
#define MSG_HANDOVER 0x7
#define MSG_REQ_COORDS  0x8
#define MSG_RESP_COORDS 0x9

#define ORIGIN_NODE_ID 1

typedef struct {
    uint8_t id;             
    struct sockaddr_in addr;
    int active;
} Node;

typedef struct {
    char axiom[256];
    char ruleF[256];
    char ruleX[256];
    char ruleY[256];
    int iterations;
    double start_x;
    double start_y;
    double angle_deg; 
    double step;      
} LSystemConfig;

Node nodes[NODE_COUNT];
char global_grid[GRID_SIZE][GRID_SIZE];
uint8_t global_seq = 0;
LSystemConfig config; 
char *gen_current = NULL;
char *gen_next = NULL;

// --- NARZĘDZIA SIECIOWE ---

uint8_t calc_crc(uint8_t *buf, int len) {
    uint8_t crc = 0; 
    for(int i=0; i<len; i++) crc += buf[i]; 
    return crc;
}

void pack_header(uint8_t *buf, int type, uint8_t seq, uint8_t nid, int len) {
    buf[0] = (ALP_VERSION << 4) | (type & 0x0F);
    buf[1] = seq; 
    buf[2] = nid;
    buf[3] = (len >> 8) & 0xFF; 
    buf[4] = len & 0xFF;
}

// Opróżnianie bufora wejściowego (śmieci z sieci)
void flush_socket(int sock) {
    char dummy[1024];
    struct sockaddr_in from;
    socklen_t flen = sizeof(from);
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    while (recvfrom(sock, dummy, sizeof(dummy), 0, (struct sockaddr*)&from, &flen) > 0);
    fcntl(sock, F_SETFL, flags);
}

// UNIWERSALNA FUNKCJA NIEZAWODNEGO WYSYŁANIA (Stop-and-Wait)
// Zwraca: długość odebranych danych w buf lub -1 jeśli błąd
int send_reliable(int sock, int node_idx, uint8_t *packet, int packet_len, 
                  int expected_type, uint8_t *recv_buf, int recv_buf_max) {
    
    struct sockaddr_in from;
    socklen_t flen = sizeof(from);
    struct timeval tv;
    int target_id = nodes[node_idx].id;

    for(int attempt = 0; attempt < MAX_RETRIES; attempt++) {
        // 1. Wyślij
        sendto(sock, packet, packet_len, 0, 
               (struct sockaddr*)&nodes[node_idx].addr, sizeof(nodes[node_idx].addr));

        // 2. Czekaj na odpowiedź (timeout)
        tv.tv_sec = 0;
        tv.tv_usec = TIMEOUT_USEC;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        while(1) {
            int n = recvfrom(sock, recv_buf, recv_buf_max, 0, (struct sockaddr*)&from, &flen);
            if (n < 0) break; // Timeout lub błąd

            int type = recv_buf[0] & 0x0F;
            uint8_t seq = recv_buf[1]; // Można sprawdzać seq, ale w prostym stop-wait wystarczy type
            uint8_t nid = recv_buf[2];

            // Czy to odpowiedź od tego noda i tego typu co chcemy?
            // Uwaga: MSG_HANDOVER przychodzi jako odpowiedź na DATA
            if (type == expected_type) { // Tutaj można dodać sprawdzanie NID jeśli node je odsyła
                return n; // SUKCES
            }
        }
        printf("WARN: Node %d no response (attempt %d/%d). Retrying...\n", target_id, attempt+1, MAX_RETRIES);
    }
    printf("ERROR: Node %d unreachable after retries.\n", target_id);
    return -1;
}

// --- LOGIKA APLIKACJI ---

int get_node_index(double x, double y) {
    int col = (int)(x / 20.0);
    int row = (int)(y / 20.0);
    if (col < 0) col = 0; if (col > 1) col = 1;
    if (row < 0) row = 0; if (row > 1) row = 1;
    return row * 2 + col;
}

void fetch_origin_coordinates(int sock) {
    int node_idx = get_node_index(config.start_x, config.start_y);
    
    if(nodes[node_idx].active == 0) {
        printf("WARN: Origin Node (Index %d) determined from config is NOT active! Using defaults.\n", node_idx);
        return;
    }

    int target_id = nodes[node_idx].id;
    printf("Config start=(%.1f, %.1f). Selected Origin Node: %d\n", 
           config.start_x, config.start_y, target_id);
    
    printf("Fetching sensor data from Node %d...\n", target_id);

    uint8_t req[6];
    uint8_t buf[256];
    global_seq++;

    // Budujemy pakiet żądania MSG_REQ_COORDS (0x8)
    pack_header(req, MSG_REQ_COORDS, global_seq, target_id, 0);
    req[5] = calc_crc(req, 5); 

    // Wysyłamy do wyliczonego node_idx
    int n = send_reliable(sock, node_idx, req, 6, MSG_RESP_COORDS, buf, sizeof(buf));

    if (n > 0) {
        // Payload: [TEMP_H, TEMP_L, HUM_H, HUM_L]
        uint16_t raw_temp = (buf[5] << 8) | buf[6];
        uint16_t raw_hum  = (buf[7] << 8) | buf[8];

        printf("Received RAW sensor data: Temp=%d, Hum=%d\n", raw_temp, raw_hum);

        // Nadpisujemy współrzędne startowe danymi z czujników
        // Analog (0-1023) -> Grid (0.0 - 40.0)
        config.start_x = ((double)raw_temp / 1023.0) * (double)GRID_SIZE;
        config.start_y = ((double)raw_hum  / 1023.0) * (double)GRID_SIZE;

        // Zabezpieczenie krawędzi
        if(config.start_x >= GRID_SIZE) config.start_x = GRID_SIZE - 0.1;
        if(config.start_y >= GRID_SIZE) config.start_y = GRID_SIZE - 0.1;

        printf("UPDATED START COORDS from sensors: X=%.2f, Y=%.2f\n", config.start_x, config.start_y);
    } else {
        printf("ERROR: Failed to fetch coords from Origin Node %d. Keeping input.txt defaults.\n", target_id);
    }
}

void load_config() {
    // Domyślne wartości
    strcpy(config.axiom, "F-F-F-F");
    strcpy(config.ruleF, "F-F+F+FF-F-F+F"); 
    strcpy(config.ruleX, "");
    strcpy(config.ruleY, "");
    config.iterations = 2;
    config.start_x = 20.0;
    config.start_y = 20.0;
    config.angle_deg = 90.0;
    config.step = 1.0;

    FILE *f = fopen("input.txt", "r");
    if(!f) {
        printf("WARN: No input.txt, using defaults.\n");
        return;
    }
    printf("Loading input.txt...\n");
    char line[512];
    while(fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        
        if(strncmp(line, "axiom:", 6) == 0) sscanf(line+6, " %s", config.axiom);
        else if(strncmp(line, "iterations:", 11) == 0) sscanf(line+11, " %d", &config.iterations);
        else if(strncmp(line, "start_x:", 8) == 0) sscanf(line+8, " %lf", &config.start_x);
        else if(strncmp(line, "start_y:", 8) == 0) sscanf(line+8, " %lf", &config.start_y);
        else if(strncmp(line, "angle:", 6) == 0) sscanf(line+6, " %lf", &config.angle_deg);
        else if(strncmp(line, "step:", 5) == 0) sscanf(line+5, " %lf", &config.step);
        
        if(strncmp(line, "ruleF:", 6) == 0) {
            char *p = strchr(line, ':'); if(p) strcpy(config.ruleF, p+1);
            // Trim leading spaces
            while(config.ruleF[0] == ' ') memmove(config.ruleF, config.ruleF+1, strlen(config.ruleF));
        }
        // ... (analogicznie dla X i Y jeśli potrzebne)
    }
    fclose(f);
    printf("Config: Axiom='%s', Iters=%d, Start=%.1f,%.1f, Angle=%.1f\n", 
           config.axiom, config.iterations, config.start_x, config.start_y, config.angle_deg);
}

void generate_lsystem() {
    if(gen_current) free(gen_current);
    if(gen_next) free(gen_next);
    
    gen_current = (char*)malloc(MAX_L_SYSTEM_SIZE);
    gen_next = (char*)malloc(MAX_L_SYSTEM_SIZE);
    
    if(!gen_current || !gen_next) { printf("Alloc fail\n"); exit(1); }

    strcpy(gen_current, config.axiom);

    for(int k=0; k<config.iterations; k++) {
        int len = 0;
        int overflow = 0;
        
        for(int i=0; gen_current[i]; i++) {
            char c = gen_current[i];
            char *replacement = NULL;
            
            if(c == 'F' && strlen(config.ruleF) > 0) replacement = config.ruleF;
            else if(c == 'X' && strlen(config.ruleX) > 0) replacement = config.ruleX;
            else if(c == 'Y' && strlen(config.ruleY) > 0) replacement = config.ruleY;
            
            if(replacement) {
                int r_len = strlen(replacement);
                if(len + r_len >= MAX_L_SYSTEM_SIZE - 1) { overflow = 1; break; }
                memcpy(&gen_next[len], replacement, r_len);
                len += r_len;
            } else {
                if(len + 1 >= MAX_L_SYSTEM_SIZE - 1) { overflow = 1; break; }
                gen_next[len++] = c;
            }
        }
        gen_next[len] = 0;

        if(overflow) {
            printf("L-System too large! Stopping at iter %d\n", k);
            break;
        }
        strcpy(gen_current, gen_next);
        printf("Iteration %d length: %ld\n", k+1, strlen(gen_current));
    }
}

void run_simulation(int sock) {
    char *full_string = gen_current;
    int cursor = 0;
    int total_len = strlen(full_string);
    if(total_len == 0) return;

    double cur_x = config.start_x;
    double cur_y = config.start_y;
    double cur_angle = 0.0; 

    uint8_t buf[1024];

    printf("Starting simulation...\n");
    flush_socket(sock); 

    int steps_done = 0;

    while (cursor < total_len) {
        int node_idx = get_node_index(cur_x, cur_y);
        int target_id = nodes[node_idx].id;

        int chunk_len = CHUNK_SIZE; 
        
        global_seq++;
        uint8_t packet[512];
        int payload_len = 6 + chunk_len; 
        
        pack_header(packet, MSG_DATA, global_seq, target_id, payload_len);

        int16_t sx = (int16_t)(cur_x * 100);
        int16_t sy = (int16_t)(cur_y * 100);
        int16_t sa = (int16_t)(cur_angle);

        packet[5] = (sx >> 8) & 0xFF; packet[6] = sx & 0xFF;
        packet[7] = (sy >> 8) & 0xFF; packet[8] = sy & 0xFF;
        packet[9] = (sa >> 8) & 0xFF; packet[10] = sa & 0xFF;

        memcpy(&packet[11], &full_string[cursor], chunk_len);
        packet[11+chunk_len] = calc_crc(packet, 11+chunk_len);

        // --- NIEZAWODNE WYSYŁANIE CHUNKA ---
        // Oczekujemy MSG_HANDOVER jako potwierdzenia wykonania ruchu
        int n = send_reliable(sock, node_idx, packet, 11+chunk_len+1, MSG_HANDOVER, buf, sizeof(buf));
        
        if (n > 0) {
            // Sukces - odczytujemy nową pozycję z Handover
            int16_t nx = (buf[5] << 8) | buf[6];
            int16_t ny = (buf[7] << 8) | buf[8];
            int16_t na = (buf[9] << 8) | buf[10];

            cur_x = nx / 100.0;
            cur_y = ny / 100.0;
            cur_angle = (double)na;
            
            cursor += chunk_len;
            steps_done++;
            if(steps_done % 10 == 0) { printf("\rStep %d/%d", steps_done, total_len); fflush(stdout); }
        } else {
            printf("\nCRITICAL ERROR: Lost connection with Node %d. Aborting.\n", target_id);
            break;
        }
    }
    printf("\nSimulation finished.\n");
}

void collect_results(int sock) {
    printf("Requesting results...\n");
    memset(global_grid, '.', sizeof(global_grid));
    flush_socket(sock); 
    
    uint8_t buf[1200]; 

    for(int i=0; i<NODE_COUNT; i++) {
        uint8_t req[6]; 
        global_seq++;
        pack_header(req, MSG_REQUEST, global_seq, nodes[i].id, 0);
        req[5] = calc_crc(req, 5);
        
        // --- NIEZAWODNE POBIERANIE WYNIKU ---
        int n = send_reliable(sock, i, req, 6, MSG_RESPONSE, buf, sizeof(buf));
        
        if(n > 0) {
            int idx = nodes[i].id - 1;
            int off_x = (idx % 2) * NODE_GRID_SIZE;
            int off_y = (idx / 2) * NODE_GRID_SIZE;

            int ptr = 5; // Payload start
            for(int y=0; y<NODE_GRID_SIZE; y++) {
                for(int x=0; x<NODE_GRID_SIZE; x++) {
                    if(buf[ptr] == '#') global_grid[off_y + y][off_x + x] = '#';
                    ptr++;
                }
            }
            printf("Node %d data merged.\n", nodes[i].id);
        }
    }
}

int main() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in saddr, caddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET; 
    saddr.sin_port = htons(PORT);
    saddr.sin_addr.s_addr = INADDR_ANY;
    
    if(bind(sock, (struct sockaddr*)&saddr, sizeof(saddr)) < 0) {
        perror("Bind failed"); return 1;
    }

    load_config(); 

    printf("=== SERVER STARTED ===\nWaiting for %d nodes...\n", NODE_COUNT);

    socklen_t clen = sizeof(caddr); 
    uint8_t buf[256];
    int reg_cnt = 0;
    
    // Reset aktywności
    for(int i=0; i<NODE_COUNT; i++) nodes[i].active = 0;

    // Faza REJESTRACJI
    while(reg_cnt < NODE_COUNT) {
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr*)&caddr, &clen);
        if (n > 0 && (buf[0] & 0x0F) == MSG_REGISTER) {
            int nid = buf[2];
            if(nid >= 1 && nid <= NODE_COUNT && nodes[nid-1].active == 0) {
                nodes[nid-1].id = nid;
                nodes[nid-1].addr = caddr;
                nodes[nid-1].active = 1;
                reg_cnt++;
                printf("Node %d registered.\n", nid);
                
                uint8_t ack[6]; 
                pack_header(ack, MSG_ACK, buf[1], 0, 0); 
                ack[5]=calc_crc(ack,5);
                sendto(sock, ack, 6, 0, (struct sockaddr*)&caddr, clen);
            }
        }
    }

    printf("Assigning regions (RELIABLE)...\n");
    
    // Faza ASSIGN (Teraz w pętli reliability!)
    for(int i=0; i<NODE_COUNT; i++) {
        uint8_t msg[32]; 
        global_seq++;
        pack_header(msg, MSG_ASSIGN, global_seq, nodes[i].id, 8);
        int idx = nodes[i].id - 1;
        
        // Obliczamy parametry dla noda
        uint8_t rx = (idx % 2) * 20;
        uint8_t ry = (idx / 2) * 20;
        
        msg[5] = rx; msg[6] = ry; 
        msg[7] = 20; msg[8] = 20; 
        
        int16_t ang = (int16_t)config.angle_deg;
        int16_t stp = (int16_t)(config.step * 100); 
        
        msg[9] = (ang >> 8) & 0xFF; msg[10] = ang & 0xFF;
        msg[11] = (stp >> 8) & 0xFF; msg[12] = stp & 0xFF;
        
        msg[13] = calc_crc(msg, 13);
        
        // WYŚLIJ I CZEKAJ NA ACK
        printf("Sending ASSIGN to Node %d...\n", nodes[i].id);
        int res = send_reliable(sock, i, msg, 14, MSG_ACK, buf, sizeof(buf));
        if(res < 0) {
            printf("Failed to configure Node %d!\n", nodes[i].id);
        } else {
            printf("Node %d configured (ACK received).\n", nodes[i].id);
        }
    }

    fetch_origin_coordinates(sock);

    generate_lsystem();
    run_simulation(sock);
    collect_results(sock);

    printf("\n=== RESULT ===\n");
    for(int y=0; y<GRID_SIZE; y++) {
        for(int x=0; x<GRID_SIZE; x++) putchar(global_grid[y][x]);
        putchar('\n');
    }

    free(gen_current);
    free(gen_next);
    return 0;
}
