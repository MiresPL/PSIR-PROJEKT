#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <stdint.h>
#include <math.h>

// --- KONFIGURACJA ---
#define PORT 8000
#define NODE_COUNT 4
#define GRID_SIZE 40        
#define NODE_GRID_SIZE 20   
#define CHUNK_SIZE 20       
#define TIMEOUT_US 4000000   // ZWIĘKSZONE DO 4 SEKUND! (Emulator jest wolny)

#define ALP_VERSION 1
#define MSG_REGISTER 0x1
#define MSG_ASSIGN   0x2
#define MSG_DATA     0x3
#define MSG_ACK      0x4
#define MSG_REQUEST  0x5
#define MSG_RESPONSE 0x6
#define MSG_HANDOVER 0x7

typedef struct {
    uint8_t id;             
    struct sockaddr_in addr;
    int active;
} Node;

Node nodes[NODE_COUNT];
char global_grid[GRID_SIZE][GRID_SIZE];
uint8_t global_seq = 0;

// Utils
int get_node_index(double x, double y) {
    int col = (int)(x / 20.0);
    int row = (int)(y / 20.0);
    if (col < 0) col = 0; if (col > 1) col = 1;
    if (row < 0) row = 0; if (row > 1) row = 1;
    return row * 2 + col;
}

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

// Parser L-Systemu
void load_and_generate(char *output) {
    char axiom[128] = "F-F-F-F";
    int iterations = 2;
    char rule_char = 'F';
    char rule_repl[128] = "F-F+F+FF-F-F+F";
    
    FILE *f = fopen("input.txt", "r");
    if(f) { 
        printf("Loading input.txt...\n");
        char line[256];
        while(fgets(line, 256, f)) {
            // Usuwanie znaków nowej linii
            line[strcspn(line, "\r\n")] = 0;
            
            if(strstr(line, "axiom:")) {
                char *ptr = strchr(line, ':');
                if(ptr) strcpy(axiom, ptr+1);
                // Usuń spacje wiodące
                while(axiom[0] == ' ') memmove(axiom, axiom+1, strlen(axiom));
            }
            if(strstr(line, "iterations:")) {
                char *ptr = strchr(line, ':');
                if(ptr) iterations = atoi(ptr+1);
            }
            if(strstr(line, "rule:")) {
                // Format: rule: F=String
                char *eq = strchr(line, '=');
                if(eq) {
                    *eq = 0; // Podział stringa na znaku =
                    char *key = strchr(line, ':') + 1;
                    while(*key == ' ') key++; // Pomiń spacje
                    rule_char = key[0];
                    strcpy(rule_repl, eq+1);
                }
            }
        }
        fclose(f);
    } else {
        printf("No input.txt found. Using internal defaults.\n");
    }

    // Generowanie
    char current[8192], next[8192];
    strcpy(current, axiom);

    for(int k=0; k<iterations; k++) {
        next[0] = 0;
        for(int i=0; current[i]; i++) {
            if(current[i] == rule_char) strcat(next, rule_repl);
            else { 
                int l=strlen(next); 
                next[l]=current[i]; 
                next[l+1]=0; 
            }
        }
        strcpy(current, next);
    }
    strcpy(output, current);
    printf("L-System: Axiom '%s', Rule '%c'->'%s', Iters %d\n", axiom, rule_char, rule_repl, iterations);
    printf("Generated string length: %ld\n", strlen(output));
}

