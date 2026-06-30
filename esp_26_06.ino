#include <WiFi.h>
#include <WebServer.h>
#include <esp_wifi.h>  
#include <esp_now.h>
#include <PlcProto.h> // Подключаем вашу библиотеку

// Связь со второй вкладкой, где лежит HTML-страница
#include "web.h"

// ========== НАСТРОЙКИ ЖЕЛЕЗА ==========
#define PIN_TO_PLC   33 // Сюда шлем 8 цифр аппаратно через RMT из библиотеки
#define PIN_FROM_PLC 27 // Отсюда ловим данные по прерываниям из библиотеки
#define PIN_PLC_BUSY 14 // Пин статуса занятости/ручного режима ПЛК
#define BUSY_STATE   LOW // Уровень, при котором конвейер считается занятым (LOW = замкнут на GND)

// Настройки WiFi
const char* ssid = "Telega-Control";
const char* password = "02345678";
WebServer server(80);

// Инициализируем объекты библиотеки PlcProto
PlcSender   plcTx(PIN_TO_PLC);
PlcReceiver plcRx(PIN_FROM_PLC);

// Статусы системы
bool isPlcBusy = false;       // Флаг аппаратной занятости (пин 26)
int plcSystemStatus = 0;      // Код текущего состояния от ПЛК (из библиотеки)

// ========== СТРУКТУРЫ РАДИО И ОЧЕРЕДИ ==========
typedef struct struct_message {
    int nodeTrack;  // Номер дорожки (8, 12, 15, 2, 9)
    int whistle;    // Статус от ноды (1 - загружена, 2 - вызов, 3 - пустая)
} struct_message;
struct_message incomingData;

typedef struct {
    int fromTrack;
    int toTrack;
} Task;

#define MAX_QUEUE 20
Task taskQueue[MAX_QUEUE];
int queueSize = 0; 

// Настройки маршрутов (5 строк для автоматики)
String trackRoutes[] = {"16", "14", "38", "02", "09"};
const int physicalTracks[] = {13, 15, 25, 2, 9};
int trackStatuses[] = {0, 0, 0, 0, 0};

bool isTaskExecuting = false;

// Для отображения ручного рейса на экране
bool isManualTripActive = false;
int manualFrom = 0;
int manualTo = 0;

// ========== ОТПРАВКА ДАННЫХ ЧЕРЕЗ БИБЛИОТЕКУ ==========
void sendSequenceToPLC(int from, int to, int digit5, int digit6, int digit7, int digit8) {
  char val[16]; // Увеличили буфер с запасом, чтобы точно не было переполнения
  snprintf(val, sizeof(val), "%02d%02d%d%d%d%d", from, to, digit5, digit6, digit7, digit8);
  
  // Передаем готовую 8-значную строку в метод вашей библиотеки
  plcTx.sendString(String(val)); 
}


// Сдвиг очереди задач вперед
void shiftQueue() {
  if (queueSize <= 0) return;
  for (int i = 0; i < queueSize - 1; i++) {
    taskQueue[i] = taskQueue[i + 1];
  }
  queueSize--;
  isTaskExecuting = false;
}

// ========== ИЗОЛИРОВАННАЯ ФУНКЦИЯ ОБРАБОТКИ СТАТУСОВ ПЛК ==========
void processPlcData() {
  // 1. Всегда опрашиваем аппаратный пин 26
  isPlcBusy = (digitalRead(PIN_PLC_BUSY) == BUSY_STATE);

  // 2. Проверяем, прилетел ли пакет из библиотеки от ПЛК
  if (plcRx.available()) {
    int plcMsg = plcRx.getInteger();   // Двузначное число
    int plcSub = plcRx.getFraction();  // Одиночная цифра

    // Фильтруем коды ПЛК (обрабатываем только если цифра равна 2)
    if (plcSub == 2) {
      switch (plcMsg) {
        case 1: // Задача выполнена (Рейс завершен, тележка пустая)
          plcSystemStatus = 1; 
          break;
        case 4: // Тележка встала на парковку
          plcSystemStatus = 4; 
          break;
        case 5: // Загрузка на борт готова
          plcSystemStatus = 5; 
          break;
        case 6: // Выгрузка готова
          plcSystemStatus = 6; 
          break;
        default:
          break;
      }
    }
  }
}

