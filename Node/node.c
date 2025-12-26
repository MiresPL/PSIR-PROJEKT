#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>

/* ================= ALP ================= */

#define ALP_SYSTEM_ID  0xA5

#define ALP_REGISTER   0x01
#define ALP_ASSIGN     0x02
#define ALP_DATA       0x03
#define ALP_ACK        0x04
#define ALP_REQUEST_DATA 0x05
#define ALP_RESPONSE_DATA 0x06

#define ALP_MAX_PAYLOAD 256

uint8_t alp_crc(uint8_t *buf, int len) {
    uint8_t crc = 0;
    for (int i = 0; i < len; i++)
        crc += buf[i];
    return crc;
}

/* ================= CONFIG ================= */
#define SERVER_IP   "192.168.89.10"
#define SERVER_PORT 8000
#define NODE_ID     1

#define MAX_REGION  32

#define MAX_STR   8192
#define MAX_RULES 16


/* ================= L-SYSTEM ================= */

typedef struct {
    char symbol;
    char replacement[MAX_STR];
} Rule;

typedef struct {
    char axiom[MAX_STR];
    int iterations;
    int angle;
    Rule rules[MAX_RULES];
    int rule_count;
} LSystem;

/* ===================== REGISTER SENDER ===================== */

void send_register(int sockfd, struct sockaddr_in *servaddr) {
    uint8_t buf[16];
    int pos = 0;

    buf[pos++] = ALP_SYSTEM_ID;
    buf[pos++] = ALP_REGISTER;
    buf[pos++] = NODE_ID;
    buf[pos++] = 0;

    buf[pos++] = 0;
    buf[pos++] = 0;

    buf[pos++] = alp_crc(buf, pos);

    sendto(sockfd, buf, pos, 0,
           (struct sockaddr *)servaddr,
           sizeof(*servaddr));

    printf("REGISTER sent (node %d)\n", NODE_ID);
}

/* ===================== TURTLE DEBUGGER ===================== */

void print_region(char region[MAX_REGION][MAX_REGION], int w, int h) {
    printf("=== NODE %d REGION DATA ===\n", NODE_ID);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) printf("%c", region[y][x]);
        printf("\n");
    }
    printf("===========================\n");
}

/* ===================== RESPONSE DATA SENDER ===================== */

void send_response_data(int sockfd, struct sockaddr_in *servaddr, uint8_t region_id) {
    uint8_t buf[16];
    int pos = 0;

    buf[pos++] = ALP_SYSTEM_ID;
    buf[pos++] = ALP_RESPONSE_DATA;
    buf[pos++] = NODE_ID;
    buf[pos++] = region_id;

    buf[pos++] = 0;
    buf[pos++] = 0;

    buf[pos++] = alp_crc(buf, pos);

    sendto(sockfd, buf, pos, 0, (struct sockaddr *) servaddr, sizeof(*servaddr));
    printf("RESPONSE_DATA sent (node %d)\n", NODE_ID);
}


/* ================= MAIN ================= */

int main() {
    int sockfd;
    struct sockaddr_in servaddr, cliaddr;
    socklen_t len = sizeof(cliaddr);
    uint8_t buffer[256];

    char region[MAX_REGION][MAX_REGION];
    memset(region, '.', sizeof(region));

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_IP, &servaddr.sin_addr);

    send_register(sockfd, &servaddr);

    int region_x = 0, region_y = 0;
    int region_w = 0, region_h = 0;
    int angle = 0;

    while (1) {
        int n = recvfrom(sockfd, buffer, sizeof(buffer), 0, (struct sockaddr *) &cliaddr, &len);

        if (n <= 0) continue;
        if (buffer[0] != ALP_SYSTEM_ID) continue;

        if (buffer[1] == ALP_ASSIGN) {
            region_x = buffer[6];
            region_y = buffer[7];
            region_w = buffer[8];
            region_h = buffer[9];
            angle    = buffer[10];

            printf("ASSIGN received:\n");
            printf("Region (%d,%d) size %dx%d angle %d\n",
                   region_x, region_y, region_w, region_h, angle);
        }

        if (buffer[1] == ALP_DATA) {
            int x = buffer[6];
            int y = buffer[7];
            char val = buffer[8];

            int lx = x - region_x;
            int ly = y - region_y;

            if (lx >= 0 && lx < region_w && ly >= 0 && ly < region_h) {
                region[ly][lx] = val;
            }
        }

        if (buffer[1] == ALP_REQUEST_DATA) {
            printf("REQUEST_DATA received\n");

            print_region(region, region_w, region_h);

            send_response_data(sockfd, &servaddr, buffer[3]);
        }

    }
    printf("Node ready for turtle + DATA stage\n");
}
