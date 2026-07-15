#include <WiFi.h>
#include <WebServer.h>
#include <esp_now.h>
#include "esp_bt.h"
#include "esp_wifi.h"

// Настройки НАШЕЙ собственной точки доступа
const char* ap_ssid = "ESP32-C3-Node";
const char* ap_password = "password123";

// --- НАСТРОЙКА ПИНОВ ---
const int RX_PLC_PIN = 0; // Приём жирных импульсов из ПЛК (GPIO 0)
const int TX_PLC_PIN = 3; // Отправка 8-значных пачек в ПЛК (GPIO 2)

// Широковещательный адрес для отправки в Тележку
uint8_t broadcastMac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Переменные для замера длительности одиночного импульса от ПЛК
volatile unsigned long pulseStartTime = 0;
volatile unsigned long pulseDuration = 0;
volatile bool pulseReadyToProcess = false;

// Переменные для веб-интерфейса
String lastLogMessage = "Ожидание действий...";
String lastSentPacket = "—";
String lastPlcOut = "—";

WebServer server(80);

// --- ПРОСТАЯ И НАДЕЖНАЯ ОТПРАВКА В ПЛК ---
void sendPlcStringSimple(String val) {
  if (val.length() != 8) return;
  lastPlcOut = val;

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

// --- ПРЕРЫВАНИЕ: Измеряем длину импульса по CHANGE ---
void IRAM_ATTR iasPulseHandler() {
  unsigned long currentTime = millis();
  
  if (digitalRead(RX_PLC_PIN) == LOW) {
    pulseStartTime = currentTime; 
  } 
  else {
    if (pulseStartTime > 0) {
      pulseDuration = currentTime - pulseStartTime; 
      pulseReadyToProcess = true;
      pulseStartTime = 0; 
    }
  }
}

// --- КОЛБЕК СОБЫТИЙНОГО ПРИЕМА ПО РАДИО (От Тележки) ---
void onDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len) {
  if (len >= 3) {
    char buf[16];
    int copy_len = (len > 15) ? 15 : len;
    memcpy(buf, incomingData, copy_len);
    buf[copy_len] = '\0';
    
    String msg = String(buf);
    lastLogMessage = "По воздуху прилетел пакет: " + msg;
    
    if (msg.startsWith("A")) {
      String trackStr = msg.substring(1); 
      if (trackStr.toInt() > 0) {
        sendPlcStringSimple(trackStr + "000000"); 
      }
    }
  }
}

