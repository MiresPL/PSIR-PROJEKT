#include <ZsutEthernet.h>
#include <ZsutEthernetUdp.h>
#include <ZsutFeatures.h>
#include <math.h>

#define ALP_VERSION  1
#define MSG_REGISTER 0x1
#define MSG_ASSIGN   0x2
#define MSG_DATA     0x3
#define MSG_ACK      0x4
#define MSG_REQUEST  0x5
#define MSG_RESPONSE 0x6
#define MSG_HANDOVER 0x7
#define MSG_REQ_COORDS  0x8
#define MSG_RESP_COORDS 0x9

// ============================================
// !!! IP TWOJEGO SERVERA (UPEWNIJ SIE ZE DOBRE) !!!
#define SERVER_IP ZsutIPAddress(192,168,89,10) 
// Ustaw odpowiednie ID dla każdego pliku hex (1, 2, 3, 4)
#define NODE_ID 1
// ============================================

#define SERVER_PORT 8000
#define LOCAL_PORT  (8000 + NODE_ID)

#define MAX_REGION 32
#define BUF_SIZE 512

char grid[MAX_REGION][MAX_REGION];
int rx = 0, ry = 0, rw = 0, rh = 0; // Inicjalizacja na 0
bool configured = false;

double turn_angle = 90.0;
double move_step = 1.0;

ZsutEthernetUDP Udp;

uint8_t alp_crc(uint8_t *buf, int len){
    uint8_t crc=0; for(int i=0; i<len; i++) crc+=buf[i]; return crc;
}

void pack_header(uint8_t *buf, int type, uint8_t seq, int payload_len){
    buf[0] = (ALP_VERSION << 4) | (type & 0x0F);
    buf[1] = seq; buf[2] = NODE_ID;
    buf[3] = (payload_len >> 8) & 0xFF; buf[4] = payload_len & 0xFF;
}

uint16_t readTemperature() {
    return ZsutAnalog5Read();
}

uint16_t readHumidity() {
    return ZsutAnalog1Read();
}


void setup(){
    Serial.begin(9600);
    byte mac[]={0x02,0x00,0x00,0x00,0x00, NODE_ID}; 
    ZsutEthernet.begin(mac);
    Udp.begin(LOCAL_PORT);

    Serial.print("Node "); Serial.print(NODE_ID); 
    Serial.print(" Port: "); Serial.println(LOCAL_PORT);
    
    memset(grid, '.', sizeof(grid));

    // Rejestracja (wysyłamy kilka razy dla pewności w setupie, ale serwer obsłuży duplikaty)
    for(int i=0; i<3; i++){
        uint8_t buf[8];
        pack_header(buf, MSG_REGISTER, 0, 0);
        buf[5] = alp_crc(buf, 5);
        Udp.beginPacket(SERVER_IP, SERVER_PORT);
        Udp.write(buf, 6);
        Udp.endPacket();
        delay(200);
    }
}

