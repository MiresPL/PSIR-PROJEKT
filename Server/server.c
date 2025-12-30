#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <math.h>

/* ================= KONFIGURACJA PROTOKOŁU ALP ================= */
#define ALP_SYSTEM_ID    0xA5
#define ALP_REGISTER     0x01
#define ALP_ASSIGN       0x02
#define ALP_DATA         0x03
#define ALP_REQUEST_DATA 0x05
#define ALP_RESPONSE_DATA 0x06
#define ALP_HANDOVER     0x07

#define PORT 8000
#define MAX_STR    8192
#define MAX_RULES  16
#define GRID_WIDTH  40
#define GRID_HEIGHT 40
#define MAX_NODES   4 
#define REGIONS_X   2

/* ================= STRUKTURY DANYCH ================= */
typedef struct {
    uint8_t node_id;
    struct sockaddr_in addr;
    int active;
} Node;

Node nodes[MAX_NODES];
int node_count = 0;
char global_grid[GRID_HEIGHT][GRID_WIDTH]; // Płótno do finalnego obrazka

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

/* ================= FUNKCJE POMOCNICZE ================= */
uint8_t alp_crc(uint8_t *buf, int len) {
    uint8_t crc = 0;
    for (int i = 0; i < len; i++) crc += buf[i];
    return crc;
}

// Funkcja obliczająca, w którym regionie jest punkt (x,y)
// Używamy floor(), aby poprawnie obsługiwać krawędzie i liczby ujemne
int get_region_for_point(double x, double y) {
    int region_w = GRID_WIDTH / REGIONS_X; // 20
    int region_h = GRID_HEIGHT / REGIONS_X; // 20
    int rx = (int)floor(x / region_w);
    int ry = (int)floor(y / region_h);
    
    // Jeśli punkt wyszedł poza siatkę 2x2, zwracamy -1 (błąd/koniec)
    if (rx < 0 || rx >= REGIONS_X || ry < 0 || ry >= REGIONS_X) return -1;
    return ry * REGIONS_X + rx;
}

