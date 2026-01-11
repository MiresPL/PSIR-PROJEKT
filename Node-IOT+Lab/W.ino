#include <WiFi.h>
#include <WiFiUdp.h>
 
#define PORT_W 8822
 
#define MSG_TO_W 0x02
#define MSG_ACK  0x03
 
WiFiUDP Udp;
uint8_t packet[64];
uint8_t sendBuf[64];
uint8_t SYSTEM_ID[16];
 
bool system_id_initialized = false;
  
bool toneActive = false;
unsigned long toneEnd = 0;
unsigned long lastToggle = 0;
 
int halfPeriod = 500;
 
void setup() {
    Serial.begin(115200);
 
    WiFi.begin("psir25zv686d", "wifipsir25zv686zlab2");
    while (WiFi.status() != WL_CONNECTED) delay(100);
 
    Serial.print("W: IP=");
    Serial.println(WiFi.localIP());
 
    Udp.begin(PORT_W);
 
    pinMode(D2, OUTPUT);
}
 
int checkSystemID(uint8_t *buf){
    for(int i=0;i<16;i++)
        if(buf[15+i] != SYSTEM_ID[i]) return 0;
    return 1;
}
 
void startTone(uint16_t diff){
    if(diff == 0) diff = 1;
 
    // częstotliwość = diff Hz → okres = 1/diff
    // półokres (toggle) = 1/(2*diff)
    halfPeriod = (1000000 / (diff * 2));
 
    toneActive = true;
    toneEnd = millis() + 1450;
}
 
void stopTone() {
    digitalWrite(D2, LOW);
    toneActive = false;
}
 
void sendAck(IPAddress ip, uint16_t port) {
    memset(sendBuf, 0, sizeof(sendBuf));
    sendBuf[0] = MSG_ACK;
    for(int i=0;i<16;i++)
        sendBuf[15+i] = SYSTEM_ID[i];
 
    Udp.beginPacket(ip, port);
    Udp.write(sendBuf, 31);
    Udp.endPacket();
}
 
void loop() {
    if (toneActive) {
        unsigned long now = micros();
 
        if (now - lastToggle >= halfPeriod) {
            lastToggle = now;
            digitalWrite(D2, digitalRead(D2) ^ 1);
            delayMicroseconds(halfPeriod);
        }
 
        if (millis() >= toneEnd) {
            stopTone();
        }
    }


    int size = Udp.parsePacket();
    if (size > 0) {
        if (size > 64) size = 64;
        Udp.read(packet, size);
 
        if (packet[0] != MSG_TO_W) return;
        if(!system_id_initialized) {
            for(int i=0;i<16;i++)
                SYSTEM_ID[i] = packet[15+i];
 
            system_id_initialized = true;
 
            Serial.print("W: Learned system ID: ");
            for(int i=0;i<16;i++) { Serial.print(SYSTEM_ID[i], HEX); Serial.print(" "); }
            Serial.println();
        }
        else if(!checkSystemID(packet)) {
            Serial.println("W: ignored (wrong system ID)");
            return;
        }
 
        uint16_t diff = (packet[7] << 8) | packet[8];
 
        Serial.print("W: diff=");
        Serial.println(diff);
 
        startTone(diff);
        sendAck(Udp.remoteIP(), Udp.remotePort());
    }
}