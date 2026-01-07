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

/* ================= KONFIGURACJA ================= */
#define ALP_VERSION      1
#define MSG_REGISTER     0x1
#define MSG_ASSIGN       0x2
#define MSG_DATA         0x3
#define MSG_ACK          0x4
#define MSG_REQUEST      0x5
#define MSG_RESPONSE     0x6
#define MSG_HANDOVER     0x7

#define MAX_STR          8192  
#define MAX_REGION       32
#define SERVER_IP        "192.168.56.104" 
#define SERVER_PORT      8000
#define NODE_ID          4 
#define TIMEOUT_MS       200
#define MAX_RETRIES      3

char grid[MAX_REGION][MAX_REGION];
int rx, ry, rw, rh, g_angle;
int sockfd;
struct sockaddr_in servaddr;

/* ================= OPTYMALIZACJA IOT (LUT) ================= */
double SIN_LUT[91]; 

void init_lut() {
    double PI = 3.1415926535;
    for(int i=0; i<=90; i++) {
        double x = i * (PI / 180.0);
        double res = x - (x*x*x)/6.0 + (x*x*x*x*x)/120.0 - (x*x*x*x*x*x*x)/5040.0;
        SIN_LUT[i] = res;
    }
    SIN_LUT[0] = 0.0; SIN_LUT[30] = 0.5; SIN_LUT[90] = 1.0;
}

double fast_sin(int deg) {
    deg %= 360;
    if (deg < 0) deg += 360;
    if (deg <= 90) return SIN_LUT[deg];
    if (deg <= 180) return SIN_LUT[180 - deg];
    if (deg <= 270) return -SIN_LUT[deg - 180];
    return -SIN_LUT[360 - deg];
}

double fast_cos(int deg) {
    return fast_sin(deg + 90);
}

// Rzutowanie na int (zgodność z serwerem)
int fast_floor(double x) {
    return (int)x; 
}

int fast_round(double x) {
    return (int)(x + 0.5);
}

/* ================= ALP PROTOCOL & RELIABILITY ================= */
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

void send_ack(int sock, struct sockaddr_in *dest) {
    uint8_t buf[5];
    pack_header(buf, MSG_ACK, 0, 0);
    buf[4] = alp_crc(buf, 4);
    sendto(sock, buf, 5, 0, (struct sockaddr *)dest, sizeof(*dest));
}

void send_reliable(uint8_t *buf, int len) {
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = TIMEOUT_MS * 1000;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);

    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    uint8_t ack_buf[16];

    for(int i=0; i<MAX_RETRIES; i++) { 
        sendto(sockfd, buf, len, 0, (struct sockaddr *)&servaddr, sizeof(servaddr));
        
        int n = recvfrom(sockfd, ack_buf, sizeof(ack_buf), 0, (struct sockaddr *)&from, &from_len);
        if (n > 0) {
            int type = ack_buf[0] & 0x0F;
            if (type == MSG_ACK) return; 
        }
        // printf("Wait for ACK... Retry %d\n", i+1);
    }
    printf("ERROR: Server unreachable.\n");
}

void send_handover(int idx, double x, double y, double angle) {
    uint8_t buf[64];
    pack_header(buf, MSG_HANDOVER, NODE_ID, 26);
    int pos = 4;

    buf[pos++] = (idx >> 8) & 0xFF;
    buf[pos++] = idx & 0xFF;
    memcpy(&buf[pos], &x, 8); pos += 8;
    memcpy(&buf[pos], &y, 8); pos += 8;
    memcpy(&buf[pos], &angle, 8); pos += 8;

    buf[pos++] = alp_crc(buf, pos);
    
    send_reliable(buf, pos);
    printf(">>> Handover sent! (Idx %d)\n", idx);
}

