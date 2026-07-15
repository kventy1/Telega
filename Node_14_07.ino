#include <WiFi.h>
#include <esp_now.h>
#include "esp_bt.h"
#include "esp_wifi.h"

// --- НАСТРОЙКА ПИНОВ ---
const int RX_PLC_PIN = 0; // Приём жирных импульсов из ПЛК (GPIO 0)
const int TX_PLC_PIN = 2; // Отправка 8-значных пачек в ПЛК (GPIO 2)

// Широковещательный адрес для отправки в Тележку
uint8_t broadcastMac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Переменные для замера длительности одиночного импульса от ПЛК
volatile unsigned long pulseStartTime = 0;
volatile unsigned long pulseDuration = 0;
volatile bool pulseReadyToProcess = false;

// --- ПРОСТАЯ И НАДЕЖНАЯ ОТПРАВКА В ПЛК (50мкс HIGH, 100мкс LOW, пауза 35мс) ---
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

// --- ПРЕРЫВАНИЕ: Измеряем длину импульса по обоим фронтам (CHANGE) ---
void IRAM_ATTR iasPulseHandler() {
  unsigned long currentTime = millis();
  
  // Оптопара инвертирует сигнал: ПЛК включил выход -> LOW, выключил -> HIGH
  if (digitalRead(RX_PLC_PIN) == LOW) {
    pulseStartTime = currentTime; // Запомнили время начала
  } 
  else {
    if (pulseStartTime > 0) {
      pulseDuration = currentTime - pulseStartTime; // Посчитали чистую длительность в мс
      pulseReadyToProcess = true;
      pulseStartTime = 0; // Сброс для следующего захода
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
    
    // Проверяем наш маркер ноды: пакет должен строго начинаться на 'A'
    if (msg.startsWith("A")) {
      // Выдергиваем всё, что идет после буквы 'A' (номер дорожки, например "13" или "15")
      String trackStr = msg.substring(1); 
      
      // Склеиваем её с шестью нулями и «выстреливаем» в ПЛК: "13000000" или "15000000"
      if (trackStr.toInt() > 0) {
        sendPlcStringSimple(trackStr + "000000"); 
      }
    }
  }
}

void setup() {
  esp_bt_controller_disable(); // Намертво глушим Bluetooth

  // Конфигурация пинов
  pinMode(RX_PLC_PIN, INPUT); 
  attachInterrupt(digitalPinToInterrupt(RX_PLC_PIN), iasPulseHandler, CHANGE);
  
  pinMode(TX_PLC_PIN, OUTPUT);
  digitalWrite(TX_PLC_PIN, LOW);

  // Чистый режим STA для работы ESP-NOW
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(); 

  // Хак принудительного переключения на 1-й канал (под частоту Тележки)
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

  // Старт ESP-NOW
  if (esp_now_init() != ESP_OK) {
    return;
  }

  // Регистрируем функцию событийного приёма по радио
  esp_now_register_recv_cb(esp_now_recv_cb_t(onDataRecv));

  // Регистрируем Broadcast для отправки на Тележку
  esp_now_peer_info_t peerInfo;
  memset(&peerInfo, 0, sizeof(peerInfo));
  memcpy(peerInfo.peer_addr, broadcastMac, 6);
  peerInfo.channel = 1; 
  peerInfo.encrypt = false;
  esp_now_add_peer(&peerInfo);

  WiFi.setTxPower(WIFI_POWER_8_5dBm); // Хак мощности под антенну-проводок
}

void loop() {
  // --- ОБРАБОТКА ИМПУЛЬСА ИЗ ПЛК ---
  if (pulseReadyToProcess) {
    int detectedTrack = 0;

    // Ворота короткого импульса: 1 сек (от 700 до 1300 мс)
    if (pulseDuration >= 700 && pulseDuration <= 1300) {
      detectedTrack = 13; 
    }
    // Ворота длинного импульса: 2 сек (от 1700 до 2300 мс)
    else if (pulseDuration >= 1700 && pulseDuration <= 2300) {
      detectedTrack = 15; 
    }

    // Если ПЛК выдал валидный импульс, пуляем маркер в Тележку
    if (detectedTrack > 0) {
      String payload = "A" + String(detectedTrack); // Получим "A13" или "A15"
      
      int msgLen = payload.length();
      uint8_t dataBuf[16];
      payload.getBytes(dataBuf, msgLen + 1);

      esp_now_send(broadcastMac, dataBuf, msgLen);
    }

    pulseReadyToProcess = false; // Ждем следующий жирный импульс
  }
}
