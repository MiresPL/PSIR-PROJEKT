#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <stdint.h>

/* ================= KONFIGURACJA PROTOKOŁU ALP (Binary & Reliable) ================= */
#define ALP_VERSION      1
#define MSG_REGISTER     0x1
#define MSG_ASSIGN       0x2
#define MSG_DATA         0x3
#define MSG_ACK          0x4
#define MSG_REQUEST      0x5
#define MSG_RESPONSE     0x6
#define MSG_HANDOVER     0x7

#define PORT 8000
#define MAX_STR    8192
#define MAX_RULES  16
#define GRID_WIDTH  40
#define GRID_HEIGHT 40
#define MAX_NODES   4 
#define REGIONS_X   2
#define TIMEOUT_MS  200
#define MAX_RETRIES 3

/* ================= STRUKTURY DANYCH ================= */
typedef struct {
    uint8_t node_id;
    struct sockaddr_in addr;
    int active;
} Node;

Node nodes[MAX_NODES];
int node_count = 0;
char global_grid[GRID_HEIGHT][GRID_WIDTH]; 

typedef struct {
    char symbol;
    char replacement[MAX_STR];
} Rule;

typedef struct {
    char alphabet[64];
    char draw_symbols[64];
    char axiom[MAX_STR];
    int iterations;
    int angle;
    Rule rules[MAX_RULES];
    int rule_count;
} LSystem;

/* ================= MATEMATYKA ================= */
int my_floor(double x) {
    int i = (int)x;
    if (x < 0 && x != i) return i - 1;
    return i;
}

int get_region_for_point(double x, double y) {
    int region_w = GRID_WIDTH / REGIONS_X;
    int region_h = GRID_HEIGHT / REGIONS_X;
    int rx = my_floor(x / region_w);
    int ry = my_floor(y / region_h);
    
    if (rx < 0 || rx >= REGIONS_X || ry < 0 || ry >= REGIONS_X) return -1;
    return ry * REGIONS_X + rx;
}

/* ================= WARSTWA SIECIOWA (ALP) ================= */
uint8_t alp_crc(uint8_t *buf, int len) {
    uint8_t crc = 0;
    for (int i = 0; i < len; i++) crc += buf[i];
    return crc;
}

void pack_header(uint8_t *buf, int type, int node_id, int payload_len) {
    buf[0] = (ALP_VERSION << 4) | (type & 0x0F);
    buf[1] = (uint8_t)node_id;
    buf[2] = (payload_len >> 8) & 0xFF;
    buf[3] = payload_len & 0xFF;
}

void send_ack(int sockfd, struct sockaddr_in *dest) {
    uint8_t buf[5];
    pack_header(buf, MSG_ACK, 0, 0);
    buf[4] = alp_crc(buf, 4);
    sendto(sockfd, buf, 5, 0, (struct sockaddr *)dest, sizeof(*dest));
}

void send_reliable(int sockfd, uint8_t *buf, int len, struct sockaddr_in *dest) {
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = TIMEOUT_MS * 1000;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);

    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    uint8_t ack_buf[16];

    for(int i=0; i<MAX_RETRIES; i++) {
        sendto(sockfd, buf, len, 0, (struct sockaddr *)dest, sizeof(*dest));
        int n = recvfrom(sockfd, ack_buf, sizeof(ack_buf), 0, (struct sockaddr *)&from, &from_len);
        if (n > 0) {
            int type = ack_buf[0] & 0x0F;
            if (type == MSG_ACK) return;
        }
        printf("WARN: No ACK. Retrying (%d/%d)...\n", i+1, MAX_RETRIES);
    }
    printf("ERROR: Transmission failed (Node unreachable).\n");
}

