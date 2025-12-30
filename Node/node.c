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

/* ================= KONFIGURACJA ================= */
#define ALP_SYSTEM_ID    0xA5
#define ALP_REGISTER     0x01
#define ALP_ASSIGN       0x02
#define ALP_DATA         0x03
#define ALP_REQUEST_DATA 0x05
#define ALP_RESPONSE_DATA 0x06
#define ALP_HANDOVER     0x07

#define MAX_STR          8192  
#define MAX_REGION       32
#define SERVER_IP        "192.168.56.104" 
#define SERVER_PORT      8000
#define NODE_ID          4  // <--- PAMIĘTAJ: Zmieniaj to (1, 2, 3, 4) przed kompilacją!

char grid[MAX_REGION][MAX_REGION];
int rx, ry, rw, rh, g_angle;
int sockfd;
struct sockaddr_in servaddr;

uint8_t alp_crc(uint8_t *buf, int len) {
    uint8_t crc = 0;
    for (int i = 0; i < len; i++) crc += buf[i];
    return crc;
}

// Funkcja wysyłająca zgłoszenie Handover
void send_handover(int idx, double x, double y, double angle) {
    uint8_t buf[64];
    int pos = 0;
    buf[pos++] = ALP_SYSTEM_ID;
    buf[pos++] = ALP_HANDOVER;
    buf[pos++] = NODE_ID;
    buf[pos++] = 0;
    
    // Payload: index(2) + x(8) + y(8) + angle(8) = 26 bajtów
    buf[pos++] = 0; buf[pos++] = 26;

    buf[pos++] = (idx >> 8) & 0xFF;
    buf[pos++] = idx & 0xFF;
    
    memcpy(&buf[pos], &x, 8); pos += 8;
    memcpy(&buf[pos], &y, 8); pos += 8;
    memcpy(&buf[pos], &angle, 8); pos += 8;

    buf[pos++] = alp_crc(buf, pos);
    sendto(sockfd, buf, pos, 0, (struct sockaddr *)&servaddr, sizeof(servaddr));
    printf(">>> Handover sent! Index: %d, Pos: %.2f,%.2f\n", idx, x, y);
}

// Funkcja Rysująca (Smart Turtle) - WERSJA POPRAWIONA (CIĄGŁA)
void draw_turtle_smart(const char *word, int start_idx, double start_x, double start_y, double start_angle) {
    double cur_x = start_x;
    double cur_y = start_y;
    double angle_rad = start_angle; 

    // 1. Zabezpieczenie krawędzi (wciąganie floatów)
    if(cur_x < rx) cur_x = rx + 0.01;
    if(cur_x >= rx+rw) cur_x = rx + rw - 0.01;
    if(cur_y < ry) cur_y = ry + 0.01;
    if(cur_y >= ry+rh) cur_y = ry + rh - 0.01;

    // 2. RYSOWANIE PUNKTU STARTOWEGO (To naprawia dziury!)
    int start_ix = (int)round(cur_x);
    int start_iy = (int)round(cur_y);
    // Sprawdzamy czy punkt startowy jest u nas
    if (start_ix >= rx && start_ix < rx + rw && start_iy >= ry && start_iy < ry + rh) {
        grid[start_iy - ry][start_ix - rx] = '#';
    }

    // 3. Pętla ruchu
    for (int i = start_idx; word[i]; i++) {
        if (word[i] == 'F') {
            double next_x = cur_x + cos(angle_rad);
            double next_y = cur_y - sin(angle_rad); // y maleje w górę

            int ix = (int)round(next_x);
            int iy = (int)round(next_y);

            // --- DETEKCJA GRANIC (HANDOVER) ---
            if (ix < rx || ix >= rx + rw || iy < ry || iy >= ry + rh) {
                // Wykryto wyjście poza region!
                send_handover(i, next_x, next_y, angle_rad);
                return; // Przerywamy pracę
            }

            // Rysowanie kolejnego punktu
            grid[iy - ry][ix - rx] = '#';
            cur_x = next_x;
            cur_y = next_y;
        } else if (word[i] == '+') {
            angle_rad += (g_angle * M_PI / 180.0);
        } else if (word[i] == '-') {
            angle_rad -= (g_angle * M_PI / 180.0);
        }
    }
    // Jeśli pętla doszła do końca, zgłaszamy Handover z indeksem końca słowa
    send_handover(strlen(word), cur_x, cur_y, angle_rad);
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0); // Wyłączenie buforowania stdout
    memset(grid, '.', sizeof(grid));
    printf("Node %d starting...\n", NODE_ID);

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) return 1;

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_IP, &servaddr.sin_addr);

    // Rejestracja
    uint8_t reg[8] = {ALP_SYSTEM_ID, ALP_REGISTER, NODE_ID, 0,0,0,0,0};
    reg[7] = alp_crc(reg, 7);
    sendto(sockfd, reg, 8, 0, (struct sockaddr *)&servaddr, sizeof(servaddr));
    printf("REGISTER sent to %s:%d\n", SERVER_IP, SERVER_PORT);

    uint8_t buffer[MAX_STR + 64];
    while (1) {
        int n = recvfrom(sockfd, buffer, sizeof(buffer), 0, NULL, NULL);
        if (n <= 0) continue;
        if (buffer[0] != ALP_SYSTEM_ID) continue;

        if (buffer[1] == ALP_ASSIGN) {
            rx = buffer[6]; ry = buffer[7];
            rw = buffer[8]; rh = buffer[9];
            g_angle = buffer[10];
            printf("ASSIGN: Region (%d,%d) size %dx%d\n", rx, ry, rw, rh);
        }
        else if (buffer[1] == ALP_DATA) {
            // Dekodowanie metadanych (idx, x, y, angle)
            int idx = (buffer[6] << 8) | buffer[7];
            double sx, sy, sa;
            memcpy(&sx, &buffer[8], 8);
            memcpy(&sy, &buffer[16], 8);
            memcpy(&sa, &buffer[24], 8);
            
            // Wyciąganie słowa L-systemu
            int header_size = 32; 
            char word[MAX_STR];
            int word_len = (buffer[4] << 8) | buffer[5];
            word_len -= 26; // Odejmujemy metadane od payloadu

            memcpy(word, &buffer[header_size], word_len);
            word[word_len] = '\0';

            printf("TASK received! Start Idx: %d, Pos: %.1f,%.1f. Working...\n", idx, sx, sy);
            draw_turtle_smart(word, idx, sx, sy, sa);
        }
        else if (buffer[1] == ALP_REQUEST_DATA) {
            printf("Server requested data. Sending local grid...\n");
            
            uint8_t resp[1024];
            int pos = 0;

            resp[pos++] = ALP_SYSTEM_ID;
            resp[pos++] = ALP_RESPONSE_DATA; 
            resp[pos++] = NODE_ID;
            resp[pos++] = 0; 
            
            // Payload size: szer * wys
            int data_size = rw * rh; 
            resp[pos++] = (data_size >> 8) & 0xFF;
            resp[pos++] = data_size & 0xFF;

            // Pakowanie siatki
            for(int y = 0; y < rh; y++) {
                for(int x = 0; x < rw; x++) {
                    resp[pos++] = grid[y][x];
                }
            }
            resp[pos++] = alp_crc(resp, pos); 
            
            sendto(sockfd, resp, pos, 0, (struct sockaddr *)&servaddr, sizeof(servaddr));
            printf("Data sent (%d bytes).\n", data_size);
        }
    }
    close(sockfd);
    return 0;
}