/* ================= L-SYSTEM (LOGIKA) ================= */
int load_lsystem(const char *filename, LSystem *ls) {
    FILE *f = fopen(filename, "r");
    if (!f) { perror("Cannot open input.txt"); return -1; }
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

/* ================= FUNKCJE SIECIOWE ================= */
void send_assign(LSystem *ls, int sockfd, Node *node, int region_id) {
    uint8_t buf[32]; int pos = 0;
    int rx = (region_id % 2) * 20;
    int ry = (region_id / 2) * 20;

    buf[pos++] = ALP_SYSTEM_ID; buf[pos++] = ALP_ASSIGN;
    buf[pos++] = node->node_id; buf[pos++] = region_id;
    buf[pos++] = 0; buf[pos++] = 5; 
    buf[pos++] = (uint8_t)rx; buf[pos++] = (uint8_t)ry;
    buf[pos++] = 20; buf[pos++] = 20; 
    buf[pos++] = (uint8_t)ls->angle;
    buf[pos++] = alp_crc(buf, pos);
    sendto(sockfd, buf, pos, 0, (struct sockaddr *)&node->addr, sizeof(node->addr));
    printf("ASSIGN -> node %d region %d (Starts at %d,%d)\n", node->node_id, region_id, rx, ry);
}

// Funkcja wysyłająca zadanie do konkretnego węzła
void send_work_chunk(int sockfd, int node_idx, const char *word, int start_idx, double x, double y, double angle) {
    uint8_t buf[MAX_STR + 64];
    int word_len = strlen(word);
    int pos = 0;

    buf[pos++] = ALP_SYSTEM_ID;
    buf[pos++] = ALP_DATA;
    buf[pos++] = nodes[node_idx].node_id;
    buf[pos++] = 0; 
    
    // Payload: index(2) + x(8) + y(8) + angle(8) + word
    int payload_len = 2 + 8 + 8 + 8 + word_len;
    buf[pos++] = (payload_len >> 8) & 0xFF;
    buf[pos++] = payload_len & 0xFF;

    // Metadane
    buf[pos++] = (start_idx >> 8) & 0xFF;
    buf[pos++] = start_idx & 0xFF;
    memcpy(&buf[pos], &x, 8); pos += 8;
    memcpy(&buf[pos], &y, 8); pos += 8;
    memcpy(&buf[pos], &angle, 8); pos += 8;

    // Słowo L-Systemu
    memcpy(&buf[pos], word, word_len);
    pos += word_len;

    buf[pos++] = alp_crc(buf, pos);

    sendto(sockfd, buf, pos, 0, (struct sockaddr *)&nodes[node_idx].addr, sizeof(nodes[node_idx].addr));
    printf("ROUTER: Sent task to Node %d (Start Idx: %d, Pos: %.1f,%.1f)\n", nodes[node_idx].node_id, start_idx, x, y);
}

/* ================= MAIN ================= */
int main() {
    LSystem ls;
    char final_word[MAX_STR];
    memset(global_grid, '.', sizeof(global_grid)); // Czyścimy płótno

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

    printf("=== SERVER STARTED ===\n");
    printf("L-System Word Length: %lu\n", strlen(final_word));
    printf("Waiting for %d nodes to register...\n", MAX_NODES);

    // 1. FAZA REJESTRACJI
    while (node_count < MAX_NODES) {
        socklen_t len = sizeof(cliaddr);
        uint8_t buffer[256];
        int n = recvfrom(sockfd, buffer, sizeof(buffer), 0, (struct sockaddr *)&cliaddr, &len);
        if (n > 0 && buffer[1] == ALP_REGISTER) {
            int exists = 0;
            for(int i=0; i<node_count; i++) if(nodes[i].node_id == buffer[2]) exists=1;
            
            if(!exists) {
                nodes[node_count].node_id = buffer[2];
                nodes[node_count].addr = cliaddr;
                nodes[node_count].active = 1;
                send_assign(&ls, sockfd, &nodes[node_count], node_count);
                node_count++;
                printf("Node %d registered.\n", buffer[2]);
            }
        }
    }

    // 2. START SYMULACJI (Wybieramy węzeł środkowy)
    // Startujemy w punkcie (20.0, 20.0). Matematycznie należy on do Regionu 3 (Node 4).
    double start_x = 20.0, start_y = 20.0;
    int start_node_idx = get_region_for_point(start_x, start_y);
    
    printf("\n>>> STARTING SIMULATION <<<\n");
    if(start_node_idx >= 0)
        send_work_chunk(sockfd, start_node_idx, final_word, 0, start_x, start_y, 0.0);

    // 3. FAZA ROUTINGU (HANDOVER)
    while (1) {
        socklen_t len = sizeof(cliaddr);
        uint8_t buffer[1024];
        int n = recvfrom(sockfd, buffer, sizeof(buffer), 0, (struct sockaddr *)&cliaddr, &len);
        if (n <= 0) continue;

        if (buffer[1] == ALP_HANDOVER) {
            int idx = (buffer[6] << 8) | buffer[7];
            double h_x, h_y, h_ang;
            memcpy(&h_x, &buffer[8], 8);
            memcpy(&h_y, &buffer[16], 8);
            memcpy(&h_ang, &buffer[24], 8);

            printf("HANDOVER from Node (Idx %d): Turtle at (%.2f, %.2f)\n", idx, h_x, h_y);

            // Sprawdź czy koniec słowa
            if (idx >= strlen(final_word)) {
                printf("DRAWING FINISHED! Proceeding to data collection...\n");
                break; 
            }

            int next_node = get_region_for_point(h_x, h_y);
            if (next_node == -1) {
                printf("Turtle escaped the grid boundaries! Stopping simulation.\n");
                break;
            }

            send_work_chunk(sockfd, next_node, final_word, idx, h_x, h_y, h_ang);
        }
    }

    // 4. FAZA ZBIERANIA DANYCH
    printf("\n>>> REQUESTING FINAL IMAGES FROM NODES <<<\n");
    for (int i = 0; i < node_count; i++) {
        uint8_t req[8] = {ALP_SYSTEM_ID, ALP_REQUEST_DATA, nodes[i].node_id, 0, 0, 0, 0, 0};
        req[7] = alp_crc(req, 7);
        sendto(sockfd, req, 8, 0, (struct sockaddr *)&nodes[i].addr, sizeof(nodes[i].addr));
    }

    // Czekamy na odpowiedzi i sklejamy obraz
    printf("Collecting data chunks... (Wait 2s)\n");
    for(int k=0; k<20; k++) { // Prosta pętla czekająca (20 * 0.1s = 2s)
        socklen_t len = sizeof(cliaddr);
        uint8_t buf[2048];
        // MSG_DONTWAIT pozwala nie blokować pętli, jeśli nic nie ma
        int n = recvfrom(sockfd, buf, sizeof(buf), MSG_DONTWAIT, (struct sockaddr *)&cliaddr, &len);
        
        if(n > 0 && buf[1] == ALP_RESPONSE_DATA) {
            uint8_t node_id = buf[2];
            int region_idx = -1;
            // Znajdź indeks regionu dla tego węzła
            for(int i=0; i<node_count; i++) if(nodes[i].node_id == node_id) region_idx = i;
            
            if(region_idx != -1) {
                 int start_x = (region_idx % 2) * 20;
                 int start_y = (region_idx / 2) * 20;
                 int ptr = 6; // Offset danych w pakiecie
                 
                 printf("Received data from Node %d. Merging into global grid...\n", node_id);
                 
                 for(int y=0; y<20; y++) {
                    for(int x=0; x<20; x++) {
                        global_grid[start_y + y][start_x + x] = buf[ptr++];
                    }
                 }
            }
        }
        usleep(100000); // 0.1s delay
    }

    // 5. WYŚWIETLENIE FINALNEGO WYNIKU
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