/* ================= FUNKCJE APLIKACJI ================= */
int load_lsystem(const char *filename, LSystem *ls) {
    FILE *f = fopen(filename, "r");
    if (!f) { perror("Input error"); return -1; }
    char line[256]; ls->rule_count = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        if (strncmp(line, "alphabet:", 9) == 0) sscanf(line + 9, "%[^\n]", ls->alphabet);
        else if (strncmp(line, "axiom:", 6) == 0) sscanf(line + 6, "%s", ls->axiom);
        else if (strncmp(line, "iterations:", 11) == 0) ls->iterations = atoi(line + 11);
        else if (strncmp(line, "angle:", 6) == 0) ls->angle = atoi(line + 6);
        else if (strncmp(line, "rule:", 5) == 0) {
            char sym; char rhs[MAX_STR]; sscanf(line + 5, " %c=%s", &sym, rhs);
            ls->rules[ls->rule_count].symbol = sym;
            strcpy(ls->rules[ls->rule_count].replacement, rhs); ls->rule_count++;
        } else if (strncmp(line, "draw:", 5) == 0) sscanf(line + 5, "%s", ls->draw_symbols);
    }
    fclose(f); return 0;
}

void generate_lsystem(LSystem *ls, char *output) {
    char current[MAX_STR]; char next[MAX_STR];
    strcpy(current, ls->axiom);
    for (int it = 0; it < ls->iterations; it++) {
        next[0] = '\0';
        for (int i = 0; current[i]; i++) {
            int replaced = 0;
            for (int r = 0; r < ls->rule_count; r++) {
                if (current[i] == ls->rules[r].symbol) {
                    strcat(next, ls->rules[r].replacement); replaced = 1; break;
                }
            }
            if (!replaced) {
                int len = strlen(next); next[len] = current[i]; next[len + 1] = '\0';
            }
        }
        strcpy(current, next);
    }
    strcpy(output, current);
}

void send_assign(LSystem *ls, int sockfd, Node *node, int region_id) {
    uint8_t buf[32]; 
    pack_header(buf, MSG_ASSIGN, node->node_id, 6);
    int pos = 4;
    int rx = (region_id % 2) * 20;
    int ry = (region_id / 2) * 20;

    buf[pos++] = (uint8_t)rx; buf[pos++] = (uint8_t)ry;
    buf[pos++] = 20; buf[pos++] = 20; 
    buf[pos++] = (uint8_t)ls->angle;
    
    buf[pos++] = alp_crc(buf, pos);
    send_reliable(sockfd, buf, pos, &node->addr);
    printf("ASSIGN -> Node %d (Region %d,%d)\n", node->node_id, rx, ry);
}

void send_work_chunk(int sockfd, int node_idx, const char *word, int start_idx, double x, double y, double angle) {
    uint8_t buf[MAX_STR + 64];
    int word_len = strlen(word);
    int payload_len = 2 + 8 + 8 + 8 + word_len;
    
    pack_header(buf, MSG_DATA, nodes[node_idx].node_id, payload_len);
    int pos = 4;

    buf[pos++] = (start_idx >> 8) & 0xFF;
    buf[pos++] = start_idx & 0xFF;
    memcpy(&buf[pos], &x, 8); pos += 8;
    memcpy(&buf[pos], &y, 8); pos += 8;
    memcpy(&buf[pos], &angle, 8); pos += 8;
    memcpy(&buf[pos], word, word_len); pos += word_len;

    buf[pos++] = alp_crc(buf, pos);
    send_reliable(sockfd, buf, pos, &nodes[node_idx].addr);
    printf("ROUTER: Task sent to Node %d (Pos %.1f, %.1f)\n", nodes[node_idx].node_id, x, y);
}

