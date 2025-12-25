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

#define ALP_MAX_PAYLOAD 256


uint8_t alp_crc(uint8_t *buf, int len) {
    uint8_t crc = 0;
    for (int i = 0; i < len; i++)
        crc += buf[i];
    return crc;
}


/* ===================== CONFIG ===================== */

#define PORT 8000
#define MAX_STR   8192
#define MAX_RULES 16

#define GRID_WIDTH  40
#define GRID_HEIGHT 40
#define REGIONS_X 2
#define REGIONS_Y 2
#define MAX_NODES (REGIONS_X * REGIONS_Y)

/* ===================== NODE ===================== */

typedef struct {
    uint8_t node_id;
    struct sockaddr_in addr;
    int active;
} Node;

Node nodes[MAX_NODES];
int node_count = 0;

/* ===================== L-SYSTEM ===================== */

typedef struct {
    char symbol;
    char replacement[MAX_STR];
} Rule;

typedef struct {
    char alphabet[64];
    char axiom[MAX_STR];
    int iterations;
    int angle;
    Rule rules[MAX_RULES];
    int rule_count;
} LSystem;

/* ---------- load L-system from file ---------- */
int load_lsystem(const char *filename, LSystem *ls) {
    FILE *f = fopen(filename, "r");
    if (!f) {
        perror("Cannot open input.txt");
        return -1;
    }

    char line[256];
    ls->rule_count = 0;

    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n')
            continue;

        if (strncmp(line, "alphabet:", 9) == 0) {
            sscanf(line + 9, "%[^\n]", ls->alphabet);
        }
        else if (strncmp(line, "axiom:", 6) == 0) {
            sscanf(line + 6, "%s", ls->axiom);
        }
        else if (strncmp(line, "iterations:", 11) == 0) {
            ls->iterations = atoi(line + 11);
        }
        else if (strncmp(line, "angle:", 6) == 0) {
            ls->angle = atoi(line + 6);
        }
        else if (strncmp(line, "rule:", 5) == 0) {
            char sym;
            char rhs[MAX_STR];
            sscanf(line + 5, " %c=%s", &sym, rhs);

            ls->rules[ls->rule_count].symbol = sym;
            strcpy(ls->rules[ls->rule_count].replacement, rhs);
            ls->rule_count++;
        }
    }

    fclose(f);
    return 0;
}

/* ---------- generate final word ---------- */
void generate_lsystem(LSystem *ls, char *output) {
    char current[MAX_STR];
    char next[MAX_STR];

    strcpy(current, ls->axiom);

    for (int it = 0; it < ls->iterations; it++) {
        next[0] = '\0';

        for (int i = 0; current[i]; i++) {
            int replaced = 0;

            for (int r = 0; r < ls->rule_count; r++) {
                if (current[i] == ls->rules[r].symbol) {
                    strcat(next, ls->rules[r].replacement);
                    replaced = 1;
                    break;
                }
            }

            if (!replaced) {
                int len = strlen(next);
                next[len] = current[i];
                next[len + 1] = '\0';
            }
        }

        strcpy(current, next);
    }

    strcpy(output, current);
}

/* ===================== ASSIGN SENDER ===================== */

void send_assign(LSystem *ls, int sockfd, Node *node, int region_id) {
    uint8_t buf[32];
    int pos = 0;

    int region_w = GRID_WIDTH / REGIONS_X;
    int region_h = GRID_HEIGHT / REGIONS_Y;

    int rx = (region_id % REGIONS_X) * region_w;
    int ry = (region_id / REGIONS_X) * region_h;

    buf[pos++] = ALP_SYSTEM_ID;
    buf[pos++] = ALP_ASSIGN;
    buf[pos++] = node->node_id;
    buf[pos++] = region_id;

    buf[pos++] = 0;
    buf[pos++] = 5; // payload length

    buf[pos++] = rx;
    buf[pos++] = ry;
    buf[pos++] = region_w;
    buf[pos++] = region_h;
    buf[pos++] = ls->angle; // angle

    buf[pos++] = alp_crc(buf, pos);

    sendto(sockfd, buf, pos, 0,
           (struct sockaddr *)&node->addr,
           sizeof(node->addr));

    printf("ASSIGN -> node %d region %d (%d,%d %dx%d)\n",
           node->node_id, region_id, rx, ry, region_w, region_h);
}


/* ===================== MAIN ===================== */

int main() {
    /* ---------- L-SYSTEM PART ---------- */
    LSystem ls;
    char final_word[MAX_STR];

    if (load_lsystem("input.txt", &ls) != 0) {
        return 1;
    }

    generate_lsystem(&ls, final_word);

    printf("=== L-SYSTEM LOADED ===\n");
    printf("Alphabet: %s\n", ls.alphabet);
    printf("Axiom: %s\n", ls.axiom);
    printf("Iterations: %d\n", ls.iterations);
    printf("Angle: %d\n", ls.angle);
    printf("Rules:\n");
    for (int i = 0; i < ls.rule_count; i++) {
        printf("  %c -> %s\n",
               ls.rules[i].symbol,
               ls.rules[i].replacement);
    }
    printf("Final word length: %lu\n", strlen(final_word));
    printf("Final word:\n%s\n", final_word);
    printf("=======================\n\n");

    /* ---------- UDP SERVER PART ---------- */
    int sockfd;
    struct sockaddr_in servaddr, cliaddr;
    socklen_t len = sizeof(cliaddr);
    uint8_t buffer[256];

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(PORT);

    bind(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr));

    printf("Server listening on port %d\n", PORT);

    while (1) {
        int n = recvfrom(sockfd, buffer, sizeof(buffer), 0,
                         (struct sockaddr *)&cliaddr, &len);

        if (n <= 0) continue;

        if (buffer[0] != ALP_SYSTEM_ID) continue;

        if (buffer[1] == ALP_REGISTER && node_count < MAX_NODES) {
            Node *node = &nodes[node_count];
            node->node_id = buffer[2];
            node->addr = cliaddr;
            node->active = 1;

            printf("REGISTER from node %d\n", node->node_id);

            send_assign(&ls, sockfd, node, node_count);
            node_count++;
        }
    }
}
