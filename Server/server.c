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

// CONFIG
#define ALP_VERSION      1
#define MSG_REGISTER     0x1
#define MSG_ASSIGN       0x2
#define MSG_DATA         0x3
#define MSG_ACK          0x4
#define MSG_REQUEST      0x5
#define MSG_RESPONSE     0x6
#define MSG_HANDOVER     0x7

#define PORT 8000
#define MAX_STR    100000
#define GRID_WIDTH  40
#define GRID_HEIGHT 40
#define NODE_WIDTH  20
#define NODE_HEIGHT 20
#define MAX_NODES   4 
#define MY_PI 3.1415926535

typedef struct {
    uint8_t node_id;
    struct sockaddr_in addr;
    int rx, ry; 
} Node;

Node nodes[MAX_NODES];
int node_count = 0;
char global_grid[GRID_HEIGHT][GRID_WIDTH]; 

// L-System Structs
typedef struct { char symbol; char replacement[MAX_STR]; } Rule;
typedef struct { char axiom[MAX_STR]; int iterations; int angle; Rule rules[16]; int rule_count; } LSystem;

// --- MANUAL MATH ---
double normalize_angle(double x) {
    while (x > MY_PI) x -= 2 * MY_PI;
    while (x < -MY_PI) x += 2 * MY_PI;
    return x;
}
double my_sin(double x) {
    x = normalize_angle(x);
    double x2 = x * x;
    double res = x;
    res += -x * x2 / 6.0;
    res += x * x2 * x2 / 120.0;
    return res;
}
double my_cos(double x) { return my_sin(x + MY_PI / 2.0); }

// --- NETWORK ---
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
    uint8_t buf[5]; pack_header(buf, MSG_ACK, 0, 0); buf[4] = alp_crc(buf, 4);
    sendto(sockfd, buf, 5, 0, (struct sockaddr *)dest, sizeof(*dest));
}

int get_node_idx(int x, int y) {
    if(x<0) x=0; if(x>=GRID_WIDTH) x=GRID_WIDTH-1;
    if(y<0) y=0; if(y>=GRID_HEIGHT) y=GRID_HEIGHT-1;
    int col = x / NODE_WIDTH;
    int row = y / NODE_HEIGHT;
    int target = (row * 2) + col + 1;
    for(int i=0; i<node_count; i++) if(nodes[i].node_id == target) return i;
    return -1;
}

// --- L-SYSTEM ---
int load_lsystem(const char *f, LSystem *ls) {
    FILE *fp = fopen(f, "r");
    if(!fp) return -1;
    char line[256]; ls->rule_count = 0;
    while(fgets(line, sizeof(line), fp)) {
        if(line[0]=='#') continue;
        if(strncmp(line, "axiom:", 6)==0) sscanf(line+6, "%s", ls->axiom);
        else if(strncmp(line, "angle:", 6)==0) ls->angle = atoi(line+6);
        else if(strncmp(line, "iterations:", 11)==0) ls->iterations = atoi(line+11);
        else if(strncmp(line, "rule:", 5)==0) {
            char k; char v[MAX_STR]; sscanf(line+5, " %c=%s", &k, v);
            ls->rules[ls->rule_count].symbol=k; strcpy(ls->rules[ls->rule_count].replacement, v);
            ls->rules[ls->rule_count++].replacement[strlen(v)]=0;
        }
    }
    fclose(fp); return 0;
}
void generate_lsystem(LSystem *ls, char *out) {
    char cur[MAX_STR], next[MAX_STR];
    strcpy(cur, ls->axiom);
    for(int i=0; i<ls->iterations; i++) {
        next[0]=0;
        for(int j=0; cur[j]; j++) {
            int r = -1;
            for(int k=0; k<ls->rule_count; k++) if(cur[j]==ls->rules[k].symbol) r=k;
            if(r!=-1) strcat(next, ls->rules[r].replacement);
            else { int l=strlen(next); next[l]=cur[j]; next[l+1]=0; }
        }
        strcpy(cur, next);
    }
    strcpy(out, cur);
}