/* ================= MAIN ================= */
int main() {
    LSystem ls;
    char final_word[MAX_STR];
    memset(global_grid, '.', sizeof(global_grid));

    if (load_lsystem("input.txt", &ls) != 0) return 1;
    generate_lsystem(&ls, final_word);

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in servaddr, cliaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(PORT);
    
    if (bind(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("Bind failed"); return 1;
    }

    printf("=== SERVER STARTED (Reliable ALP) ===\n");
    printf("Waiting for %d nodes to register...\n", MAX_NODES);

    struct timeval tv_zero = {0, 0};
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv_zero, sizeof tv_zero);

    while (node_count < MAX_NODES) {
        socklen_t len = sizeof(cliaddr);
        uint8_t buffer[256];
        int n = recvfrom(sockfd, buffer, sizeof(buffer), 0, (struct sockaddr *)&cliaddr, &len);
        if (n > 0) {
            int type = buffer[0] & 0x0F;
            if (type == MSG_REGISTER) {
                uint8_t nid = buffer[1];
                int exists = 0;
                for(int i=0; i<node_count; i++) if(nodes[i].node_id == nid) exists=1;
                if(!exists) {
                    nodes[node_count].node_id = nid;
                    nodes[node_count].addr = cliaddr;
                    nodes[node_count].active = 1;
                    send_ack(sockfd, &cliaddr);
                    send_assign(&ls, sockfd, &nodes[node_count], node_count);
                    node_count++;
                    printf("Node %d registered.\n", nid);
                }
            }
        }
    }

    double start_x = 20.0, start_y = 20.0;
    int start_node_idx = get_region_for_point(start_x, start_y);
    printf("\n>>> STARTING SIMULATION <<<\n");
    if(start_node_idx >= 0)
        send_work_chunk(sockfd, start_node_idx, final_word, 0, start_x, start_y, 0.0);

    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv_zero, sizeof tv_zero);

    while (1) {
        socklen_t len = sizeof(cliaddr);
        uint8_t buffer[1024];
        int n = recvfrom(sockfd, buffer, sizeof(buffer), 0, (struct sockaddr *)&cliaddr, &len);
        if (n <= 0) continue;

        int type = buffer[0] & 0x0F;

        if (type == MSG_HANDOVER) {
            send_ack(sockfd, &cliaddr);

            // === POPRAWKA TUTAJ: Indeksy przesunięte o 2 w lewo! ===
            // Header (4 bajty) -> Payload zaczyna się od buffer[4]
            int idx = (buffer[4] << 8) | buffer[5];
            double h_x, h_y, h_ang;
            memcpy(&h_x, &buffer[6], 8);
            memcpy(&h_y, &buffer[14], 8);
            memcpy(&h_ang, &buffer[22], 8);

            printf("HANDOVER (Idx %d) at (%.2f, %.2f)\n", idx, h_x, h_y);

            if (idx >= strlen(final_word)) {
                printf("DRAWING FINISHED!\n");
                break; 
            }

            int next_node = get_region_for_point(h_x, h_y);
            if (next_node == -1) {
                printf("Turtle escaped!\n");
                break;
            }
            send_work_chunk(sockfd, next_node, final_word, idx, h_x, h_y, h_ang);
        }
    }

    printf("\n>>> REQUESTING FINAL IMAGES <<<\n");
    for (int i = 0; i < node_count; i++) {
        uint8_t buf[16];
        pack_header(buf, MSG_REQUEST, nodes[i].node_id, 0);
        buf[4] = alp_crc(buf, 4);
        send_reliable(sockfd, buf, 5, &nodes[i].addr);
    }

    printf("Collecting data...\n");
    struct timeval tv_poll = {0, 100000};
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv_poll, sizeof tv_poll);

    int received_count = 0;
    while(received_count < 60) { 
        socklen_t len = sizeof(cliaddr);
        uint8_t buf[2048];
        int n = recvfrom(sockfd, buf, sizeof(buf), 0, (struct sockaddr *)&cliaddr, &len);
        if(n > 0) {
            int type = buf[0] & 0x0F;
            if (type == MSG_RESPONSE) {
                send_ack(sockfd, &cliaddr); 
                uint8_t node_id = buf[1];
                int region_idx = -1;
                for(int i=0; i<node_count; i++) if(nodes[i].node_id == node_id) region_idx = i;
                if(region_idx != -1) {
                     int start_x = (region_idx % 2) * 20;
                     int start_y = (region_idx / 2) * 20;
                     int ptr = 4;
                     printf("Merged data from Node %d.\n", node_id);
                     for(int y=0; y<20; y++) {
                        for(int x=0; x<20; x++) {
                            char val = buf[ptr++];
                            if(val == '#') global_grid[start_y + y][start_x + x] = '#';
                        }
                     }
                }
            }
        }
        received_count++;
    }

    printf("\n================ FINAL RESULT ================\n");
    for(int y = 0; y < GRID_HEIGHT; y++) {
        for(int x = 0; x < GRID_WIDTH; x++) {
            printf("%c", global_grid[y][x]);
        }
        printf("\n");
    }
    printf("==============================================\n");
    return 0;
}
