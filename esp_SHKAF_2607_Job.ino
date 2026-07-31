#include <WiFi.h>
#include <esp_now.h>
#include "esp_bt.h"
#include "esp_wifi.h"

const int RX_PLC_PIN = 27;
const int TX_PLC_PIN = 33;

uint8_t broadcastMac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

volatile unsigned long pulseStartTime = 0;
volatile unsigned long pulseDuration = 0;
volatile bool pulseReadyToProcess = false;

// === Для обработки нажатий от Ноды (пины 0/1 → треки 25/27) ===
volatile bool nodeButtonReady = false;
volatile int nodeButtonTrack = 0;

// === НОВОЕ: КОЛЬЦЕВАЯ ОЧЕРЕДЬ КОМАНД В ПЛК (10 штук) ===
String plcQueue[10];
int queueHead = 0;       // откуда берём
int queueCount = 0;      // сколько в очереди
bool isSending = false;   // Шкаф сейчас отправляет в ПЛК?

void sendPlcStringSimple(String val) {
  if (val.length() != 8) return;
  for (int i = 0; i < 8; i++) {
    int digit = val.charAt(i) - '0';
    if (digit < 0 || digit > 9) continue;
    long pulseCount = (digit * 100) + 50; 
    for (long j = 0; j < pulseCount; j++) {
      digitalWrite(TX_PLC_PIN, HIGH);
      delayMicroseconds(50);
      digitalWrite(TX_PLC_PIN, LOW);
      delayMicroseconds(100);
    }
    delay(35); 
  }
}

// === НОВОЕ: положить команду в очередь ===
void enqueuePlcCommand(String cmd) {
  if (queueCount >= 10) {
    Serial.println("⚠ queue FULL, drop");
    return;
  }
  int tail = (queueHead + queueCount) % 10;
  plcQueue[tail] = cmd;
  queueCount++;
  Serial.print("📋 enqueue: ");
  Serial.print(cmd);
  Serial.print(" (size=");
  Serial.print(queueCount);
  Serial.println(")");
}

// === НОВОЕ: обработать очередь (вызывать из loop) ===
void processPlcQueue() {
  if (isSending || queueCount == 0) return;
  
  String cmd = plcQueue[queueHead];
  queueHead = (queueHead + 1) % 10;
  queueCount--;
  
  isSending = true;
  Serial.print("📤 PLC: ");
  Serial.println(cmd);
  sendPlcStringSimple(cmd);
  isSending = false;
}

void IRAM_ATTR iasPulseHandler() {
  unsigned long currentTime = millis();
  if (digitalRead(RX_PLC_PIN) == LOW) {
    pulseStartTime = currentTime;
  } else {
    if (pulseStartTime > 0) {
      pulseDuration = currentTime - pulseStartTime;
      pulseReadyToProcess = true;
      pulseStartTime = 0;
    }
  }
}

void onDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len) {
  if (len < 2) return;
  
  char buf[16];
  int copy_len = (len > 15) ? 15 : len;
  memcpy(buf, incomingData, copy_len);
  buf[copy_len] = '\0';
  
  String msg = String(buf);
  
  // === ЛОГИКА 1: трек от ПЛК ("A13" / "A15") ===
  if (msg.startsWith("A")) {
    String trackStr = msg.substring(1); 
    if (trackStr.toInt() > 0) {
      enqueuePlcCommand(trackStr + "000000");   // ← БЫЛО: sendPlcStringSimple
    }
    return;
  }
  
  // === ЛОГИКА 2: кнопки от Ноды ("BS_пин_состояние" / "BSS_пин_состояние") ===
  if (msg.startsWith("BS_") || msg.startsWith("BSS_")) {
    int firstUnderscore = msg.indexOf('_');
    int secondUnderscore = msg.indexOf('_', firstUnderscore + 1);
    
    if (firstUnderscore > 0 && secondUnderscore > 0) {
      String prefix = msg.substring(0, firstUnderscore);
      String pinStr = msg.substring(firstUnderscore + 1, secondUnderscore);
      String stateStr = msg.substring(secondUnderscore + 1);
      
      int pin = pinStr.toInt();
      int state = stateStr.toInt();
      
      // === Длинные пины 0/1 → треки 25/27 (как длинный импульс) ===
      if (state == 1 && pin == 0) {
        nodeButtonTrack = 25;
        nodeButtonReady = true;
        return;
      }
      if (state == 1 && pin == 1) {
        nodeButtonTrack = 27;
        nodeButtonReady = true;
        return;
      }
      
      // === Остальные пины → команды в ПЛК (через очередь) ===
      String plcMessage = (prefix == "BS") ? "99" : "98";
      if (pin < 10) plcMessage += "0" + String(pin);
      else plcMessage += String(pin);
      plcMessage += String(state) + "000";
      
      enqueuePlcCommand(plcMessage);             // ← БЫЛО: sendPlcStringSimple
    }
    return;
  }
}

void setup() {
  esp_bt_controller_disable();

  pinMode(RX_PLC_PIN, INPUT); 
  attachInterrupt(digitalPinToInterrupt(RX_PLC_PIN), iasPulseHandler, CHANGE);
  pinMode(TX_PLC_PIN, OUTPUT);
  digitalWrite(TX_PLC_PIN, LOW);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(); 
  WiFi.setSleep(false);
  esp_wifi_set_max_tx_power(127);
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) return;
  esp_now_register_recv_cb(esp_now_recv_cb_t(onDataRecv));

  esp_now_peer_info_t peerInfo;
  memset(&peerInfo, 0, sizeof(peerInfo));
  memcpy(peerInfo.peer_addr, broadcastMac, 6);
  peerInfo.channel = 1; 
  peerInfo.encrypt = false;
  esp_now_add_peer(&peerInfo);

  WiFi.setTxPower(WIFI_POWER_8_5dBm);
}

void loop() {
  // === СТАРОЕ: импульс от ПЛК Шкафа (1с → 13, 2с → 15) ===
  if (pulseReadyToProcess) {
    int detectedTrack = 0;
    if (pulseDuration >= 700 && pulseDuration <= 1300) {
      detectedTrack = 13; 
    }
    else if (pulseDuration >= 1700 && pulseDuration <= 2300) {
      detectedTrack = 15; 
    }

    if (detectedTrack > 0) {
      String payload = "A" + String(detectedTrack);
      int msgLen = payload.length();
      uint8_t dataBuf[16];
      payload.getBytes(dataBuf, msgLen + 1);
      esp_now_send(broadcastMac, dataBuf, msgLen);
    }

    pulseReadyToProcess = false;
  }
  
  // === Нода прислала нажатие пина 0/1 → отправляем A25/A27 в Телегу ===
  if (nodeButtonReady) {
    String payload = "A" + String(nodeButtonTrack);
    int msgLen = payload.length();
    uint8_t dataBuf[16];
    payload.getBytes(dataBuf, msgLen + 1);
    esp_now_send(broadcastMac, dataBuf, msgLen);
    nodeButtonReady = false;
  }
  
  // === НОВОЕ: обработка очереди команд в ПЛК ===
  processPlcQueue();
}