// ========== ОБРАБОТЧИК РАДИО (ESP-NOW ОТ НОД) ==========
void OnDataRecv(const uint8_t * mac, const uint8_t *incomingBytes, int len) {
  if (len != sizeof(incomingData)) return;
  memcpy(&incomingData, incomingBytes, sizeof(incomingData));
  
  int track = incomingData.nodeTrack; 
  int status = incomingData.whistle;  
  
  // Ищем индекс дорожки в физическом массиве
  int idx = -1;
  for (int i = 0; i < 5; i++) {
    if (physicalTracks[i] == track) { idx = i; break; }
  }
  if (idx == -1) return; // Не наша дорожка — игнорируем
  
  if (status == 1) {
    // 1 - Загружена (Сквозной статус для сайта)
    if (trackStatuses[idx] == 0) trackStatuses[idx] = 1; 
  } 
  else if (status == 2) {
    // 2 - Вызов / Флаг выезда (Ставим задачу в очередь)
    if (trackStatuses[idx] == 1) {
      trackStatuses[idx] = 3; // Временный статус "В очереди" для отображения на сайте
      
      // Защита от дублирования задачи в очереди
      bool alreadyInQueue = false;
      for (int i = 0; i < queueSize; i++) {
        if (taskQueue[i].fromTrack == track) { alreadyInQueue = true; break; }
      }
      
      // Если задачи нет в очереди и буфер не переполнен — добавляем рейс
      if (!alreadyInQueue && queueSize < MAX_QUEUE) {
        taskQueue[queueSize].fromTrack = track;
        taskQueue[queueSize].toTrack = trackRoutes[idx].toInt(); // Маршрут берем из таблицы автоматики
        queueSize++;
      }
    }
  }
  else if (status == 3) {
    // 3 - Пустая / Тележка забрала груз (Сквозной статус для сайта)
    trackStatuses[idx] = 0; 
  }
}

// ========== ЭНДПОИНТЫ ВЕБ-СЕРВЕРА ==========
void handleRoot() {
  server.send(200, "text/html", html_page);
}

void handleSetRoutes() {
  if (server.hasArg("val")) {
    String payload = server.arg("val");
    int startPos = 0;
    for (int i = 0; i < 5; i++) {
      int pipePos = payload.indexOf('|', startPos);
      if (pipePos != -1) {
        trackRoutes[i] = payload.substring(startPos, pipePos);
        startPos = pipePos + 1;
      } else {
        trackRoutes[i] = payload.substring(startPos);
        break;
      }
    }
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "BAD");
  }
}

void handleRunManualTrip() {
  if (isTaskExecuting || isManualTripActive) {
    server.send(403, "text/plain", "BUSY");
    return;
  }
  
  if (server.hasArg("from") && server.hasArg("to") && server.hasArg("rollers")) {
    manualFrom = server.arg("from").toInt();
    manualTo = server.arg("to").toInt();
    int rollers = server.arg("rollers").toInt();
    
    int d5 = 2; 
    int d6 = 0; 
    if (rollers == 2) { d5 = 2; d6 = 2; }
    
    sendSequenceToPLC(manualFrom, manualTo, d5, d6, 0, 0);
    
    isManualTripActive = true;
    isTaskExecuting = true;
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "BAD");
  }
}

void handleGetStatusData() {
  String json = "{";
  json += "\"statuses\":[";
  for (int i = 0; i < 5; i++) {
    json += String(trackStatuses[i]);
    if (i < 4) json += ",";
  }
  json += "],";
  
  json += "\"is_paused\":0,";
  json += "\"is_busy\":" + String(isPlcBusy ? 1 : 0) + ","; // Статус занятости (пин 26) транслируем на сайт
  json += "\"manual_active\":" + String(isManualTripActive ? 1 : 0) + ",";
  json += "\"manual_from\":" + String(manualFrom) + ",";
  json += "\"manual_to\":" + String(manualTo) + ",";
  
  json += "\"queue\":[";
  for (int i = 0; i < queueSize; i++) {
    json += "{\"from\":\"" + String(taskQueue[i].fromTrack) + "\",\"to\":\"" + String(taskQueue[i].toTrack) + "\"}";
    if (i < queueSize - 1) json += ",";
  }
  json += "]";
  json += "}";
  
  server.send(200, "application/json", json);
}

