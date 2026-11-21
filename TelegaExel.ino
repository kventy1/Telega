#include <WiFi.h>
#include <WebServer.h>
#include <esp_wifi.h>
#include <ModbusMaster.h>
#include <HardwareSerial.h>
#include <esp_now.h>
#include <Preferences.h>
extern const char simple_page[] PROGMEM;
extern const char html_page[]   PROGMEM;

void sendCommandToNode(int trackNum);
void handleIncomingTrack(int track, int whistle);

// ========== НАСТРОЙКИ MODBUS RTU ==========
#define MODBUS_RX_PIN 17
#define MODBUS_TX_PIN 16
#define MODBUS_SLAVE_ID 17
#define MODBUS_BAUD 57600

HardwareSerial modbusSerial(2);
ModbusMaster modbus;

static const uint16_t REG_LINE_START = 16460;
static const uint16_t REG_LINE_STOP = 16461;
static const uint16_t REG_ROLLER1 = 16462;
static const uint16_t REG_ROLLER2 = 16463;
static const uint16_t REG_TAKE_TASK = 16464;
static const uint16_t REG_RESET_WAY = 16465;
static const uint16_t REG_FORCE_AUTO = 16466;
static const uint16_t REG_PAUSE = 16467;
static const uint16_t REG_RESET_WIFI = 16468;
static const uint16_t REG_READ_START = 16477;
static const uint16_t REG_READ_COUNT = 23;
static const uint16_t REG_PARKING_READY = 16494;

const char* modbusRegisterNames[REG_READ_COUNT] = {
  "Датчик выгрузки первого ролика", "Датчик загрузки первого ролика",
  "Датчик загрузки второго ролика", "Датчик выгрузки второго ролика",
  "Двигатель вперёд", "Двигатель назад", "Барьер передний", "Барьер задний",
  "Ролик 1 полный", "Ролик 2 полный", "Таймер роликов", "Вайфа готов",
  "Режим Бул", "ВФ авто занят", "ExtraStop", "Аларм", "Выгрузка готово",
  "Парковка готов", "Загрузка готов",
   "Регистр 16496", "Регистр 16497", "Регистр 16498", "Регистр 16499"
};
uint16_t modbusValues[REG_READ_COUNT] = {0};
bool modbusDataValid = false;
uint32_t lastModbusPoll = 0;
uint8_t lastModbusError = 0;

// ===== RS-регистры тележки (16496..16499) → в эфир NRS<val> =====
#define RS_SRC_COUNT 4
static const uint16_t RS_SRC_REGS[RS_SRC_COUNT] = {16496, 16497, 16498, 16499};
uint16_t rsLastVal[RS_SRC_COUNT] = {0, 0, 0, 0};
bool     rsFirstPoll = true;

// Состояния импульсных команд и последовательности старта задания.
bool resetWayPending = false;
bool resetWifiPending = false;
bool resetWayHigh = false;
bool resetWifiHigh = false;
uint32_t resetWayStartedAt = 0;
uint32_t resetWifiStartedAt = 0;
uint8_t taskWriteStep = 0;
bool taskWriteInProgress = false;
uint16_t pendingTaskFrom = 0;
uint16_t pendingTaskTo = 0;
uint16_t pendingRoller1 = 0;
uint16_t pendingRoller2 = 0;
bool taskTakePulseHigh = false;
uint32_t taskTakePulseStartedAt = 0;
bool taskWriteFailed = false;
bool waitForCompletionPulse = false;
bool completionPulseSeen = false;
bool previousWifiReady = false;
bool forceAutoState = false;
bool previousParkingReady = false;

// Настройки WiFi
const char* ssid = "Telega-Control";
const char* password = "02345678";

WebServer server(80);