// --- ИНТЕРФЕЙС ВЕБ-СТРАНИЦЫ ---
const char HTML_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Пульт Ноды ESP32-C3</title>
<style>
  body{font-family:Arial,sans-serif;text-align:center;margin-top:20px;background:#f0f2f5;color:#333;}
  .card{background:white;padding:20px;border-radius:12px;display:inline-block;box-shadow:0 4px 15px rgba(0,0,0,0.1);width:320px;margin:8px;}
  .btn{color:white;border:none;padding:14px;font-size:15px;border-radius:8px;cursor:pointer;width:100%;font-weight:bold;margin:6px 0;background:#1a73e8;transition:0.1s;}
  .btn:hover{background:#1557b0;} .btn-red{background:#dc3545;} .btn-red:hover{background:#bd2130;}
  h2{margin-bottom:15px;color:#4a5568;font-size:18px;border-bottom:2px solid #f0f2f5;padding-bottom:8px;}
  p{text-align:left;line-height:24px;font-size:16px;margin:8px 0;}
  .highlight{font-weight:bold;color:#1a73e8;}
</style></head>
<body>

  <div class="card">
    <h2>Состояние Ноды (Своя AP)</h2>
    <p>Лог: <span id="log" class="highlight">Загрузка...</span></p>
    <p>Улетело в эфир: <span id="tx_air" class="highlight">—</span></p>
    <p>Выдано в ПЛК (пин 2): <span id="tx_plc" class="highlight">—</span></p>
  </div><br>

  <div class="card">
    <h2>Эмулятор автоматики</h2>
    <button class="btn" onclick="sendCmd('emu_plc?track=13')">Имитировать ПЛК: Дорожка 13 (1 сек)</button>
    <button class="btn" onclick="sendCmd('emu_plc?track=15')">Имитировать ПЛК: Дорожка 15 (2 сек)</button>
    <button class="btn btn-red" onclick="sendCmd('emu_radio?track=13')">Имитировать радио-ответ: А13</button>
    <button class="btn btn-red" onclick="sendCmd('emu_radio?track=15')">Имитировать радио-ответ: А15</button>
  </div>

<script>
async function sendCmd(url) {
  try { await fetch('/' + url); upd(); } catch(e){}
}
async function upd(){
  try {
    let r = await fetch('/data');
    let d = await r.json();
    document.getElementById('log').textContent = d.log;
    document.getElementById('tx_air').textContent = d.tx_air;
    document.getElementById('tx_plc').textContent = d.tx_plc;
  } catch(e){}
}
setInterval(upd, 500);
upd();
</script></body></html>
)rawliteral";

void setup() {
  esp_bt_controller_disable(); // Намертво глушим Bluetooth

  // Конфигурация пинов
  pinMode(RX_PLC_PIN, INPUT); 
  attachInterrupt(digitalPinToInterrupt(RX_PLC_PIN), iasPulseHandler, CHANGE);
  
  pinMode(TX_PLC_PIN, OUTPUT);
  digitalWrite(TX_PLC_PIN, LOW);

  // Поднимаем НАШУ точку доступа строго на 1 канале (под частоту Тележки)
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(ap_ssid, ap_password, 1, 0, 4); // 3-й параметр "1" — это фиксация 1-го канала
  WiFi.setTxPower(WIFI_POWER_8_5dBm); 

  // Дополнительно страхуем фиксацию радио-канала для ESP-NOW
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

  // Старт ESP-NOW
  if (esp_now_init() != ESP_OK) {
    return;
  }

  esp_now_register_recv_cb(esp_now_recv_cb_t(onDataRecv));

  // Регистрация Broadcast
  esp_now_peer_info_t peerInfo;
  memset(&peerInfo, 0, sizeof(peerInfo));
  memcpy(peerInfo.peer_addr, broadcastMac, 6);
  peerInfo.channel = 1; 
  peerInfo.encrypt = false;
  esp_now_add_peer(&peerInfo);

  // --- МАРШРУТЫ СЕРВЕРА ---
  server.on("/", [](){ server.send(200, "text/html", HTML_PAGE); });
  
  server.on("/data", [](){
    String json = "{";
    json += "\"log\":\"" + lastLogMessage + "\",";
    json += "\"tx_air\":\"" + lastSentPacket + "\",";
    json += "\"tx_plc\":\"" + lastPlcOut + "\"";
    json += "}";
    server.send(200, "application/json", json);
  });

  // Эмуляция импульса ПЛК через кнопку на сайте
  server.on("/emu_plc", [](){
    if (server.hasArg("track")) {
      String track = server.arg("track");
      lastSentPacket = "A" + track;
      lastLogMessage = "[Эмуляция] Кнопка нажата. Выстрел радио-маркера A" + track;
      
      int msgLen = lastSentPacket.length();
      uint8_t dataBuf[16];
      lastSentPacket.getBytes(dataBuf, msgLen + 1);
      esp_now_send(broadcastMac, dataBuf, msgLen);
    }
    server.send(200, "text/plain", "OK");
  });

  // Эмуляция прилета радиопакета от тележки через кнопку на сайте
  server.on("/emu_radio", [](){
    if (server.hasArg("track")) {
      String track = server.arg("track");
      String fakeRadio = "A" + track;
      int msgLen = fakeRadio.length();
      uint8_t fakeBuf[16];
      fakeRadio.getBytes(fakeBuf, msgLen + 1);
      
      // Имитируем прилет, вызывая колбек вручную
      onDataRecv(broadcastMac, fakeBuf, msgLen);
    }
    server.send(200, "text/plain", "OK");
  });

  server.begin();
}

void loop() {
  server.handleClient();

  // --- ОБРАБОТКА ФИЗИЧЕСКОГО ИМПУЛЬСА ИЗ ПЛК ---
  if (pulseReadyToProcess) {
    lastLogMessage = "Физический импульс на пине 0: " + String(pulseDuration) + " мс";

    int detectedTrack = 0;
    if (pulseDuration >= 700 && pulseDuration <= 1300) {
      detectedTrack = 13; 
    }
    else if (pulseDuration >= 1700 && pulseDuration <= 2300) {
      detectedTrack = 15; 
    }

    if (detectedTrack > 0) {
      lastSentPacket = "A" + String(detectedTrack);
      lastLogMessage = "Импульс принят! Выстрел в Тележку: " + lastSentPacket;
      
      int msgLen = lastSentPacket.length();
      uint8_t dataBuf[16];
      lastSentPacket.getBytes(dataBuf, msgLen + 1);
      esp_now_send(broadcastMac, dataBuf, msgLen);
    } else {
      lastLogMessage = "Физический импульс (" + String(pulseDuration) + " мс) отсечен как помеха";
    }

    pulseReadyToProcess = false; 
  }
}