void handleCancelTask() {
  if (isTaskExecuting) {
    sendSequenceToPLC(0, 0, 0, 0, 1, 0); // В линию улетит: 00000010
    if (isManualTripActive) {
      isManualTripActive = false;
      isTaskExecuting = false;
    } else if (queueSize > 0) {
      for (int i = 0; i < 5; i++) {
        if (physicalTracks[i] == taskQueue[0].fromTrack) { trackStatuses[i] = 0; break; }
      }
      shiftQueue();
    }
    server.send(200, "text/plain", "OK");
  } else {
    server.send(200, "text/plain", "NO_ACTIVE_TASK");
  }
}

void handleGlobalStop() {
  sendSequenceToPLC(0, 0, 0, 0, 2, 0); // В линию улетит: 00000020
  server.send(200, "text/plain", "OK");
}

void handleGlobalClear() {
  sendSequenceToPLC(0, 0, 0, 0, 3, 0); // В линию улетит: 00000030
  for (int i = 0; i < 5; i++) trackStatuses[i] = 0; 
  queueSize = 0; 
  isTaskExecuting = false;
  isManualTripActive = false;
  server.send(200, "text/plain", "OK");
}

// ========== SETUP & LOOP ==========
void setup() {
  // Инициализируем оба модуля вашей библиотеки PlcProto
  plcTx.begin(); 
  plcRx.begin(); 

  // Конфигурируем пин занятости ПЛК
  pinMode(PIN_PLC_BUSY, INPUT_PULLUP);

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(ssid, password);
  WiFi.setSleep(false);
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() == ESP_OK) {
    esp_now_register_recv_cb(esp_now_recv_cb_t(OnDataRecv));
  }

  server.on("/", handleRoot);
  server.on("/set_routes", handleSetRoutes);
  server.on("/run_manual_trip", handleRunManualTrip);
  server.on("/get_status_data", handleGetStatusData);
  server.on("/cancel_task", handleCancelTask);
  server.on("/global_stop", handleGlobalStop);
  server.on("/global_clear", handleGlobalClear);
  server.begin();
}

void loop() {
  server.handleClient();
  
  // Обновляем движок приема библиотеки PlcProto на каждом круге loop
  plcRx.update();

  // Вызываем единую функцию опроса пина 26 и разбора пакетов ПЛК
  processPlcData();

  bool taskFinishedSuccess = false;

  // --- ОБРАБОТКА УСПЕШНОГО ФИНИША ОТ ПЛК ---
  // Если ПЛК прислал код 1-2 (Задача выполнена успешно)
  if (isTaskExecuting && plcSystemStatus == 1) {
    taskFinishedSuccess = true;
    plcSystemStatus = 0; // Сбрасываем статус ответа для следующих рейсов
  }

  // Если рейс успешно завершен
  if (isTaskExecuting && taskFinishedSuccess) {
    if (isManualTripActive) {
      isManualTripActive = false;
      isTaskExecuting = false; // Для ручного пуска освобождаем тележку сразу
    } else if (queueSize > 0) {
      // Для автоматики: рейс завершен, сдвигаем очередь задач вперед
      shiftQueue(); 
    }
  }

  // --- АВТОМАТИЧЕСКАЯ ОБРАБОТКА ОЧЕРЕДИ ЗАДАЧ (СТАРТ ПЕРВОЙ ЗАДАЧИ) ---
  if (!isManualTripActive && queueSize > 0 && !isTaskExecuting && !isPlcBusy && digitalRead(PIN_FROM_PLC) != LOW) {
    
    // Находим индекс выполняемой дорожки, чтобы сайт подсветил её зеленым («В ПУТИ»)
    for (int i = 0; i < 5; i++) {
      if (physicalTracks[i] == taskQueue[0].fromTrack) { 
        trackStatuses[i] = 2; // Переключаем статус в 2 (Выполняется)
        break; 
      }
    }
    
    // Шлем в ПЛК данные ПЕРВОЙ задачи из нашей очереди (Коды автоматики: 1 и 4)
sendSequenceToPLC(taskQueue[0].fromTrack, taskQueue[0].toTrack, 1, 4, 0, 0);
isTaskExecuting = true;
}
// Проверка активности точки доступа Wi-Fi
static unsigned long lastApCheck = 0;
if (millis() - lastApCheck > 15000) {lastApCheck = millis();
if (WiFi.softAPgetStationNum() == 0 && WiFi.softAPIP().toString() == "0.0.0.0")
 {WiFi.softAP(ssid, password);
}
}
}