char rx_node_buf[16];
uint8_t broadcastMac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ========== СТРУКТУРЫ РАДИО И ОЧЕРЕДИ ==========
typedef struct struct_message {
    int nodeTrack;
    int whistle;
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
String trackRoutes[] = {"14", "16", "38", "40", "09"};
const int physicalTracks[] = {13, 15, 25, 41, 69};
int trackStatuses[] = {0, 0, 0, 0, 0};

Preferences prefs;

bool isTaskExecuting = false;

bool isManualTripActive = false;
int manualFrom = 0;
int manualTo = 0;

bool modbusBit(uint16_t address);
void shiftQueue();

// ========== Modbus helpers ==========
bool writeModbusRegister(uint16_t reg, uint16_t value) {
  modbus.setTransmitBuffer(0, value);
  uint8_t result = modbus.writeMultipleRegisters(reg, 1);
  lastModbusError = result;
  if (result != modbus.ku8MBSuccess) { modbusDataValid = false; return false; }
  return true;
}

bool readModbusRegisters() {
  uint8_t result = modbus.readHoldingRegisters(REG_READ_START, REG_READ_COUNT);
  lastModbusError = result;
  if (result != modbus.ku8MBSuccess) { modbusDataValid = false; return false; }
  for (uint8_t i = 0; i < REG_READ_COUNT; ++i) modbusValues[i] = modbus.getResponseBuffer(i);
  modbusDataValid = true;
  return true;
}

bool modbusBit(uint16_t address) {
  if (!modbusDataValid || address < REG_READ_START || address >= REG_READ_START + REG_READ_COUNT) return false;
  return modbusValues[address - REG_READ_START] != 0;
}

bool startInterlocksClear() {
  return modbusDataValid && modbusBit(16489) &&
         !modbusBit(16477) && !modbusBit(16478) &&
         !modbusBit(16479) && !modbusBit(16480);
}

void beginTaskWrites(int from, int to, int rollers) {
  if (taskWriteInProgress || taskTakePulseHigh || !startInterlocksClear()) return;
  pendingTaskFrom = from;
  pendingTaskTo = to;
  if (rollers == 0) {
    pendingRoller1 = 0;
    pendingRoller2 = 0;
  } else {
    pendingRoller1 = 2;
    pendingRoller2 = (rollers == 2) ? 2 : 0;
  }
  taskWriteStep = 0;
  taskWriteFailed = false;
  taskWriteInProgress = true;
}

void processModbusActions() {
  const uint32_t now = millis();

  // ===== 1. Импульс TAKE_TASK: без return, с аварийным сбросом =====
  if (taskTakePulseHigh && now - taskTakePulseStartedAt >= 2000) {
    if (writeModbusRegister(REG_TAKE_TASK, 0)) {
      taskTakePulseHigh = false;
    } else if (now - taskTakePulseStartedAt >= 10000) {
      taskTakePulseHigh = false;   // аварийный сброс
      lastModbusError = 0xE1;
    }
  }

  // ===== 2. Сбросы: максимум одна запись за цикл, без return =====
  bool wroteThisCycle = false;

  if (!wroteThisCycle && resetWayPending && !resetWayHigh) {
    if (writeModbusRegister(REG_RESET_WAY, 1)) {
      resetWayHigh = true;
      resetWayStartedAt = now;
      resetWayPending = false;
    }
    wroteThisCycle = true;
  }
  else if (!wroteThisCycle && resetWayHigh && now - resetWayStartedAt >= 1000) {
    if (writeModbusRegister(REG_RESET_WAY, 0)) {
      resetWayHigh = false;
    } else if (now - resetWayStartedAt >= 10000) {
      resetWayHigh = false;        // аварийный сброс
      lastModbusError = 0xE2;
    }
    wroteThisCycle = true;
  }
  else if (!wroteThisCycle && resetWifiPending && !resetWifiHigh) {
    if (writeModbusRegister(REG_RESET_WIFI, 1)) {
      resetWifiHigh = true;
      resetWifiStartedAt = now;
      resetWifiPending = false;
    }
    wroteThisCycle = true;
  }
  else if (!wroteThisCycle && resetWifiHigh && now - resetWifiStartedAt >= 1000) {
    if (writeModbusRegister(REG_RESET_WIFI, 0)) {
      resetWifiHigh = false;
    } else if (now - resetWifiStartedAt >= 10000) {
      resetWifiHigh = false;       // аварийный сброс
      lastModbusError = 0xE3;
    }
    wroteThisCycle = true;
  }

  // ===== 3. Запись задачи: только если в этом цикле ещё не писали =====
  if (!wroteThisCycle && taskWriteInProgress) {
    uint16_t reg = 0, value = 0;
    switch (taskWriteStep) {
      case 0: reg = REG_LINE_START; value = pendingTaskFrom; break;
      case 1: reg = REG_LINE_STOP;  value = pendingTaskTo;   break;
      case 2: reg = REG_ROLLER1;    value = pendingRoller1;  break;
      case 3: reg = REG_ROLLER2;    value = pendingRoller2;  break;
    }
    if (!writeModbusRegister(reg, value)) {
      taskWriteFailed = true; taskWriteInProgress = false;
      if (isManualTripActive) isManualTripActive = false;
      if (queueSize > 0 && !isManualTripActive) {
        for (int i = 0; i < 5; ++i) if (physicalTracks[i] == taskQueue[0].fromTrack) { trackStatuses[i] = 1; break; }
      }
      isTaskExecuting = false;
      return;
    }
    taskWriteStep++;
    if (taskWriteStep >= 4) {
      taskWriteInProgress = false;
      if (writeModbusRegister(REG_TAKE_TASK, 1)) {
        taskTakePulseHigh = true; taskTakePulseStartedAt = now; isTaskExecuting = true;
      } else {
        taskWriteFailed = true;
        if (isManualTripActive) isManualTripActive = false;
        if (queueSize > 0 && !isManualTripActive) {
          for (int i = 0; i < 5; ++i) if (physicalTracks[i] == taskQueue[0].fromTrack) { trackStatuses[i] = 1; break; }
        }
        isTaskExecuting = false;
      }
    }
    return;
  }

  // ===== 4. Опрос Modbus — выполняется ВСЕГДА, даже если писали =====
  if (now - lastModbusPoll >= 250) {
    lastModbusPoll = now;
    if (readModbusRegisters()) {
      bool wifiReady = modbusBit(16488);
      bool parkingReady = modbusBit(REG_PARKING_READY);
      bool wifiRising = wifiReady && !previousWifiReady;
      bool parkingRising = parkingReady && !previousParkingReady;
      previousWifiReady = wifiReady;
      previousParkingReady = parkingReady;

      // ===== RS: отправка в эфир при изменении 16496..16499 =====
      for (int i = 0; i < RS_SRC_COUNT; i++) {
        uint16_t idx = RS_SRC_REGS[i] - REG_READ_START;
        if (idx >= REG_READ_COUNT) continue;
        uint16_t v = modbusValues[idx];
        if (rsFirstPoll) {
          rsLastVal[i] = v;
        } else if (v != rsLastVal[i]) {
          rsLastVal[i] = v;
          sendRsToNode(i + 1, v);
        }
      }
      if (rsFirstPoll) rsFirstPoll = false;

      if (parkingRising && isTaskExecuting) {
        int fromTrack = isManualTripActive ? manualFrom : (queueSize > 0 ? taskQueue[0].fromTrack : 0);
        if (fromTrack > 0) sendCommandToNode(fromTrack);
      }

      if (wifiRising && isTaskExecuting) {
        if (isManualTripActive) {
          isManualTripActive = false;
          isTaskExecuting = false;
        } else if (queueSize > 0) {
          for (int i = 0; i < 5; ++i) if (physicalTracks[i] == taskQueue[0].fromTrack) { trackStatuses[i] = 0; break; }
          shiftQueue();
        } else { isTaskExecuting = false; }
        if (queueSize > 0) { waitForCompletionPulse = true; completionPulseSeen = true; }
      }
    }
  }
}

void shiftQueue() {
  if (queueSize <= 0) return;
  for (int i = 0; i < queueSize - 1; i++) taskQueue[i] = taskQueue[i + 1];
  queueSize--;
  isTaskExecuting = false;
}

// ========== ОБРАБОТЧИК РАДИО (ESP-NOW) ==========
void OnDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len) {
  if (len >= 3) {
    char rx_node_buf[16];
    memcpy(rx_node_buf, incomingData, min(len, 15));
    rx_node_buf[min(len, 15)] = '\0';
    String msg = String(rx_node_buf);

    if (msg.startsWith("A")) {
      int parsedTrack = msg.substring(1).toInt();
      if (parsedTrack > 0) {
        for (int i = 0; i < sizeof(physicalTracks)/sizeof(physicalTracks[0]); i++) {
          if (physicalTracks[i] == parsedTrack && trackStatuses[i] == 0) {
            bool alreadyInQueue = false;
            for (int j = 0; j < queueSize; j++) {
              if (taskQueue[j].fromTrack == parsedTrack) alreadyInQueue = true;
            }
            if (!alreadyInQueue && queueSize < MAX_QUEUE) {
              taskQueue[queueSize].fromTrack = parsedTrack;
              taskQueue[queueSize].toTrack = trackRoutes[i].toInt();
              queueSize++;
              trackStatuses[i] = 1;
            }
          }
        }
      }
    }
  }
}

// ========== ЭНДПОИНТЫ ВЕБ-СЕРВЕРА ==========
void handleRoot() { server.send_P(200, "text/html", html_page); }
void handleSimple() { server.send_P(200, "text/html", simple_page); }

void handleSetRoutes() {
  if (server.hasArg("val")) {
    String payload = server.arg("val");
    int startPos = 0;
    for (int i = 0; i < 5; i++) {
      int pipePos = payload.indexOf('|', startPos);
      if (pipePos != -1) { trackRoutes[i] = payload.substring(startPos, pipePos); startPos = pipePos + 1; }
      else { trackRoutes[i] = payload.substring(startPos); break; }
    }
    prefs.begin("routes", false);
    for (int i = 0; i < 5; i++) prefs.putString(("r" + String(i)).c_str(), trackRoutes[i]);
    prefs.end();
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "BAD");
  }
}