// --- MAIN ---
int main(int argc, char *argv[]) {
    if(argc<2) { printf("Usage: %s <file>\n", argv[0]); return 1; }
    
    LSystem ls; char final_str[MAX_STR];
    memset(global_grid, '.', sizeof(global_grid));
    load_lsystem(argv[1], &ls);
    generate_lsystem(&ls, final_str);
    printf("L-System: %lu chars\n", strlen(final_str));

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in serv, cli;
    memset(&serv, 0, sizeof(serv));
    serv.sin_family = AF_INET; serv.sin_addr.s_addr = INADDR_ANY; serv.sin_port = htons(PORT);
    bind(sockfd, (struct sockaddr*)&serv, sizeof(serv));

    printf("Waiting for nodes...\n");
    while(node_count < MAX_NODES) {
        socklen_t len = sizeof(cli); uint8_t buf[256];
        int n = recvfrom(sockfd, buf, sizeof(buf), 0, (struct sockaddr*)&cli, &len);
        if(n>0 && (buf[0]&0x0F)==MSG_REGISTER) {
            uint8_t id = buf[1];
            int known=0;
            for(int i=0;i<node_count;i++) if(nodes[i].node_id==id) known=1;
            if(!known) {
                nodes[node_count].node_id = id;
                nodes[node_count].addr = cli;
                nodes[node_count].rx = ((id-1)%2)*NODE_WIDTH;
                nodes[node_count].ry = ((id-1)/2)*NODE_HEIGHT;
                send_ack(sockfd, &cli);
                
                uint8_t as[32]; pack_header(as, MSG_ASSIGN, id, 6);
                as[4]=nodes[node_count].rx; as[5]=nodes[node_count].ry;
                as[6]=NODE_WIDTH; as[7]=NODE_HEIGHT; as[8]=ls.angle;
                as[9]=alp_crc(as, 9);
                sendto(sockfd, as, 10, 0, (struct sockaddr*)&cli, len);
                printf("Node %d Reg. Region %d,%d. Port %d\n", id, nodes[node_count].rx, nodes[node_count].ry, ntohs(cli.sin_port));
                node_count++;
            }
        }
    }
    sleep(1);

    // --- SIMULATION ---
    double cx=19.5, cy=25.0, ca=0; // Start Center Up
    int str_idx = 0;
    int total = strlen(final_str);
    int curr_node = get_node_idx((int)cx, (int)cy);

    printf("Starting Stream...\n");
    struct timeval tv = {0, 400000}; // INCREASED TIMEOUT TO 400ms
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);

    while(str_idx < total) {
        int chunk = 50; 
        if(str_idx + chunk > total) chunk = total - str_idx;
        
        // Handle OOB
        if(curr_node == -1) {
            printf("WARN: Turtle OOB at %.1f,%.1f. Simulating blindly.\n", cx, cy);
            for(int k=0; k<chunk; k++) {
                char c = final_str[str_idx+k];
                if(c=='F') {
                    cx += my_cos(ca * MY_PI/180.0);
                    cy += my_sin(ca * MY_PI/180.0);
                } else if(c=='+') ca += ls.angle;
                else if(c=='-') ca -= ls.angle;
            }
            str_idx += chunk;
            curr_node = get_node_idx((int)cx, (int)cy);
            continue; 
        }

        uint8_t pkt[256];
        pack_header(pkt, MSG_DATA, nodes[curr_node].node_id, 12 + chunk);
        float fx=(float)cx; float fy=(float)cy; float fa=(float)ca;
        memcpy(&pkt[4], &fx, 4); memcpy(&pkt[8], &fy, 4); memcpy(&pkt[12], &fa, 4);
        memcpy(&pkt[16], &final_str[str_idx], chunk);
        pkt[4+12+chunk] = alp_crc(pkt, 4+12+chunk);

        int success = 0;
        for(int r=0; r<5; r++) { 
            // Debug print
            printf("Sending to Node %d (Attempt %d)...\n", nodes[curr_node].node_id, r+1);
            
            sendto(sockfd, pkt, 5+12+chunk, 0, (struct sockaddr*)&nodes[curr_node].addr, sizeof(nodes[curr_node].addr));
            
            uint8_t resp[256]; socklen_t l = sizeof(cli);
            int n = recvfrom(sockfd, resp, sizeof(resp), 0, (struct sockaddr*)&cli, &l);
            
            if(n>0) {
                int type = resp[0] & 0x0F;
                if(type == MSG_HANDOVER) {
                    nodes[curr_node].addr = cli;
                    float nx, ny, na; uint16_t proc;
                    memcpy(&nx, &resp[4], 4); memcpy(&ny, &resp[8], 4); memcpy(&na, &resp[12], 4);
                    proc = (resp[16]<<8) | resp[17];
                    
                    printf("Handover Node %d -> %.1f,%.1f. Processed %d\n", nodes[curr_node].node_id, nx, ny, proc);
                    cx=nx; cy=ny; ca=na;
                    str_idx += proc;
                    curr_node = get_node_idx((int)cx, (int)cy);
                    success = 1;
                    send_ack(sockfd, &cli);
                    
                    // IMPORTANT: SLEEP TO LET NETWORK SETTLE
                    usleep(50000); 
                    break;
                }
                else if(type == MSG_ACK) {
                    for(int k=0; k<chunk; k++) {
                        char c = final_str[str_idx+k];
                        if(c=='F') {
                            cx += my_cos(ca * MY_PI/180.0);
                            cy += my_sin(ca * MY_PI/180.0);
                        } 
                        else if(c=='+') ca += ls.angle;
                        else if(c=='-') ca -= ls.angle;
                    }
                    str_idx += chunk;
                    success = 1; 
                    printf("Node %d ACKed.\n", nodes[curr_node].node_id);
                    break;
                }
            }
        }
        if(!success) { 
            printf("Timeout Node %d. Skipping chunk.\n", nodes[curr_node].node_id); 
            str_idx+=chunk; 
        }
    }

    // --- COLLECTION ---
    printf("Collecting...\n");
    for(int i=0; i<node_count; i++) {
        int sx = nodes[i].rx; int sy = nodes[i].ry;
        for(int r=0; r<NODE_HEIGHT; r++) {
            uint8_t rq[8]; pack_header(rq, MSG_REQUEST, nodes[i].node_id, 1);
            rq[4]=r; rq[5]=alp_crc(rq, 5);
            
            for(int try=0; try<5; try++) {
                sendto(sockfd, rq, 6, 0, (struct sockaddr*)&nodes[i].addr, sizeof(nodes[i].addr));
                uint8_t rb[64]; socklen_t l=sizeof(cli);
                if(recvfrom(sockfd, rb, sizeof(rb), 0, (struct sockaddr*)&cli, &l) > 0) {
                    if((rb[0]&0x0F)==MSG_RESPONSE) {
                         memcpy(&global_grid[sy+r][sx], &rb[4], NODE_WIDTH);
                         break;
                    }
                }
            }
        }
    }

    printf("\n=== RESULT ===\n");
    for(int y=0; y<GRID_HEIGHT; y++) {
        for(int x=0; x<GRID_WIDTH; x++) putchar(global_grid[y][x]);
        putchar('\n');
    }
    return 0;
}