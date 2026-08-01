#include <WiFi.h>
#include <esp_now.h>
#include "esp_bt.h"
#include "esp_wifi.h"
#include <driver/rmt.h>
#define RMT_CHANNEL RMT_CHANNEL_0
#define RMT_CLK_DIV 80        // 1 тик = 1 мкс
#define MAX_PULSES 1000

rmt_item32_t pulseItems[MAX_PULSES];
bool rmtInitialized = false;
const int RX_PLC_PIN = 27;
const int TX_PLC_PIN = 18; // 33

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

// ========== ИНИЦИАЛИЗАЦИЯ RMT  ==========
void setupRMT() {
  rmt_config_t config = RMT_DEFAULT_CONFIG_TX((gpio_num_t)TX_PLC_PIN, RMT_CHANNEL);
  config.clk_div = RMT_CLK_DIV;
  config.tx_config.loop_en = false;
  config.tx_config.idle_output_en = true;
  config.tx_config.idle_level = RMT_IDLE_LEVEL_LOW;
  
  rmt_config(&config);
  rmt_driver_install(RMT_CHANNEL, 0, 0);
  
  // Заполняем базовыми значениями (длительности 50/100)
  for (int i = 0; i < MAX_PULSES; i++) {
    pulseItems[i].level0 = 1;
    pulseItems[i].duration0 = 50;   // ОРИГИНАЛ
    pulseItems[i].level1 = 0;
    pulseItems[i].duration1 = 100;  // ОРИГИНАЛ
  }
  rmtInitialized = true;
}

// ========== ОТПРАВКА ИМПУЛЬСОВ ЧЕРЕЗ RMT ==========
void sendPulseBlock(uint32_t pulseCount) {
  if (!rmtInitialized || pulseCount == 0) return;
  if (pulseCount > MAX_PULSES) pulseCount = MAX_PULSES;
  rmt_write_items(RMT_CHANNEL, pulseItems, pulseCount, true); // true = ждать завершения
}

void sendPlcStringSimple(String val) {
  if (val.length() != 4) return;  // Теперь 4 цифры
  
  for (int i = 0; i < 4; i++) {
    int digit = val.charAt(i) - '0';
    if (digit < 0 || digit > 9) continue;
    
    long pulseCount = (digit * 40) + 20;  // Новая формула
    sendPulseBlock((uint32_t)pulseCount); // Отправка через RMT
    delay(35);  // Пауза между цифрами (оставляем)
  }
}

// === НОВОЕ: положить команду в очередь ===
void enqueuePlcCommand(String cmd) {
  int num = cmd.toInt();
  char buf[5];
  snprintf(buf, sizeof(buf), "%04d", num);
  String formatted = String(buf);
  
  if (queueCount >= 10) {
    return;
  }
  int tail = (queueHead + queueCount) % 10;
  plcQueue[tail] = formatted;
  queueCount++;
}

// === НОВОЕ: обработать очередь (вызывать из loop) ===
void processPlcQueue() {
  if (isSending || queueCount == 0) return;
  
  String cmd = plcQueue[queueHead];
  queueHead = (queueHead + 1) % 10;
  queueCount--;
  
  isSending = true;
 
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
      enqueuePlcCommand(trackStr);   // ← БЫЛО: sendPlcStringSimple
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
      String plcMessage = (prefix == "BS") ? "9" : "8";  // ← Теперь одна цифра
if (pin < 10) {
  plcMessage += "0" + String(pin);   // 9 + 0 + пин = 3 цифры
} else {
  plcMessage += String(pin);          // 9 + пин = 3 цифры (если пин 10-99)
}
plcMessage += String(state);          // + состояние = 4 цифры
      
      enqueuePlcCommand(plcMessage);             //  sendPlcStringSimple
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
  setupRMT(); 

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
