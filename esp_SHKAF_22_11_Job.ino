#include <WiFi.h>
#include <esp_now.h>
#include "esp_bt.h"
#include "esp_wifi.h"
#include <ModbusMaster.h>
#include <HardwareSerial.h>

// ================= Modbus =================
HardwareSerial modbusSerial(2);
ModbusMaster  node;

const int RX_PIN = 16;          // ←  пины RS-485
const int TX_PIN = 17;
uint8_t  modbusSlaveId = 20;    // ← адрес ПР102
uint32_t modbusBaud    = 57600; // ← скорость

// Адреса регистров 
const uint16_t REG_CMD   = 16400; // запись команды (9001/8002/NNNN)
const uint16_t REG_ACK   = 16401; // подтверждение = 1
const uint16_t REG_EVENT = 16402; // событие 0..60
const uint16_t REG_FLAG  = 16403; // флаг нового события = 1

// ===== RS-регистры (задел на будущее) =====
#define RS_MAX 99
const uint16_t REG_RS[RS_MAX] = {
 16404,    // REG_RS1  
  16405,    // REG_RS2  
  16406,    // REG_RS3 
  16407,    // REG_RS4  
  
};

// ================= ESP-NOW =================
uint8_t broadcastMac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ================= Очередь в ПЛК =================
String plcQueue[10];
int queueHead = 0;
int queueCount = 0;
bool isSending = false;

// ===== Очередь RS-команд =====
struct RsCmd { uint8_t n; uint16_t val; };
RsCmd rsQueue[10];
int  rsHead = 0, rsCount = 0;
bool rsSending = false;

// ================= Служебное =================
volatile bool nodeButtonReady = false;   // мёртвый код
volatile int  nodeButtonTrack = 0;

// ================= Modbus helpers =================
bool modbusReadReg(uint16_t reg, uint16_t &out) {
  uint8_t r = node.readHoldingRegisters(reg, 1);
  if (r == node.ku8MBSuccess) { out = node.getResponseBuffer(0); return true; }
  return false;
}

uint8_t modbusWriteReg(uint16_t reg, uint16_t val) {
  node.setTransmitBuffer(0, val);
  return node.writeMultipleRegisters(reg, 1);
}

// ================= Очередь =================
void enqueuePlcCommand(String cmd) {
  cmd.trim();
  String digitsOnly = "";
  for (int i = 0; i < cmd.length(); i++) {
    char c = cmd.charAt(i);
    if (c >= '0' && c <= '9') digitsOnly += c;
  }
  while (digitsOnly.length() < 4) digitsOnly = "0" + digitsOnly;
  if (digitsOnly.length() > 4) digitsOnly = digitsOnly.substring(digitsOnly.length() - 4);

  if (queueCount >= 10) return;                 // молча отбрасываем
  int tail = (queueHead + queueCount) % 10;
  plcQueue[tail] = digitsOnly;
  queueCount++;
}

void processPlcQueue() {
  if (isSending || queueCount == 0) return;

  String cmd = plcQueue[queueHead];
  queueHead = (queueHead + 1) % 10;
  queueCount--;

  isSending = true;
  uint16_t val = (uint16_t)cmd.toInt();

  // 1) команда в REG_CMD
  uint8_t r = modbusWriteReg(REG_CMD, val);
  // 2) подтверждение = 1 только при успехе
  if (r == node.ku8MBSuccess) {
    modbusWriteReg(REG_ACK, 1);
  }
  isSending = false;
}

// ===== RS-очередь =====
void enqueueRs(uint8_t n, uint16_t val) {
  if (n < 1 || n > RS_MAX) return;
  if (REG_RS[n - 1] == 0) return;          // регистр не назначен — игнор
  if (rsCount >= 10) return;               // переполнение — молча
  int tail = (rsHead + rsCount) % 10;
  rsQueue[tail].n = n;
  rsQueue[tail].val = val;
  rsCount++;
}