void loop(){
    uint8_t buf[BUF_SIZE];
    int packetSize = Udp.parsePacket();
    
    if(packetSize > 0){
        Udp.read(buf, BUF_SIZE);
        int type = buf[0] & 0x0F;
        uint8_t seq = buf[1];
        int len = (buf[3] << 8) | buf[4];

        if(type == MSG_ASSIGN){
            rx = buf[5]; ry = buf[6]; rw = buf[7]; rh = buf[8];
            
            int16_t ang = (buf[9] << 8) | buf[10];
            int16_t stp = (buf[11] << 8) | buf[12];
            
            turn_angle = (double)ang;
            move_step = (double)stp / 100.0;
            configured = true;

            Serial.print("ASSIGNED: "); Serial.print(rx); Serial.print(","); Serial.println(ry);

            // Odsyłamy ACK
            uint8_t b[6]; pack_header(b, MSG_ACK, seq, 0); b[5]=alp_crc(b,5);
            Udp.beginPacket(SERVER_IP, SERVER_PORT); Udp.write(b,6); Udp.endPacket();
        }
        else if(type == MSG_DATA){
            int16_t sx = (buf[5]<<8)|buf[6];
            int16_t sy = (buf[7]<<8)|buf[8];
            int16_t sa = (buf[9]<<8)|buf[10];

            double cx = sx/100.0; 
            double cy = sy/100.0; 
            int ca = sa;
            
            int cmds_len = len - 6;

            for(int i=0; i<cmds_len; i++){
                char cmd = buf[11+i];
                if(cmd=='F'){ 
                    // POPRAWKA: round() zamiast rzutowania (int)
                    // Naprawia błędy np. 19.999 -> 20
                    int ix = (int)round(cx); 
                    int iy = (int)round(cy);
                    
                    if(configured) {
                        int local_x = ix - rx;
                        int local_y = iy - ry;

                        if(local_x >= 0 && local_x < rw && local_y >= 0 && local_y < rh) {
                              grid[local_y][local_x] = '#';
                        }
                    }

                    double rad = (double)ca * 3.14159265 / 180.0;
                    cx += move_step * cos(rad); 
                    cy += move_step * sin(rad); 
                }
                else if(cmd=='+') { 
                    ca = (ca + (int)turn_angle) % 360;
                }
                else if(cmd=='-') { 
                    ca = ca - (int)turn_angle;
                    if(ca < 0) ca += 360;
                }
            }

            // Odsyłamy HANDOVER jako potwierdzenie
            uint8_t r[32]; pack_header(r, MSG_HANDOVER, seq, 6);
            int16_t nx=(int16_t)(cx*100); 
            int16_t ny=(int16_t)(cy*100); 
            int16_t na=(int16_t)ca;
            
            r[5]=(nx>>8)&0xFF; r[6]=nx&0xFF;
            r[7]=(ny>>8)&0xFF; r[8]=ny&0xFF;
            r[9]=(na>>8)&0xFF; r[10]=na&0xFF;
            r[11]=alp_crc(r,11);
            
            Udp.beginPacket(SERVER_IP, SERVER_PORT); Udp.write(r,12); Udp.endPacket();
        }
        else if(type == MSG_REQ_COORDS){
            Serial.println("REQ: Origin Coords requested.");
            
            // 1. Odczytujemy sensory
            // Z5 -> Temperatura -> Współrzędna X
            // Z1 -> Wilgotność  -> Współrzędna Y
            uint16_t raw_temp = readTemperature(); 
            uint16_t raw_hum  = readHumidity();

            // 2. Budujemy odpowiedź
            // Payload 4 bajty: [T_H, T_L, H_H, H_L]
            uint8_t resp[16];
            pack_header(resp, MSG_RESP_COORDS, seq, 4);

            resp[5] = (raw_temp >> 8) & 0xFF;
            resp[6] = raw_temp & 0xFF;
            resp[7] = (raw_hum >> 8) & 0xFF;
            resp[8] = raw_hum & 0xFF;

            // CRC całości (nagłówek + 4 bajty danych)
            resp[9] = alp_crc(resp, 9);

            // 3. Wysyłamy
            Udp.beginPacket(SERVER_IP, SERVER_PORT);
            Udp.write(resp, 10);
            Udp.endPacket();
            
            Serial.print("SENT: T="); Serial.print(raw_temp);
            Serial.print(" H="); Serial.println(raw_hum);
        }
        else if(type == MSG_REQUEST){
            // Dla pewności odsyłamy ACK zanim wyślemy dane (opcjonalne, ale zgodne z protokołem)
            // Ale tutaj RESPONSE pełni rolę danych.
            
            // Przygotowanie bufora na odpowiedź
            // Nagłówek 5 bajtów + dane + CRC
            // Dla 20x20 = 400 bajtów + 6 = 406 bajtów. Miesci się w 512.
            uint8_t out[600]; 
            int data_size = rw * rh;
            if(data_size == 0) data_size = 400; // Zabezpieczenie jakby nie był skonfigurowany
            
            pack_header(out, MSG_RESPONSE, seq, data_size);
            int p=5;
            for(int y=0; y<20; y++) { // Hardcoded 20 for safety if rh=0
                for(int x=0; x<20; x++) {
                     if(y < rh && x < rw) out[p++] = grid[y][x];
                     else out[p++] = '.';
                }
            }
            out[p++] = alp_crc(out,p);
            
            Udp.beginPacket(SERVER_IP, SERVER_PORT); Udp.write(out,p); Udp.endPacket();
        }
    }
}
