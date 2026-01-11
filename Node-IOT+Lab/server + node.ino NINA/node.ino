#include <ZsutEthernet.h>
#include <ZsutEthernetUdp.h>
#include <ZsutFeatures.h>

#define ALP_VERSION  1
#define MSG_REGISTER 0x1
#define MSG_ASSIGN   0x2
#define MSG_DATA     0x3
#define MSG_ACK      0x4
#define MSG_REQUEST  0x5
#define MSG_RESPONSE 0x6
#define MSG_HANDOVER 0x7

// ============================================
// !!! TUTAJ ZMIEN NODE_ID NA 1, 2, 3 LUB 4 !!!
#define NODE_ID 4 
// !!! TUTAJ WPISZ IP SWOJEGO LINUXA (sprawdz ip a) !!!
#define SERVER_IP ZsutIPAddress(192,168,56,104) 
// ============================================

#define SERVER_PORT 8000
#define LOCAL_PORT  8000
#define MAX_REGION 32
#define BUF_SIZE 512

char grid[MAX_REGION][MAX_REGION];
int rx, ry, rw, rh;
ZsutEthernetUDP Udp;

uint8_t alp_crc(uint8_t *buf, int len){
    uint8_t crc=0; for(int i=0; i<len; i++) crc+=buf[i]; return crc;
}

void pack_header(uint8_t *buf, int type, uint8_t seq, int payload_len){
    buf[0] = (ALP_VERSION << 4) | (type & 0x0F);
    buf[1] = seq; buf[2] = NODE_ID;
    buf[3] = (payload_len >> 8) & 0xFF; buf[4] = payload_len & 0xFF;
}

void setup(){
    Serial.begin(9600);
    byte mac[]={0x02,0x00,0x00,0x00,0x00, NODE_ID}; 
    ZsutEthernet.begin(mac);
    Udp.begin(LOCAL_PORT);

    Serial.print("Node "); Serial.print(NODE_ID); 
    Serial.print(" IP: "); Serial.println(ZsutEthernet.localIP());
    Serial.print(" Target Server: "); Serial.println(SERVER_IP);

    memset(grid, '.', sizeof(grid));

    // Rejestracja
    uint8_t buf[8];
    pack_header(buf, MSG_REGISTER, 0, 0);
    buf[5] = alp_crc(buf, 5);
    Udp.beginPacket(SERVER_IP, SERVER_PORT);
    Udp.write(buf, 6);
    Udp.endPacket();
    Serial.println("Sent REGISTER");
}

void loop(){
    uint8_t buf[BUF_SIZE];
    int packetSize = Udp.parsePacket();
    
    if(packetSize > 0){
        Udp.read(buf, BUF_SIZE);
        int type = buf[0] & 0x0F;
        uint8_t seq = buf[1];
        int len = (buf[3] << 8) | buf[4];

        // LOGOWANIE ODBIORU
        Serial.print("Recv Type: "); Serial.println(type, HEX);

        if(type == MSG_ASSIGN){
            rx = buf[5]; ry = buf[6]; rw = buf[7]; rh = buf[8];
            Serial.print("ASSIGN: "); Serial.print(rx); Serial.print(","); Serial.println(ry);
            
            // ACK
            uint8_t b[6]; pack_header(b, MSG_ACK, seq, 0); b[5]=alp_crc(b,5);
            Udp.beginPacket(SERVER_IP, SERVER_PORT); Udp.write(b,6); Udp.endPacket();
        }
        else if(type == MSG_DATA){
            //Serial.println("DATA processing...");
            int16_t sx = (buf[5]<<8)|buf[6];
            int16_t sy = (buf[7]<<8)|buf[8];
            int16_t sa = (buf[9]<<8)|buf[10];

            double cx = sx/10.0; double cy = sy/10.0; int ca = sa;
            int cmds_len = len - 6;

            for(int i=0; i<cmds_len; i++){
                char cmd = buf[11+i];
                if(cmd=='F'){
                    double rad = ca*3.14159/180.0;
                    cx += cos(rad); cy += sin(rad);
                    int ix=(int)(cx+0.5); int iy=(int)(cy+0.5);
                    if(ix>=rx && ix<rx+rw && iy>=ry && iy<ry+rh)
                        grid[iy-ry][ix-rx] = '#';
                }
                else if(cmd=='+') ca=(ca+90)%360;
                else if(cmd=='-') { ca-=90; if(ca<0) ca+=360; }
            }
            delay(50);

            // HANDOVER
            uint8_t r[32]; pack_header(r, MSG_HANDOVER, seq, 6);
            int16_t nx=(int16_t)(cx*10); int16_t ny=(int16_t)(cy*10); int16_t na=(int16_t)ca;
            r[5]=(nx>>8)&0xFF; r[6]=nx&0xFF;
            r[7]=(ny>>8)&0xFF; r[8]=ny&0xFF;
            r[9]=(na>>8)&0xFF; r[10]=na&0xFF;
            r[11]=alp_crc(r,11);
            
            Udp.beginPacket(SERVER_IP, SERVER_PORT); Udp.write(r,12); Udp.endPacket();
            Serial.println("Sent HANDOVER");
        }
        else if(type == MSG_REQUEST){
            uint8_t out[1100]; pack_header(out, MSG_RESPONSE, seq, rw*rh);
            int p=5;
            for(int y=0; y<rh; y++) for(int x=0; x<rw; x++) out[p++]=grid[y][x];
            out[p++]=alp_crc(out,p);
            
            Udp.beginPacket(SERVER_IP, SERVER_PORT); Udp.write(out,p); Udp.endPacket();
            Serial.println("Sent RESULTS");
        }
    }
}