void handleRunManualTrip() {
  if (isTaskExecuting || isManualTripActive || taskWriteInProgress) {
    server.send(403, "text/plain", "BUSY"); return;
  }
  if (!startInterlocksClear()) {
    server.send(403, "text/plain", "MODE_OR_SENSOR_INTERLOCK"); return;
  }
  if (server.hasArg("from") && server.hasArg("to") && server.hasArg("rollers")) {
    manualFrom = server.arg("from").toInt();
    manualTo = server.arg("to").toInt();
    int rollers = server.arg("rollers").toInt();
    beginTaskWrites(manualFrom, manualTo, rollers);
    if (!taskWriteInProgress) { server.send(503, "text/plain", "MODBUS_BUSY"); return; }
    isManualTripActive = true;
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "BAD");
  }
}

void handleGetStatusData() {
  String json = "{";
  json += "\"statuses\":[";
  for (int i = 0; i < 5; i++) { json += String(trackStatuses[i]); if (i < 4) json += ","; }
  json += "],";
  json += "\"is_paused\":0,";
  bool busy;
  if (!modbusDataValid) busy = true;
  else busy = modbusBit(16490) || !modbusBit(16489);
  json += "\"is_busy\":" + String(busy ? 1 : 0) + ",";
  json += "\"force_auto\":" + String(forceAutoState ? 1 : 0) + ",";
  json += "\"manual_active\":" + String(isManualTripActive ? 1 : 0) + ",";
  json += "\"manual_from\":" + String(manualFrom) + ",";
  json += "\"manual_to\":" + String(manualTo) + ",";
  json += "\"queue\":[";
  for (int i = 0; i < queueSize; i++) {
    json += "{\"from\":\"" + String(taskQueue[i].fromTrack) + "\",\"to\":\"" + String(taskQueue[i].toTrack) + "\"}";
    if (i < queueSize - 1) json += ",";
  }
  json += "]";
  json += ",\"routes\":[";
  for (int i = 0; i < 5; i++) { json += "\"" + trackRoutes[i] + "\""; if (i < 4) json += ","; }
  json += "]";
  json += "}";
  server.send(200, "application/json", json);
}

void handleCancelTask() {
  if (isTaskExecuting || isManualTripActive || taskWriteInProgress) {
    resetWayPending = true;
    if (isManualTripActive) { isManualTripActive = false; isTaskExecuting = false; taskWriteInProgress = false; }
    else if (queueSize > 0) {
      for (int i = 0; i < 5; ++i) if (physicalTracks[i] == taskQueue[0].fromTrack) { trackStatuses[i] = 0; break; }
      shiftQueue();
    }
    server.send(200, "text/plain", "OK");
  } else server.send(200, "text/plain", "NO_ACTIVE_TASK");
}

void handleGlobalStop() { resetWifiPending = true; server.send(200, "text/plain", "OK"); }

void handleForceAuto() {
  if (server.hasArg("val")) {
    bool v = server.arg("val").toInt() != 0;
    if (writeModbusRegister(REG_FORCE_AUTO, v ? 1 : 0)) {
      forceAutoState = v;
      server.send(200, "text/plain", "OK");
    } else {
      server.send(503, "text/plain", "MODBUS_FAIL");
    }
  } else {
    server.send(400, "text/plain", "BAD");
  }
}

void handleGlobalClear() {
  resetWifiPending = true;
  for (int i = 0; i < 5; ++i) trackStatuses[i] = 0;
  queueSize = 0;
  isTaskExecuting = false;
  isManualTripActive = false;
  taskWriteInProgress = false;
  waitForCompletionPulse = false;
  completionPulseSeen = false;
  server.send(200, "text/plain", "OK");
}

void handleSetInput() {
  if (!server.hasArg("reg") || !server.hasArg("value")) { server.send(400, "text/plain", "BAD_ARGS"); return; }
  uint16_t reg = (uint16_t)server.arg("reg").toInt();
  int value = server.arg("value").toInt();
  if ((reg != 16469 && reg != 16470) || (value != 0 && value != 1)) { server.send(400, "text/plain", "BAD_VALUE"); return; }
  bool ok = writeModbusRegister(reg, (uint16_t)value);
  server.send(ok ? 200 : 503, "text/plain", ok ? "OK" : "MODBUS_ERROR");
}

void handleGetModbusData() {
  if (!modbusDataValid) { server.send(503, "application/json", "{\"valid\":false}"); return; }
  String json = "{\"valid\":true,\"values\":[";
  for (uint8_t i = 0; i < REG_READ_COUNT; ++i) { if (i) json += ","; json += String(modbusValues[i]); }
  json += "]}";
  server.send(200, "application/json", json);
}