void run_simulation(int sock, char *full_string) {
    int cursor = 0;
    int total_len = strlen(full_string);
    if(total_len == 0) return;

    double cur_x = 10.0;
    double cur_y = 10.0;
    double cur_angle = 270.0;

    struct sockaddr_in from; 
    socklen_t flen = sizeof(from);
    uint8_t buf[1024];

    printf("Starting simulation...\n");

    while (cursor < total_len) {
        int node_idx = get_node_index(cur_x, cur_y);
        int target_id = nodes[node_idx].id;

        int chunk_len = CHUNK_SIZE;
        if (cursor + chunk_len > total_len) chunk_len = total_len - cursor;

        global_seq++;
        uint8_t packet[512];
        int payload_len = 6 + chunk_len; // 6 bajtów pozycji + tekst
        
        pack_header(packet, MSG_DATA, global_seq, target_id, payload_len);

        int16_t sx = (int16_t)(cur_x * 10);
        int16_t sy = (int16_t)(cur_y * 10);
        int16_t sa = (int16_t)(cur_angle);

        packet[5] = (sx >> 8) & 0xFF; packet[6] = sx & 0xFF;
        packet[7] = (sy >> 8) & 0xFF; packet[8] = sy & 0xFF;
        packet[9] = (sa >> 8) & 0xFF; packet[10] = sa & 0xFF;

        memcpy(&packet[11], &full_string[cursor], chunk_len);
        packet[11+chunk_len] = calc_crc(packet, 11+chunk_len);

        int success = 0;
        for(int retry=0; retry<3; retry++) {
            sendto(sock, packet, 11+chunk_len+1, 0, 
                  (struct sockaddr*)&nodes[node_idx].addr, sizeof(nodes[node_idx].addr));

            struct timeval tv = {4, 0}; // 4 SEKUNDY TIMEOUT
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr*)&from, &flen);
            
            if (n > 0 && (buf[0] & 0x0F) == MSG_HANDOVER) { // Ignoruję seq dla uproszczenia
                int16_t nx = (buf[7] << 8) | buf[8];
                int16_t ny = (buf[9] << 8) | buf[10];
                int16_t na = (buf[11] << 8) | buf[12];

                cur_x = nx / 10.0;
                cur_y = ny / 10.0;
                cur_angle = (double)na;
                
                cursor += chunk_len;
                success = 1;
                // printf("Chunk processed by Node %d. New pos: %.1f %.1f\n", target_id, cur_x, cur_y);
                usleep(50000); // Mała pauza dla emulatora
                break;
            } else {
                printf("Timeout/Fail Node %d (Retry %d)...\n", target_id, retry+1);
            }
        }

        if (!success) {
	    printf("WARNING: Node %d timeout. Skipping chunk logic to continue...\n", target_id);
            
            cursor += chunk_len;

		//previous version
            //printf("CRITICAL ERROR: Node %d timeout. Aborting.\n", target_id);
            //break;
        }
    }
    printf("Simulation finished.\n");
}

void collect_results(int sock) {
    printf("Requesting results...\n");
    memset(global_grid, '.', sizeof(global_grid));
    
    struct sockaddr_in from; 
    socklen_t flen = sizeof(from);
    uint8_t buf[1200]; 

    struct timeval tv = {2, 0}; // 2 sekundy na odbiór wyników
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    for(int i=0; i<NODE_COUNT; i++) {
        uint8_t req[6]; 
        global_seq++;
        pack_header(req, MSG_REQUEST, global_seq, nodes[i].id, 0);
        req[5] = calc_crc(req, 5);
        
        sendto(sock, req, 6, 0, (struct sockaddr*)&nodes[i].addr, sizeof(nodes[i].addr));

        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr*)&from, &flen);
        if (n > 0 && (buf[0] & 0x0F) == MSG_RESPONSE) {
            int nid = buf[2];
            int idx = nid - 1;
            int off_x = (idx % 2) * NODE_GRID_SIZE;
            int off_y = (idx / 2) * NODE_GRID_SIZE;

            int ptr = 5;
            for(int y=0; y<NODE_GRID_SIZE; y++) {
                for(int x=0; x<NODE_GRID_SIZE; x++) {
                    if(buf[ptr] == '#') global_grid[off_y + y][off_x + x] = '#';
                    ptr++;
                }
            }
            printf("Node %d OK.\n", nid);
        } else {
            printf("Node %d NO RESPONSE.\n", nodes[i].id);
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
    bind(sock, (struct sockaddr*)&saddr, sizeof(saddr));

    printf("=== SERVER STARTED ===\nWaiting for %d nodes...\n", NODE_COUNT);

    socklen_t clen = sizeof(caddr); 
    uint8_t buf[256];
    int reg_cnt = 0;
    for(int i=0; i<NODE_COUNT; i++) nodes[i].active = 0;

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
                uint8_t ack[6]; pack_header(ack, MSG_ACK, buf[1], 0, 0); ack[5]=calc_crc(ack,5);
                sendto(sock, ack, 6, 0, (struct sockaddr*)&caddr, clen);
            }
        }
    }

    printf("Assigning regions...\n");
    for(int i=0; i<NODE_COUNT; i++) {
        uint8_t msg[32]; global_seq++;
        pack_header(msg, MSG_ASSIGN, global_seq, nodes[i].id, 4);
        int idx = nodes[i].id - 1;
        msg[5] = (idx % 2) * 20; msg[6] = (idx / 2) * 20; // X, Y
        msg[7] = 20; msg[8] = 20; // W, H
        msg[9] = calc_crc(msg, 9);
        sendto(sock, msg, 10, 0, (struct sockaddr*)&nodes[i].addr, sizeof(nodes[i].addr));
        usleep(100000); // 100ms pauzy
    }

    char lsystem[8192];
    load_and_generate(lsystem);
    run_simulation(sock, lsystem);
    collect_results(sock);

    printf("\n=== RESULT ===\n");
    for(int y=0; y<GRID_SIZE; y++) {
        for(int x=0; x<GRID_SIZE; x++) putchar(global_grid[y][x]);
        putchar('\n');
    }
    return 0;
}