#include <ZsutEthernet.h>
#include <ZsutEthernetUdp.h>
#include <ZsutFeatures.h>
 
#define PORT_E 12316
 
// typy wiadomości
#define MSG_FROM_U 0x01
#define MSG_TO_W   0x02
#define MSG_ACK    0x03
 
uint8_t packet[64];
uint8_t outPacket[64];
uint8_t SYSTEM_ID[16];
 
ZsutEthernetUDP Udp;
 
void setup() {
    byte mac[] = {0x08,0x00,0x27,0xD8,0x7D,0xF7};   // wymagany MAC
    ZsutEthernet.begin(mac);         // IP przydzielone przez EBSim
   
    Udp.begin(PORT_E);               
    ZsutIPAddress ip = ZsutEthernet.localIP();
    Serial.println(ip);
}
 
uint16_t readVirtualAngle() {
    return ZsutAnalog5Read();
}
 
int checkSystemID(uint8_t* buf) {
    for(int i = 0; i < 16; i++)
        if(buf[15 + i] != SYSTEM_ID[i]) return 0;
    return 1;
}
 
void sendToW(uint16_t diff) {
    memset(outPacket, 0, sizeof(outPacket));
 
    outPacket[0] = MSG_TO_W;
    outPacket[7] = (diff >> 8) & 0xFF;
    outPacket[8] = diff & 0xFF;
 
    for(int i=0;i<16;i++)
        outPacket[15+i] = SYSTEM_ID[i];
 
    ZsutIPAddress W_IP(10,6,86,91);
 
    Udp.beginPacket(W_IP, 8822);
    Udp.write(outPacket, 31);
    Udp.endPacket();
}
 
void loop() {
    int size = Udp.parsePacket();
    if(size > 0) {
        if(size > 64) size = 64;
        Udp.read(packet, size);

        if(packet[0] == MSG_ACK) {
            Serial.print("E: Received ACK from W");
            return;
        } else if (packet[0] != MSG_FROM_U) {
            Serial.println("E: ignored (wrong type)");
            return;
        }
 
        static bool system_id_initialized = false;
 
        if(!system_id_initialized) {
            for(int i = 0; i < 16; i++)
                SYSTEM_ID[i] = packet[15+i];
 
            system_id_initialized = true;
 
            Serial.print("E: Learned system ID: ");
            for(int i=0;i<16;i++) { Serial.print(SYSTEM_ID[i], HEX); Serial.print(" "); }
            Serial.println();
        } else if(!checkSystemID(packet)) {
            Serial.println("E: ignored (wrong system ID)");
            return;
        }
 
        uint16_t angleU = (packet[7] << 8) | packet[8];
        uint16_t angleE = readVirtualAngle();
 
        Serial.print("E: U="); Serial.print(angleU);
        Serial.print("  E="); Serial.println(angleE);
 
        if(angleE > angleU) {
            uint16_t diff = angleE - angleU;
            sendToW(diff);
        }
    }
}