// ========== ПРОСТАЯ СТРАНИЦА (РУЧНОЙ РЕЙС) ==========
const char simple_page[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1.0,maximum-scale=1.0,user-scalable=no">
<title>Telega Simple</title>
<style>
  * { margin:0; padding:0; box-sizing:border-box; }
  body { background:#04010d; overflow:hidden; font-family:'Courier New',monospace;
         user-select:none; -webkit-user-select:none; height:100vh; width:100vw; }
  .top-bar { position:fixed; top:6px; left:10px; right:10px; display:flex;
             align-items:center; gap:10px; z-index:999; pointer-events:none; }
  .ip-addr { background:rgba(0,0,0,.5); color:#ffaa00; font-size:10px;
             padding:3px 8px; border-radius:12px; backdrop-filter:blur(4px); }
  .status-dot { width:12px; height:12px; border-radius:50%; background:#00ff00;
                box-shadow:0 0 8px #00ff00; transition:all .3s; }
  .status-dot.blocked { background:#ff0000; box-shadow:0 0 8px #ff0000; }
  .auto-switch { display:inline-flex; align-items:center; gap:6px; pointer-events:auto;
                 background:rgba(0,0,0,.5); padding:4px 10px; border-radius:12px;
                 border:1px solid rgba(0,240,255,.3); }
  .auto-switch input { display:none; }
  .auto-slider { width:34px; height:16px; background:#120224; border:1px solid #ff0066;
                 border-radius:10px; position:relative; transition:.15s; }
  .auto-slider::after { content:""; position:absolute; top:1px; left:1px; width:12px;
                        height:12px; background:#ff0066; border-radius:50%;
                        transition:left .15s,background .15s; }
  .auto-switch input:checked + .auto-slider { background:#003322; border-color:#00f0ff; }
  .auto-switch input:checked + .auto-slider::after { left:18px; background:#00f0ff; }
  .auto-label { font-size:10px; color:#888; font-weight:bold; }
  .auto-switch input:checked ~ .auto-label { color:#00f0ff; }
  .scene { position:relative; width:100vw; height:100vh; perspective:130px; perspective-origin:50% 45%; }
  .sky { position:absolute; top:0; left:0; width:100%; height:45%;
         background:linear-gradient(to bottom,#05010f 0%,#1f033a 50%,#ff0066 100%); }
  .fog { position:absolute; top:40%; left:0; width:100%; height:10%;
         background:linear-gradient(to bottom,transparent,#ff0066,#06020f);
         opacity:.6; pointer-events:none; z-index:2; }
  .preview { position:absolute; top:7%; left:50%; transform:translateX(-50%);
             background:#ffcc00; border:6px solid #000; border-radius:40px;
             padding:14px 26px; text-align:center; z-index:15; opacity:0;
             visibility:hidden; transition:all .2s; box-shadow:0 0 40px rgba(0,240,255,.5);
             white-space:nowrap; pointer-events:none; }
  .preview.show { opacity:1; visibility:visible; animation:pulse .8s ease-out; }
  @keyframes pulse { 0%{transform:translateX(-50%) scale(.8);opacity:0}
                     80%{transform:translateX(-50%) scale(1.05)}
                     100%{transform:translateX(-50%) scale(1);opacity:1} }
  .preview-main { font-size:34px; font-weight:bold; color:#000; letter-spacing:4px; }
  .preview-sub { font-size:14px; font-weight:bold; color:#000; margin-top:4px; letter-spacing:2px; }
  .go-btn { position:absolute; top:24%; left:50%; transform:translateX(-50%);
            width:85%; max-width:280px; padding:16px 0; background:rgba(18,2,36,.75);
            border:2px solid #00f0ff; border-radius:60px;
            box-shadow:0 0 20px rgba(0,240,255,.4); cursor:pointer; font-family:'Courier New',monospace;
            font-size:24px; font-weight:bold; color:#00f0ff; text-shadow:0 0 10px #00f0ff;
            text-align:center; letter-spacing:4px; transition:all .1s; z-index:10;
            -webkit-tap-highlight-color:transparent; }
  .go-btn:active { transform:translateX(-50%) scale(.97); background:rgba(0,240,255,.25);
                   box-shadow:0 0 35px rgba(0,240,255,.8); color:#fff; text-shadow:0 0 15px #fff; }
  .go-btn:disabled { background:#333; border-color:#555; color:#888; cursor:not-allowed;
                     box-shadow:none; text-shadow:none; }
  .track-container { position:absolute; top:45%; left:0; width:100%; height:55%;
                     transform:rotateX(74deg); transform-origin:top center;
                     background:#06020f; overflow:hidden; }
  .road-line-left, .road-line-right { position:absolute; top:0; width:3px; height:100%;
    background:#fff; box-shadow:0 0 12px #fff; transform-origin:top center; }
  .road-line-left { left:50%; transform:translateX(-120px); }
  .road-line-right { left:50%; transform:translateX(120px); }
  .dashed-center { position:absolute; left:50%; top:0; width:3px; height:100%;
    transform:translateX(-50%); background:repeating-linear-gradient(0deg,#fff 0,#fff 20px,
    transparent 20px,transparent 55px); box-shadow:0 0 10px #fff;
    animation:moveDash .6s linear infinite; }
  @keyframes moveDash { 0%{background-position:0 0} 100%{background-position:0 75px} }
  .controls-left, .controls-right { position:absolute; top:75%; transform:translateY(-50%);
    z-index:20; display:flex; flex-direction:column; align-items:center; gap:12px; }
  .controls-left { left:6%; } .controls-right { right:6%; }
  .dial-item { background:rgba(0,0,0,.5); border-radius:12px; padding:5px; }
  .dial-label { text-align:center; font-size:12px; color:#ffaa00; letter-spacing:2px;
                margin-bottom:5px; font-family:'Courier New',monospace; text-shadow:0 0 8px #00ffaa; }
  .dial-box { width:85px; height:70px; background:#120224; border:2px solid #00f0ff;
              border-radius:8px; box-shadow:0 0 15px rgba(0,240,255,.4);
              overflow-y:scroll; scrollbar-width:none; scroll-snap-type:y mandatory; }
  .dial-box::-webkit-scrollbar { display:none; }
  .dial-strip { list-style:none; padding:0; margin:0; }
  .dial-strip li { height:70px; line-height:70px; font-weight:bold; font-size:36px;
                   color:#00f0ff; text-shadow:0 0 10px #00f0ff; text-align:center;
                   scroll-snap-align:center; }
  .rollers-block { position:absolute; left:50%; bottom:4%; transform:translateX(-50%);
                   z-index:20; display:flex; flex-direction:column; align-items:center;
                   gap:6px; }
  .rollers-title { font-size:11px; color:#ffaa00; letter-spacing:2px; text-shadow:0 0 8px #00ffaa; }
  .rollers-row { display:flex; gap:8px; }
  .roller-radio { position:relative; }
  .roller-radio input { display:none; }
  .roller-radio label { display:block; padding:10px 14px; font-size:13px; font-weight:bold;
    background:#120224; border:2px solid #ff0066; border-radius:12px; color:#ff0066;
    text-shadow:0 0 6px #ff0066; cursor:pointer; transition:.15s; min-width:64px; text-align:center;
    -webkit-tap-highlight-color:transparent; }
  .roller-radio input:checked + label { border-color:#00f0ff; color:#00f0ff;
    background:rgba(0,240,255,.15); text-shadow:0 0 10px #00f0ff;
    box-shadow:0 0 14px rgba(0,240,255,.6); }
  .roller-radio label:active { transform:scale(.96); }
</style>
</head>
<body>

<div class="top-bar">
  <div class="ip-addr" id="espIp">--.--.--.--</div>
  <div class="status-dot" id="statusDot"></div>
  <label class="auto-switch">
    <input type="checkbox" id="autoChk">
    <span class="auto-slider"></span>
    <span class="auto-label">AUTO</span>
  </label>
</div>

<div class="scene">
  <div class="sky"></div>
  <div class="fog"></div>
  <div class="preview" id="previewBlock">
    <div class="preview-main" id="previewMain">--</div>
    <div class="preview-sub" id="previewSub">-- --</div>
  </div>
  <button class="go-btn" id="goBtn">В ПУТЬ!</button>
  <div class="track-container">
    <div class="road">
      <div class="road-line-left"></div>
      <div class="road-line-right"></div>
      <div class="dashed-center"></div>
    </div>
  </div>
</div>

<div class="controls-left">
  <div class="dial-item">
    <div class="dial-label">ОТКУДА</div>
    <div class="dial-box" id="dial1"><ul class="dial-strip" id="strip1"></ul></div>
  </div>
</div>

<div class="controls-right">
  <div class="dial-item">
    <div class="dial-label">КУДА</div>
    <div class="dial-box" id="dial2"><ul class="dial-strip" id="strip2"></ul></div>
  </div>
</div>

<div class="rollers-block">
  <div class="rollers-title">РОЛИКИ</div>
  <div class="rollers-row">
    <div class="roller-radio"><input type="radio" name="rollers" id="r1" value="1" checked><label for="r1">1</label></div>
    <div class="roller-radio"><input type="radio" name="rollers" id="r2" value="2"><label for="r2">2</label></div>
    <div class="roller-radio"><input type="radio" name="rollers" id="r0" value="0"><label for="r0">ПУСТОЙ</label></div>
  </div>
</div>

<script>
fetch('/ip').then(r=>r.text()).then(ip=>{document.getElementById('espIp').innerText=ip;})
  .catch(()=>{document.getElementById('espIp').innerText='no net';});

function createStrip(id){
  let s=''; for(let i=1;i<=60;i++){let n=i<10?'0'+i:i; s+='<li data-val="'+n+'">'+n+'</li>';}
  document.getElementById(id).innerHTML=s;
}
createStrip('strip1'); createStrip('strip2');
const dial1=document.getElementById('dial1'), dial2=document.getElementById('dial2');
dial1.scrollTop=0; dial2.scrollTop=0;
function getDialValue(dial, stripId){
  const items=document.getElementById(stripId).getElementsByTagName('li');
  let idx=Math.round(dial.scrollTop/70);
  idx=Math.min(Math.max(idx,0),items.length-1);
  return items[idx].getAttribute('data-val');
}

let pendingFrom=null, pendingTo=null, pendingRollers=null;
const goBtn=document.getElementById('goBtn');
const previewBlock=document.getElementById('previewBlock');
const previewMain=document.getElementById('previewMain');
const previewSub=document.getElementById('previewSub');

function resetConfirmation(){
  if(pendingFrom===null) return;
  pendingFrom=pendingTo=pendingRollers=null;
  previewBlock.classList.remove('show');
  goBtn.textContent='В ПУТЬ!';
  goBtn.style.borderColor='#00f0ff';
  goBtn.style.boxShadow='0 0 20px rgba(0,240,255,.4)';
  goBtn.style.color='#00f0ff';
  goBtn.style.textShadow='0 0 10px #00f0ff';
}
dial1.addEventListener('scroll', resetConfirmation);
dial2.addEventListener('scroll', resetConfirmation);
document.querySelectorAll('input[name="rollers"]').forEach(r=>r.addEventListener('change', resetConfirmation));

function showPreview(from,to,rollers){
  previewMain.textContent=from+' / '+to;
  previewSub.textContent = rollers==='0' ? 'ПУСТОЙ' : (rollers==='2'?'2 РОЛИКА':'1 РОЛИК');
  previewBlock.classList.remove('show'); void previewBlock.offsetWidth;
  previewBlock.classList.add('show');
  setTimeout(()=>{ if(pendingFrom!==null) return; previewBlock.classList.remove('show'); },3000);
}

goBtn.onclick=function(){
  if(pendingFrom===null){
    const from=getDialValue(dial1,'strip1');
    const to=getDialValue(dial2,'strip2');
    const rollers=document.querySelector('input[name="rollers"]:checked').value;
    pendingFrom=from; pendingTo=to; pendingRollers=rollers;
    showPreview(from,to,rollers);
    goBtn.textContent='ПОДТВЕРДИТЬ';
    goBtn.style.borderColor='#ff0066';
    goBtn.style.boxShadow='0 0 20px rgba(255,0,102,.6)';
    goBtn.style.color='#ff0066';
    goBtn.style.textShadow='0 0 10px #ff0066';
  } else {
    const q='/run_manual_trip?from='+pendingFrom+'&to='+pendingTo+'&rollers='+pendingRollers;
    fetch(q).then(r=>r.text().then(t=>{
      if(r.ok){
        goBtn.textContent='ЗАПУЩЕНО';
        goBtn.style.borderColor='#00ff66'; goBtn.style.color='#00ff66';
      } else {
        goBtn.textContent='ОТКАЗ: '+t;
        goBtn.style.borderColor='#ff3300'; goBtn.style.color='#ff3300';
      }
    })).catch(()=>{
      goBtn.textContent='НЕТ СВЯЗИ';
      goBtn.style.borderColor='#ff3300'; goBtn.style.color='#ff3300';
    }).finally(()=>{
      pendingFrom=pendingTo=pendingRollers=null;
      setTimeout(()=>{ document.getElementById('previewBlock').classList.remove('show'); },500);
      setTimeout(()=>{
        goBtn.textContent='В ПУТЬ!';
        goBtn.style.borderColor='#00f0ff'; goBtn.style.color='#00f0ff';
        goBtn.style.boxShadow='0 0 20px rgba(0,240,255,.4)';
        goBtn.style.textShadow='0 0 10px #00f0ff';
      },1500);
    });
  }
};

function pollStatus(){
  fetch('/status').then(r=>r.text()).then(s=>{
    const blocked = (s==='1');
    document.getElementById('statusDot').classList.toggle('blocked', blocked);
    goBtn.disabled = blocked;
    if(blocked){ goBtn.textContent='ЗАНЯТО'; }
    else if(pendingFrom===null){ goBtn.textContent='В ПУТЬ!'; }
    else { goBtn.textContent='ПОДТВЕРДИТЬ'; }
  }).catch(()=>{});
}
setInterval(pollStatus,500); pollStatus();

let autoBusy=false;
const autoChk=document.getElementById('autoChk');
function loadAuto(){
  fetch('/get_status_data').then(r=>r.json()).then(d=>{
    autoChk.checked = (d.force_auto===1);
  }).catch(()=>{});
}
loadAuto(); setInterval(loadAuto,2000);
autoChk.onchange=function(){
  if(autoBusy) return;
  autoBusy=true;
  const want=this.checked?1:0;
  fetch('/force_auto?val='+want).then(r=>{ if(!r.ok) throw 0; })
    .catch(()=>{ this.checked=!this.checked; })
    .finally(()=>{ autoBusy=false; });
};
</script>
</body>
</html>
)rawliteral";

// ========== КОД СТРАНИЦЫ ИНТЕРФЕЙСА (ГЛАВНАЯ) ==========
const char html_page[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
    <title>Пульт Управления Конвейером</title>
    <style>
        * { box-sizing: border-box; margin: 0; padding: 0; font-family: monospace; }
        body { background: #111; color: #fff; padding: 10px; font-size: 16px; }
        .section-title { font-weight: bold; background: #222; padding: 5px; margin-bottom: 5px; text-align: center; border: 1px solid #444; }
        .table-container { width: 100%; margin-bottom: 15px; }
        table { width: 100%; border-collapse: collapse; text-align: left; }
        th, td { padding: 8px; border: 1px solid #444; vertical-align: middle; }
        th { background: #222; font-weight: bold; }
        input[type="text"], select { background: #000; color: #fff; border: 1px solid #666; padding: 5px; font-size: 16px; text-align: center; }
        input[type="text"], input[type="number"]  { width: 70px; min-width: 50px; }
        select { width: 100%; height: 32px; font-family: monospace; }
        input:focus, select:focus { border-color: #00f0ff; outline: none; }
        button { width: 100%; padding: 12px; background: #333; color: #fff; border: 1px solid #555; font-weight: bold; cursor: pointer; font-size: 14px; }
        button:active { background: #555; }
        .status-cell { text-align: center; font-weight: bold; width: 80px; }
        .st-0 { color: #888; }
        .st-1 { color: #ffcc00; }
        .st-2 { color: #00ff66; }
        .queue-container { width: 100%; display: flex; flex-direction: column; gap: 4px; }
        .task-row { display: flex; width: 100%; border: 1px solid #444; background: #222; align-items: center; min-height: 50px; }
        .task-row.active { background: #332222; border-color: #ff3333; }
        .task-row.active.paused { background: #333322; border-color: #ffcc00; }
        .cancel-btn-cell { width: 100px; padding: 5px; }
        .cancel-btn { background: #aa2222; border-color: #ff5555; padding: 8px; width: 100%; height: 100%; }
        .cancel-btn:active { background: #ff3333; }
        .task-text { flex-grow: 1; text-align: center; font-size: 18px; font-weight: bold; padding: 5px; }
        .task-status { width: 100px; text-align: center; font-size: 12px; border-left: 1px solid #444; padding: 5px; }
        .bottom-actions { margin-top: 15px; display: flex; gap: 10px; }
        .btn-danger { background: #551111; border-color: #aa3333; }
        .btn-danger:active { background: #aa2222; }
        .manual-btn { background: #004433; border-color: #00aa66; padding: 5px; font-size: 14px; }
        .manual-btn:active { background: #00aa66; }
        .manual-inputs { display: flex; gap: 5px; align-items: center; }
        .manual-inputs span { font-size: 12px; }
        .manual-inputs input[type="number"] { width: 50px; }
        .manual-inputs select { width: 70px; }
.status-dot {
  display: inline-block;
  width: 12px;
  height: 12px;
  border-radius: 50%;
  margin-left: 10px;
  vertical-align: middle;
  box-shadow: 0 0 8px rgba(0,0,0,0.5);
}
.dot-free { background: #00ff66; box-shadow: 0 0 10px #00ff66; }
.dot-busy { background: #ff3333; box-shadow: 0 0 10px #ff3333; animation: blink 1s infinite; }
@keyframes blink { 0% { opacity: 0.3; } 50% { opacity: 1; } 100% { opacity: 0.3; } }
        .modbus-values { width: 100%; margin-top: 6px; border: 1px solid #444; }
        .modbus-row { display: flex; justify-content: space-between; gap: 12px; padding: 7px 9px; border-bottom: 1px solid #333; background: #181818; }
        .modbus-row:last-child { border-bottom: 0; }
        .modbus-name { overflow-wrap: anywhere; color: #ddd; }
        .modbus-value { min-width: 28px; text-align: right; font-weight: bold; color: #00ffaa; }
        .auto-switch { display:inline-flex; align-items:center; gap:6px; cursor:pointer; user-select:none; }
.auto-switch input { display:none; }
.auto-slider {
  width: 38px; height: 18px; background: #444; border: 1px solid #666;
  border-radius: 10px; position: relative; transition: background .15s;
}
.auto-slider::after {
  content: ""; position: absolute; top: 1px; left: 1px;
  width: 14px; height: 14px; background: #ccc; border-radius: 50%;
  transition: left .15s, background .15s;
}
.auto-switch input:checked + .auto-slider { background: #006633; border-color: #00aa66; }
.auto-switch input:checked + .auto-slider::after { left: 21px; background: #00ffaa; }
.auto-label { font-size: 12px; color: #aaa; }
.auto-switch input:checked ~ .auto-label { color: #00ffaa; }
        .cart-panel { width: 100%; max-width: 260px; margin: 5px auto 0; padding: 0 4px; }
        .cart-body {
          --r1-x: 9%;  --r1-y: 13%; --r1-w: 82%; --r1-h: 27%;
          --r2-x: 9%;  --r2-y: 59%; --r2-w: 82%; --r2-h: 27%;
          --s1-x: 7%;  --s1-y: 27%;
          --s2-x: 93%; --s2-y: 27%;
          --s3-x: 7%;  --s3-y: 72%;
          --s4-x: 93%; --s4-y: 72%;
          position: relative; width: 100%; aspect-ratio: 1.3; overflow: hidden;
          background: linear-gradient(145deg,#168db5 0%,#086a8c 60%,#06445d 100%);
          border: 2px solid #263b47; border-radius: 12px;
          box-shadow: inset 0 2px 4px #ffffff25, 0 2px 5px #0007;
        }
        .cart-body:before { content: ""; position: absolute; inset: 2%; border: 1px solid #ffffff28; border-radius: 8px; pointer-events: none; }
        .cart-roller {
          position: absolute; box-sizing: border-box;
          border: 2px solid #26343c; border-radius: 8px;
          background: linear-gradient(135deg,#6bff91,#079b43);
          box-shadow: inset 0 1px 3px #ffffff55, 0 2px 3px #00253670;
          transition: background .2s, box-shadow .2s;
        }
        .cart-roller:after { content: ""; position: absolute; inset: 7% 3%; border: 1px solid #ffffff45; border-radius: 5px; pointer-events: none; }
        .cart-roller.full { background: linear-gradient(135deg,#ff7777,#d90018); box-shadow: inset 0 1px 4px #ffffff70, 0 0 7px #ff263c88; }
        .cart-roller.r1 { left: var(--r1-x); top: var(--r1-y); width: var(--r1-w); height: var(--r1-h); }
        .cart-roller.r2 { left: var(--r2-x); top: var(--r2-y); width: var(--r2-w); height: var(--r2-h); }
        .cart-sensor {
          position: absolute; left: 50%; top: 50%; width: 8%; aspect-ratio: 1;
          border-radius: 50%; border: 1.5px solid #1c2a31;
          background: radial-gradient(circle at 35% 30%,#9ba4aa,#343b40 72%);
          transform: translate(-50%,-50%);
          box-shadow: inset 0 1px 2px #ffffff45, 0 1px 3px #00172299;
          transition: background .18s, box-shadow .18s;
        }
        .cart-sensor.on { background: radial-gradient(circle at 35% 30%,#ff7777,#e3122a 70%); box-shadow: 0 0 6px #ff263c, inset 0 1px 2px #ffffff99; }
        .cart-sensor.s1 { left: var(--s1-x); top: var(--s1-y); }
        .cart-sensor.s2 { left: var(--s2-x); top: var(--s2-y); }
        .cart-sensor.s3 { left: var(--s3-x); top: var(--s3-y); }
        .cart-sensor.s4 { left: var(--s4-x); top: var(--s4-y); }
        .cart-arrow { display: block; width: 50%; height: auto; margin: 0 auto; padding: 0; border: 0; background: transparent; cursor: pointer; -webkit-tap-highlight-color: transparent; }
        .cart-arrow svg { display: block; width: 100%; height: auto; filter: drop-shadow(0 1px 2px #0008); }
        .cart-arrow polygon { fill: #69747b; stroke: #26343c; stroke-width: 3; stroke-linejoin: round; transition: fill .18s, filter .18s; }
        .cart-arrow.active polygon { fill: #18d875; filter: drop-shadow(0 0 3px #18d87599); }
        .cart-arrow:active { background: transparent; transform: scale(.96); }
        @media (max-width: 380px) { .cart-panel { max-width: 260px; padding-left: 2px; padding-right: 2px; } }
    </style>
</head>
<body>

    <div class="section-title" style="display:flex; align-items:center; justify-content:center; gap:10px;">
  МАРШРУТЫ
  <label class="auto-switch">
    <input type="checkbox" id="force_auto_chk">
    <span class="auto-slider"></span>
    <span class="auto-label">AUTO</span>
  </label>
  <span id="plc_busy_dot" class="status-dot dot-free"></span>
</div>

    <div class="table-container">
        <table>
            <thead>
                <tr>
                    <th>Источник</th>
                    <th style="width: 90px; text-align: center;">Статус</th>
                    <th>Куда везти / Настройка</th>
                </tr>
            </thead>
            <tbody>
                <tr>
                    <td>KDT 13</td>
                    <td class="status-cell st-0" id="node_st_1">0</td>
                    <td><input type="number" min="0" max="99" inputmode="numeric" id="route_1" value="14"></td>
                </tr>
                <tr>
                    <td>KDT 15</td>
                    <td class="status-cell st-0" id="node_st_2">0</td>
                    <td><input type="number" min="0" max="99" inputmode="numeric" id="route_2" value="16"></td>
                </tr>
                <tr>
                    <td>BS_1_2-25</td>
                    <td class="status-cell st-0" id="node_st_3">0</td>
                    <td><input type="number" min="0" max="99" inputmode="numeric" id="route_3" value="15"></td>
                </tr>
                <tr>
                    <td>BS_3_4-41</td>
                    <td class="status-cell st-0" id="node_st_4">0</td>
                    <td><input type="number" min="0" max="99" inputmode="numeric" id="route_4" value="02"></td>
                </tr>
                <tr>
                    <td>Дорожка 5</td>
                    <td class="status-cell st-0" id="node_st_5">0</td>
                    <td><input type="number" min="0" max="99" inputmode="numeric" id="route_5" value="09"></td>
                </tr>
                <tr style="background:#1a2925;">
                    <td style="font-weight:bold;color:#00ffaa;">6. РЕЙС</td>
                    <td><a class="manual-btn" href="/simple"
                           style="display:block;text-align:center;text-decoration:none;line-height:1.2;">
                           ПУСК</a></td>
                    <td style="text-align:center;color:#888;font-size:12px;">
                        Ручной рейс — на отдельной странице
                    </td>
                </tr>
            </tbody>
        </table>
    </div>
    <button id="saveRoutesBtn">ОБНОВИТЬ МАРШРУТЫ АВТОМАТИКИ</button>

    <div class="section-title" style="margin-top: 20px;">ОЧЕРЕДЬ ВЫПОЛНЕНИЯ ЗАДАЧ</div>
    <div class="queue-container" id="queueList"></div>

    <div class="bottom-actions">
        <button class="btn-danger" id="stopAllBtn">СБРОС</button>
        <button class="btn-danger" id="clearAllBtn">СБРОСИТЬ ВСЁ</button>
    </div>
    <div class="cart-panel" aria-label="Состояние тележки">
        <button type="button" class="cart-arrow" id="motorForward" aria-label="Двигатель вперёд" data-reg="16469" data-value="0">
            <svg viewBox="0 0 100 42" aria-hidden="true"><polygon points="4,38 50,4 96,38"></polygon></svg>
        </button>
        <div class="cart-body">
            <div class="cart-roller r1" id="roller1"></div>
            <div class="cart-roller r2" id="roller2"></div>
            <div class="cart-sensor s1" id="sensorLoad1" title="Датчик загрузки первого ролика"></div>
            <div class="cart-sensor s2" id="sensorUnload1" title="Датчик выгрузки первого ролика"></div>
            <div class="cart-sensor s3" id="sensorLoad2" title="Датчик загрузки второго ролика"></div>
            <div class="cart-sensor s4" id="sensorUnload2" title="Датчик выгрузки второго ролика"></div>
        </div>
        <button type="button" class="cart-arrow" id="motorBackward" aria-label="Двигатель назад" data-reg="16470" data-value="0">
            <svg viewBox="0 0 100 42" aria-hidden="true"><polygon points="4,4 96,4 50,38"></polygon></svg>
        </button>
    </div>
<div class="section-title" style="margin-top: 20px;">ДОП. РЕГИСТРЫ</div>
<div class="modbus-values">
  <div class="modbus-row"><span class="modbus-name">16496</span><span class="modbus-value" id="extra_0">—</span></div>
  <div class="modbus-row"><span class="modbus-name">16497</span><span class="modbus-value" id="extra_1">—</span></div>
  <div class="modbus-row"><span class="modbus-name">16498</span><span class="modbus-value" id="extra_2">—</span></div>
  <div class="modbus-row"><span class="modbus-name">16499</span><span class="modbus-value" id="extra_3">—</span></div>
</div>

<script>
    let routesLoaded = false;

    document.getElementById('saveRoutesBtn').onclick = function() {
        let payload = `${document.getElementById('route_1').value}|${document.getElementById('route_2').value}|${document.getElementById('route_3').value}|${document.getElementById('route_4').value}|${document.getElementById('route_5').value}`;
        fetch('/set_routes?val=' + encodeURIComponent(payload))
            .then(res => { this.style.borderColor = '#00ff66'; setTimeout(() => this.style.borderColor='#555', 1000); })
            .catch(() => { this.style.borderColor = '#ff3300'; setTimeout(() => this.style.borderColor='#555', 1000); });
    };

    function cancelActiveTask() {
        fetch('/cancel_task').catch(() => {});
    }

    document.getElementById('stopAllBtn').onclick = function() { fetch('/global_stop'); };
    let forceAutoBusy = false;

document.getElementById('force_auto_chk').onchange = function() {
  if (forceAutoBusy) return;
  forceAutoBusy = true;
  const want = this.checked ? 1 : 0;
  fetch('/force_auto?val=' + want)
    .then(r => { if (!r.ok) throw new Error('fail'); })
    .catch(() => { this.checked = !this.checked; })
    .finally(() => { forceAutoBusy = false; });
};

    document.getElementById('clearAllBtn').onclick = function() { fetch('/global_clear'); };

    function updateDashboard() {
        fetch('/get_status_data')
            .then(res => res.json())
            .then(data => {
let busyDot = document.getElementById('plc_busy_dot');
if (data.is_busy === 1) {
    busyDot.className = 'status-dot dot-busy';
} else {
    busyDot.className = 'status-dot dot-free';
}

                if (!routesLoaded && data.routes) {
                    for (let i = 1; i <= 5; i++) {
                        document.getElementById('route_' + i).value = data.routes[i-1];
                    }
                    routesLoaded = true;
                }

                for(let i = 1; i <= 5; i++) {
                    let cell = document.getElementById('node_st_' + i);
                    let statusValue = data.statuses[i-1];
                    cell.innerText = statusValue;
                    cell.className = 'status-cell st-' + statusValue;
                }

                let queueContainer = document.getElementById('queueList');
                queueContainer.innerHTML = '';

                if (data.manual_active === 1) {
                    let row = document.createElement('div');
                    row.className = 'task-row active';
                    if (data.is_paused === 1) row.classList.add('paused');
                    row.innerHTML = `
                        <div class="cancel-btn-cell"><button class="cancel-btn" onclick="cancelActiveTask()">ОТМЕНА</button></div>
                        <div class="task-text" style="color: #00ffaa;">[РУЧНОЙ] ${data.manual_from} → ${data.manual_to}</div>
                        <div class="task-status">${data.is_paused === 1 ? 'ПАУЗА ПЛК' : 'В ПУТИ'}</div>
                    `;
                    queueContainer.appendChild(row);
                    return;
                }

                if (data.queue.length === 0) {
                    queueContainer.innerHTML = '<div class="task-row"><div class="task-text">ЗАДАЧ НЕТ (ОЖИДАНИЕ)</div></div>';
                    return;
                }

                data.queue.forEach((task, index) => {
                    let row = document.createElement('div');
                    row.className = 'task-row';

                    if (index === 0) {
                        row.classList.add('active');
                        if (data.is_paused === 1) row.classList.add('paused');
                        let btnCell = document.createElement('div');
                        btnCell.className = 'cancel-btn-cell';
                        btnCell.innerHTML = '<button class="cancel-btn" onclick="cancelActiveTask()">ОТМЕНА</button>';
                        row.appendChild(btnCell);
                    } else {
                        let spacer = document.createElement('div');
                        spacer.className = 'cancel-btn-cell';
                        row.appendChild(spacer);
                    }

                    let textCell = document.createElement('div');
                    textCell.className = 'task-text';
                    textCell.innerText = `${task.from} \u2192 ${task.to}`;
                    row.appendChild(textCell);

                    let statusCell = document.createElement('div');
                    statusCell.className = 'task-status';
                    if (index === 0) {
                        statusCell.innerText = data.is_paused === 1 ? 'ПАУЗА ПЛК' : 'В ПУТИ';
                    } else {
                        statusCell.innerText = 'В ОЧЕРЕДИ';
                    }
                    row.appendChild(statusCell);
                    queueContainer.appendChild(row);
                });
            })
            .catch(() => {});
    }

    function updateModbusValues() {
      const status = document.getElementById('modbus_status');
      fetch('/get_modbus_data')
        .then(r => { if (!r.ok) throw new Error('no data'); return r.json(); })
        .then(data => {
          if (!data.valid || !Array.isArray(data.values) || data.values.length !== modbusNames.length) {
            throw new Error('invalid');
          }
          status.textContent = 'Связь OK';
          data.values.forEach((v, i) => {
            const el = document.getElementById('mbv_' + i);
            if (el) el.textContent = v;
          });
        })
        .catch(() => {
          status.textContent = 'Нет связи';
        });
    }

    let cartRequestInProgress = false;
    function updateCart() {
      if (cartRequestInProgress) return;
      cartRequestInProgress = true;
      fetch('/get_modbus_data')
        .then(r => { if (!r.ok) throw new Error('no data'); return r.json(); })
        .then(data => {
         if (!data.valid || !Array.isArray(data.values) || data.values.length < 19) return;
          const v = data.values;
          document.getElementById('sensorUnload1').classList.toggle('on', Number(v[0]) === 1);
          document.getElementById('sensorLoad1').classList.toggle('on', Number(v[1]) === 1);
          document.getElementById('sensorLoad2').classList.toggle('on', Number(v[2]) === 1);
          document.getElementById('sensorUnload2').classList.toggle('on', Number(v[3]) === 1);
          document.getElementById('motorForward').classList.toggle('active', Number(v[4]) === 1);
          document.getElementById('motorBackward').classList.toggle('active', Number(v[5]) === 1);
          document.getElementById('roller1').classList.toggle('full', Number(v[8]) === 1);
          document.getElementById('roller2').classList.toggle('full', Number(v[9]) === 1);
          for (let i = 0; i < 4; i++) {
            const el = document.getElementById('extra_' + i);
            if (el && data.values.length >= 23) el.textContent = data.values[19 + i];
          }
        })
        .catch(() => {
        })
        .finally(() => {
          cartRequestInProgress = false;
          setTimeout(updateCart, 200);
        });
    }

    function setupCartArrow(id) {
      const button = document.getElementById(id);
      button.addEventListener('click', () => {
        const next = button.dataset.value === '1' ? 0 : 1;
        const reg = button.dataset.reg;
        fetch(`/set_input?reg=${reg}&value=${next}`, { method: 'GET' })
          .then(r => { if (!r.ok) throw new Error('write failed'); button.dataset.value = String(next); })
          .catch(() => {});
      });
    }
    setupCartArrow('motorForward');
    setupCartArrow('motorBackward');
    updateCart();

    updateDashboard();
    setInterval(updateDashboard, 1000);
</script>
</body>
</html>
)rawliteral";

// --- ФУНКЦИЯ ОТПРАВКИ МАРКЕРА АКТИВАЦИИ НА НОДУ ---
void sendCommandToNode(int trackNum) {
  String trackStr = String(trackNum);
  if (trackNum < 10) trackStr = "0" + trackStr;
  String payload = "A" + trackStr;
  int msgLen = payload.length();
  uint8_t dataBuf[16];
  payload.getBytes(dataBuf, msgLen + 1);
  esp_now_send(broadcastMac, dataBuf, msgLen);
}

void sendRsToNode(uint8_t n, uint16_t val) {
  String payload = String(n) + "RS" + String(val);
  uint8_t buf[16];
  payload.getBytes(buf, payload.length() + 1);
  esp_now_send(broadcastMac, buf, payload.length());
}

// ========== SETUP & LOOP ==========
void setup() {
  prefs.begin("routes", true);
  for (int i = 0; i < 5; i++) {
    trackRoutes[i] = prefs.getString(("r" + String(i)).c_str(), trackRoutes[i]);
  }
  prefs.end();

  modbusSerial.begin(MODBUS_BAUD, SERIAL_8N1, MODBUS_RX_PIN, MODBUS_TX_PIN);
  modbus.begin(MODBUS_SLAVE_ID, modbusSerial);

  modbusDataValid = false;
  lastModbusPoll = millis();

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(ssid, password);
  WiFi.setSleep(false);
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() == ESP_OK) {
    esp_now_register_recv_cb(esp_now_recv_cb_t(OnDataRecv));
  }

  server.on("/", handleRoot);
  server.on("/simple", handleSimple);
  server.on("/set_routes", handleSetRoutes);
  server.on("/run_manual_trip", handleRunManualTrip);
  server.on("/get_status_data", handleGetStatusData);
  server.on("/get_modbus_data", handleGetModbusData);
  server.on("/set_input", handleSetInput);
  server.on("/cancel_task", handleCancelTask);
  server.on("/global_stop", handleGlobalStop);
  server.on("/force_auto", handleForceAuto);
  server.on("/global_clear", handleGlobalClear);
  server.on("/ip", [](){ server.send(200, "text/plain", WiFi.softAPIP().toString()); });
  server.on("/status", [](){
    bool blocked = isTaskExecuting || isManualTripActive || taskWriteInProgress;
    server.send(200, "text/plain", blocked ? "1" : "0");
  });

  server.begin();

  esp_now_peer_info_t peerInfo;
  memset(&peerInfo, 0, sizeof(peerInfo));
  memcpy(peerInfo.peer_addr, broadcastMac, 6);
  peerInfo.channel = 1;
  peerInfo.encrypt = false;
  esp_now_add_peer(&peerInfo);
}

void loop() {
  server.handleClient();
  processModbusActions();

  if (!isTaskExecuting && !isManualTripActive && !taskWriteInProgress && queueSize > 0 && startInterlocksClear()) {
    if (!waitForCompletionPulse || completionPulseSeen) {
      int from = taskQueue[0].fromTrack;
      int to = taskQueue[0].toTrack;
      beginTaskWrites(from, to, 1);
      if (taskWriteInProgress) {
        for (int i = 0; i < 5; ++i) if (physicalTracks[i] == from) { trackStatuses[i] = 2; break; }
        waitForCompletionPulse = false;
        completionPulseSeen = false;
      }
    }
  }

  
}
