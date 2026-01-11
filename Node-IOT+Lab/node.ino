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

#define NODE_ID 1
#define SERVER_IP ZsutIPAddress(192,168,89,10)
#define SERVER_PORT 8000

#define MAX_REGION 32
#define BUF_SIZE 256

char grid[MAX_REGION][MAX_REGION];

int rx, ry, rw, rh;
int g_angle;
int cur_x, cur_y;
int dir;   // 0=up 1=right 2=down 3=left

ZsutEthernetUDP Udp;

/* ================= CRC ================= */
uint8_t alp_crc(uint8_t *buf,int len){
    uint8_t crc=0;
    for(int i=0;i<len;i++) crc+=buf[i];
    return crc;
}

void pack_header(uint8_t *buf,int type,int payload){
    buf[0]=(ALP_VERSION<<4)|(type&0x0F);
    buf[1]=NODE_ID;
    buf[2]=(payload>>8)&0xFF;
    buf[3]=payload&0xFF;
}

/* ================= ACK ================= */
void send_ack(){
    uint8_t b[5];
    pack_header(b,MSG_ACK,0);
    b[4]=alp_crc(b,4);
    Udp.beginPacket(SERVER_IP,SERVER_PORT);
    Udp.write(b,5);
    Udp.endPacket();
}

/* ================= Move ================= */
void step_forward(){
    if(dir==0) cur_y--;
    if(dir==1) cur_x++;
    if(dir==2) cur_y++;
    if(dir==3) cur_x--;

    if(cur_x>=rx && cur_x<rx+rw && cur_y>=ry && cur_y<ry+rh){
        grid[cur_y-ry][cur_x-rx] = '#';
    }
}

/* ================= Arduino ================= */

void setup(){
    Serial.begin(9600);

    byte mac[]={0x08,0x00,0x27,0xAA,0xBB,NODE_ID};
    ZsutEthernet.begin(mac);
    Udp.begin(8000);

    Serial.print("Node IP: ");
    Serial.println(ZsutEthernet.localIP());

    memset(grid,'.',sizeof(grid));

    // REGISTER
    uint8_t buf[8];
    pack_header(buf,MSG_REGISTER,0);
    buf[4]=alp_crc(buf,4);
    Udp.beginPacket(SERVER_IP,SERVER_PORT);
    Udp.write(buf,5);
    Udp.endPacket();

    Serial.println("REGISTER sent");
}

void loop(){
    uint8_t buf[BUF_SIZE];
    int n = Udp.parsePacket();
    if(n<=0) return;
    if(n>BUF_SIZE) n=BUF_SIZE;
    Udp.read(buf,n);

    int type = buf[0] & 0x0F;

    if(type==MSG_ASSIGN){
        send_ack();
        rx=buf[4]; ry=buf[5];
        rw=buf[6]; rh=buf[7];
        g_angle=buf[8];

        cur_x = rx + rw/2;
        cur_y = ry + rh/2;
        dir = 0;

        Serial.print("ASSIGN rx="); Serial.print(rx);
        Serial.print(" ry="); Serial.print(ry);
        Serial.print(" w="); Serial.print(rw);
        Serial.print(" h="); Serial.println(rh);
    }

    else if(type==MSG_DATA){
        send_ack();

        int len = ((buf[2]<<8)|buf[3]);

        Serial.print("DATA ");
        Serial.print(len);
        Serial.println(" bytes");

        for(int i=0;i<len;i++){
            char c = buf[4+i];
            if(c=='F') step_forward();
            else if(c=='+') dir=(dir+1)&3;
            else if(c=='-') dir=(dir+3)&3;
        }
    }

    else if(type==MSG_REQUEST){
        send_ack();

        uint8_t out[1100];
        int sz = rw*rh;
        pack_header(out,MSG_RESPONSE,sz);

        int p=4;
        for(int y=0;y<rh;y++)
            for(int x=0;x<rw;x++)
                out[p++] = grid[y][x];

        out[p++] = alp_crc(out,p);

        Udp.beginPacket(SERVER_IP,SERVER_PORT);
        Udp.write(out,p);
        Udp.endPacket();

        Serial.println("Region sent");
    }
}