void processRsQueue() {
  if (rsSending || rsCount == 0) return;
  rsSending = true;
  RsCmd c = rsQueue[rsHead];
  rsHead = (rsHead + 1) % 10;
  rsCount--;

  if (c.n >= 1 && c.n <= RS_MAX && REG_RS[c.n - 1] != 0) {
    modbusWriteReg(REG_RS[c.n - 1], c.val);
  }
  rsSending = false;
}

// ================= ESP-NOW send =================
void sendEspNow(const String &payload) {
  uint8_t buf[16];
  payload.getBytes(buf, payload.length() + 1);
  esp_now_send(broadcastMac, buf, payload.length());
}

// ================= ESP-NOW recv =================
void onDataRecv(const uint8_t *mac, const uint8_t *incomingData, int len) {
  if (len < 2) return;
  char buf[16];
  int copy_len = (len > 15) ? 15 : len;
  memcpy(buf, incomingData, copy_len);
  buf[copy_len] = '\0';

  String msg = String(buf);
  msg.trim();

  if (msg == "BS_0_1") {
    sendEspNow("A25");
    return;
  }
  else if (msg == "BSS_1_1") {
    sendEspNow("A41");
    return;
  }
  else if (msg.startsWith("BS_") || msg.startsWith("BSS_")) {
    int u1 = msg.indexOf('_');
    int u2 = msg.indexOf('_', u1 + 1);
    if (u1 > 0 && u2 > 0) {
      String pinStr   = msg.substring(u1 + 1, u2);
      String stateStr = msg.substring(u2 + 1);
      char pinBuf[3];
      snprintf(pinBuf, sizeof(pinBuf), "%02d", pinStr.toInt());
      String plcMessage = (msg.startsWith("BS")) ? "9" : "8";
      plcMessage += String(pinBuf);
      plcMessage += stateStr;
      enqueuePlcCommand(plcMessage);
    }
  }
  else if (msg.startsWith("A")) {
    String trackStr = msg.substring(1);
    if (trackStr.toInt() > 0) enqueuePlcCommand(trackStr);
  }
  // ===== RS-ветка =====
  else if (msg.indexOf("RS") >= 0) {
    int rsPos = msg.indexOf("RS");
    String numPart = msg.substring(0, rsPos);
    String valPart = msg.substring(rsPos + 2);
    numPart.trim(); valPart.trim();

    int  n = numPart.toInt();
    long v = valPart.toInt();

    if (n >= 1 && n <= RS_MAX && v >= 0 && v <= 65535) {
      enqueueRs((uint8_t)n, (uint16_t)v);
    }
  }
}

// ================= setup =================
void setup() {
  delay(500);
  esp_bt_controller_disable();

  // Modbus init
  modbusSerial.begin(modbusBaud, SERIAL_8N1, RX_PIN, TX_PIN);
  node.begin(modbusSlaveId, modbusSerial);

  // WiFi / ESP-NOW
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

// ================= loop =================
void loop() {
  static unsigned long lastPoll = 0;

  // ===== Счётчик-флаг событий: ESP только ЧИТАЕТ, НЕ пишет =====
  static uint16_t prevFlag = 0;
  static bool flagFirstPoll = true;

  if (millis() - lastPoll >= 150) {
    lastPoll = millis();

    uint16_t flag = 0;
    if (modbusReadReg(REG_FLAG, flag)) {
      if (flagFirstPoll) {
        prevFlag = flag;              // первый опрос — запоминаем, ничего не шлём
        flagFirstPoll = false;
      } else if (flag != prevFlag) {  // счётчик изменился → новое событие
        prevFlag = flag;
        uint16_t event = 0;
        if (modbusReadReg(REG_EVENT, event) && event != 0) {
          sendEspNow("A" + String(event));
        }
      }
    }
  }

  // === Мёртвый код  ===
  if (nodeButtonReady) {
    sendEspNow("A" + String(nodeButtonTrack));
    nodeButtonReady = false;
  }

  // === Очередь в ПЛК ===
  processPlcQueue();
  processRsQueue();   // ===== RS-очередь
}