/* ================= LOGIKA RYSOWANIA ================= */
void draw_turtle_smart(const char *word, int start_idx, double start_x, double start_y, double start_angle) {
    double cur_x = start_x;
    double cur_y = start_y;
    
    int current_angle_deg = (int)fast_round(start_angle * 180.0 / 3.1415926535);

    if(cur_x < rx) cur_x = rx + 0.001;
    if(cur_x >= rx+rw) cur_x = rx + rw - 0.001;
    if(cur_y < ry) cur_y = ry + 0.001;
    if(cur_y >= ry+rh) cur_y = ry + rh - 0.001;

    int start_ix = fast_floor(cur_x);
    int start_iy = fast_floor(cur_y);
    
    if (start_ix >= rx && start_ix < rx + rw && start_iy >= ry && start_iy < ry + rh) {
        grid[start_iy - ry][start_ix - rx] = '#';
    }

    for (int i = start_idx; word[i]; i++) {
        if (word[i] == 'F') {
            double next_x = cur_x + fast_cos(current_angle_deg);
            double next_y = cur_y - fast_sin(current_angle_deg); 

            int ix = fast_floor(next_x);
            int iy = fast_floor(next_y);

            if (ix < rx || ix >= rx + rw || iy < ry || iy >= ry + rh) {
                double angle_rad = current_angle_deg * 3.1415926535 / 180.0;
                send_handover(i + 1, next_x, next_y, angle_rad);
                return; 
            }
            grid[iy - ry][ix - rx] = '#';
            cur_x = next_x; cur_y = next_y;
        } else if (word[i] == '+') {
            current_angle_deg += g_angle;
        } else if (word[i] == '-') {
            current_angle_deg -= g_angle;
        }
    }
    double angle_rad = current_angle_deg * 3.1415926535 / 180.0;
    send_handover(strlen(word), cur_x, cur_y, angle_rad);
}

int main(int argc, char *argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0); 
    memset(grid, '.', sizeof(grid));
    init_lut();
    
    int my_id = NODE_ID;
    if(argc > 1) my_id = atoi(argv[1]);
    printf("Node %d starting... (LUT Enabled)\n", my_id);

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    
    struct timeval tv = {0, 0};
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_IP, &servaddr.sin_addr);

    // Rejestracja
    uint8_t buf[16];
    pack_header(buf, MSG_REGISTER, my_id, 0);
    buf[4] = alp_crc(buf, 4);
    
    printf("Sending REGISTER...\n");
    send_reliable(buf, 5);
    printf("REGISTERED!\n");

    uint8_t buffer[MAX_STR + 64];
    while (1) {
        struct timeval tv_zero = {0, 0};
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv_zero, sizeof tv_zero);

        int n = recvfrom(sockfd, buffer, sizeof(buffer), 0, NULL, NULL);
        if (n <= 0) continue;

        int type = (buffer[0]) & 0x0F;

        if (type == MSG_ASSIGN) {
            send_ack(sockfd, &servaddr); 
            rx = buffer[4]; ry = buffer[5];
            rw = buffer[6]; rh = buffer[7];
            g_angle = buffer[8];
            printf("ASSIGN: Region (%d,%d)\n", rx, ry);
        }
        else if (type == MSG_DATA) {
            send_ack(sockfd, &servaddr); 
            
            int idx = (buffer[4] << 8) | buffer[5];
            double sx, sy, sa;
            memcpy(&sx, &buffer[6], 8);
            memcpy(&sy, &buffer[14], 8);
            memcpy(&sa, &buffer[22], 8);
            
            char word[MAX_STR];
            int word_len = (buffer[2] << 8) | buffer[3];
            word_len -= 26; 

            memcpy(word, &buffer[30], word_len);
            word[word_len] = '\0';

            printf("TASK: Idx %d. Working...\n", idx);
            draw_turtle_smart(word, idx, sx, sy, sa);
        }
        else if (type == MSG_REQUEST) {
            // === POPRAWKA TUTAJ: Najpierw potwierdź (ACK), potem wyślij dane ===
            send_ack(sockfd, &servaddr);
            
            uint8_t resp[2048];
            int data_size = rw * rh;
            pack_header(resp, MSG_RESPONSE, my_id, data_size);
            int pos = 4;
            
            for(int y = 0; y < rh; y++) 
                for(int x = 0; x < rw; x++) 
                    resp[pos++] = grid[y][x];
            
            resp[pos++] = alp_crc(resp, pos); 
            
            printf("Request received. Sending %d bytes...\n", data_size);
            send_reliable(resp, pos);
            printf("Data sent & ACKed.\n");
        }
    }
    close(sockfd);
    return 0;
}
