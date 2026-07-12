#include <WiFi.h>
#include <WebServer.h>
#include <esp_wifi.h>  
#include <driver/rmt.h>
#include <esp_now.h>
void handleIncomingTrack(int track, int whistle); 
// ========== НАСТРОЙКИ ЖЕЛЕЗА ==========
#define OUT_PIN 33
#define BLOCK_PIN 27 // Пин блокировки/подтверждения от ПЛК (LOW = пауза)

// Настройки WiFi
const char* ssid = "Telega-Control";
const char* password = "02345678";
WebServer server(80);

char rx_node_buf[16];                // Буфер для сборки строки
uint8_t broadcastMac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; // Адрес для стрельбы во все ноды


// ========== RMT НАСТРОЙКИ ==========
#define RMT_CHANNEL RMT_CHANNEL_0
#define RMT_CLK_DIV 80  // 1 тик = 1 мкс
#define MAX_PULSES 1000
rmt_item32_t pulseItems[MAX_PULSES];
bool rmtInitialized = false;

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
String trackRoutes[] = {"08", "12", "15", "02", "09"};
const int physicalTracks[] = {8, 12, 15, 2, 9};
int trackStatuses[] = {0, 0, 0, 0, 0};

bool isTaskExecuting = false;

// Для отображения ручного рейса на экране
bool isManualTripActive = false;
int manualFrom = 0;
int manualTo = 0;

// Сюда мы вставим HTML-код во втором сообщении
extern const char* html_page; 

// ========== ИНИЦИАЛИЗАЦИЯ ЖЕЛЕЗА ==========
void setupRMT() {
  rmt_config_t config = RMT_DEFAULT_CONFIG_TX((gpio_num_t)OUT_PIN, RMT_CHANNEL);
  config.clk_div = RMT_CLK_DIV;
  config.tx_config.loop_en = false;
  config.tx_config.idle_output_en = true;
  config.tx_config.idle_level = RMT_IDLE_LEVEL_LOW;
  
  rmt_config(&config);
  rmt_driver_install(RMT_CHANNEL, 0, 0);
  
  for (int i = 0; i < MAX_PULSES; i++) {
    pulseItems[i].level0 = 1;
    pulseItems[i].duration0 = 50;   
    pulseItems[i].level1 = 0;
    pulseItems[i].duration1 = 100;  
  }
  rmtInitialized = true;
}

void sendPulseBlock(uint32_t pulseCount) {
  if (!rmtInitialized || pulseCount == 0) return;
  if (pulseCount > MAX_PULSES) pulseCount = MAX_PULSES;
  rmt_write_items(RMT_CHANNEL, pulseItems, pulseCount, true);
}

// Базовая функция выдачи 8 цифр в линию ПЛК (Ваш исходный алгоритм)
void sendSequenceToPLC(int from, int to, int digit5, int digit6) {
  char val[9];
  snprintf(val, sizeof(val), "%02d%02d%d%d00", from, to, digit5, digit6);

  for (int i = 0; i < 8; i++) {
    int digit = val[i] - '0';
    long pulseCount = (digit * 100) + 50;
    sendPulseBlock((uint32_t)pulseCount);
    delay(35);
  }
}

void shiftQueue() {
  if (queueSize <= 0) return;
  for (int i = 0; i < queueSize - 1; i++) {
    taskQueue[i] = taskQueue[i + 1];
  }
  queueSize--;
  isTaskExecuting = false;
}

// ========== ОБРАБОТЧИК РАДИО (ESP-NOW) ==========
void OnDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len) {
  if (len >= 3) {
    // Безопасно копируем данные в текстовый буфер
    int copy_len = (len > 15) ? 15 : len;
    memcpy(rx_node_buf, incomingData, copy_len);
    rx_node_buf[copy_len] = '\0';
    
    String msg = String(rx_node_buf);
    
    // Если по воздуху прилетел маркер ноды "A12"
    if (msg.startsWith("A")) {
      String trackStr = msg.substring(1);
      int parsedTrack = trackStr.toInt(); // Получили число 12
      
      if (parsedTrack > 0) {
        // Вызываем РОДНУЮ функцию вашей прошивки!
        // Она сама обновит статусы, поставит в очередь конвейера 
        // и правильно отдаст данные на веб-страницу Центра при AJAX-запросе.
        handleIncomingTrack(parsedTrack, 1); 
      }
    }
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
    
    sendSequenceToPLC(manualFrom, manualTo, d5, d6);
    
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
  
  json += "\"is_paused\":" + String((digitalRead(27) == LOW) ? 1 : 0) + ",";
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
    sendSequenceToPLC(0, 0, 0, 9); 
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
  sendSequenceToPLC(0, 0, 0, 9); 
  server.send(200, "text/plain", "OK");
}

void handleGlobalClear() {
  sendSequenceToPLC(0, 0, 0, 9); 
  for (int i = 0; i < 5; i++) trackStatuses[i] = 0; 
  queueSize = 0; 
  isTaskExecuting = false;
  isManualTripActive = false;
  server.send(200, "text/plain", "OK");
}

// ========== SETUP & LOOP ==========
void setup() {
  pinMode(OUT_PIN, OUTPUT);
  digitalWrite(OUT_PIN, LOW);
  pinMode(BLOCK_PIN, INPUT_PULLUP);
  
  setupRMT();

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
    esp_now_peer_info_t peerInfo;
  memset(&peerInfo, 0, sizeof(peerInfo));
  memcpy(peerInfo.peer_addr, broadcastMac, 6);
  peerInfo.channel = 1;  // Ставим 1 канал, так как вся тележка жестко зафиксирована на 1 канале!
  peerInfo.encrypt = false;
  esp_now_add_peer(&peerInfo);
}

  // ========== КОД СТРАНИЦЫ ИНТЕРФЕЙСА ==========
const char* html_page = R"rawliteral(
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
        input[type="text"] { width: 100%; }
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
        .manual-inputs input[type="text"] { width: 50px; }
        .manual-inputs select { width: 70px; }
    </style>
</head>
<body>

    <div class="section-title">НАСТРОЙКА МАРШРУТОВ И РУЧНОЙ РЕЙС</div>
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
                    <td>Дорожка 1</td>
                    <td class="status-cell st-0" id="node_st_1">0</td>
                    <td><input type="text" id="route_1" value="08"></td>
                </tr>
                <tr>
                    <td>Дорожка 2</td>
                    <td class="status-cell st-0" id="node_st_2">0</td>
                    <td><input type="text" id="route_2" value="12"></td>
                </tr>
                <tr>
                    <td>Дорожка 3</td>
                    <td class="status-cell st-0" id="node_st_3">0</td>
                    <td><input type="text" id="route_3" value="15"></td>
                </tr>
                <tr>
                    <td>Дорожка 4</td>
                    <td class="status-cell st-0" id="node_st_4">0</td>
                    <td><input type="text" id="route_4" value="02"></td>
                </tr>
                <tr>
                    <td>Дорожка 5</td>
                    <td class="status-cell st-0" id="node_st_5">0</td>
                    <td><input type="text" id="route_5" value="09"></td>
                </tr>
                <!-- ШЕСТАЯ СТРОКА: РУЧНОЙ РЕЙС -->
                <tr style="background: #1a2925;">
                    <td style="font-weight: bold; color: #00ffaa;">6. РЕЙС</td>
                    <td><button class="manual-btn" id="runManualBtn">ПУСК</button></td>
                    <td>
                        <div class="manual-inputs">
                            <span>Откуда:</span>
                            <input type="text" id="man_from" value="03" maxlength="2">
                            <span>Куда:</span>
                            <input type="text" id="man_to" value="15" maxlength="2">
                            <span>Ролики:</span>
                            <select id="man_rollers">
                                <option value="1">1</option>
                                <option value="2">2</option>
                            </select>
                        </div>
                    </td>
                </tr>
            </tbody>
        </table>
    </div>
    <button id="saveRoutesBtn">ОБНОВИТЬ МАРШРУТЫ АВТОМАТИКИ</button>

    <div class="section-title" style="margin-top: 20px;">ОЧЕРЕДЬ ВЫПОЛНЕНИЯ ЗАДАЧ</div>
    <div class="queue-container" id="queueList"></div>

    <div class="bottom-actions">
        <button class="btn-danger" id="stopAllBtn">ОБЩАЯ ПАУЗА</button>
        <button class="btn-danger" id="clearAllBtn">СБРОСИТЬ ВСЁ</button>
    </div>

<script>
    // 1. Отправка таблицы маршрутов
    document.getElementById('saveRoutesBtn').onclick = function() {
        let payload = `${document.getElementById('route_1').value}|${document.getElementById('route_2').value}|${document.getElementById('route_3').value}|${document.getElementById('route_4').value}|${document.getElementById('route_5').value}`;
        fetch('/set_routes?val=' + encodeURIComponent(payload))
            .then(res => { this.style.borderColor = '#00ff66'; setTimeout(() => this.style.borderColor='#555', 1000); })
            .catch(() => { this.style.borderColor = '#ff3300'; setTimeout(() => this.style.borderColor='#555', 1000); });
    };

    // 2. ОТПРАВКА ШЕСТОЙ СТРОКИ (РУЧНОЙ РЕЙС)
    document.getElementById('runManualBtn').onclick = function() {
        let from = document.getElementById('man_from').value;
        let to = document.getElementById('man_to').value;
        let rollers = document.getElementById('man_rollers').value;
        
        fetch(`/run_manual_trip?from=${from}&to=${to}&rollers=${rollers}`)
            .then(res => console.log('Ручной рейс отправлен'))
            .catch(() => console.log('Ошибка пуска'));
    };

    function cancelActiveTask() {
        fetch('/cancel_task').catch(() => {});
    }

    document.getElementById('stopAllBtn').onclick = function() { fetch('/global_stop'); };
    document.getElementById('clearAllBtn').onclick = function() { fetch('/global_clear'); };

    // 3. Фоновый опрос
    function updateDashboard() {
        fetch('/get_status_data')
            .then(res => res.json())
            .then(data => {
                for(let i = 1; i <= 5; i++) {
                    let cell = document.getElementById('node_st_' + i);
                    let statusValue = data.statuses[i-1];
                    cell.innerText = statusValue;
                    cell.className = 'status-cell st-' + statusValue;
                }

                let queueContainer = document.getElementById('queueList');
                queueContainer.innerHTML = '';

                // Если запущен РУЧНОЙ РЕЙС (6-я строка)
                if (data.manual_active === 1) {
                    let row = document.createElement('div');
                    row.className = 'task-row active';
                    if (data.is_paused === 1) row.classList.add('paused');
                    
                                       row.innerHTML = `
                        <div class="cancel-btn-cell"><button class="cancel-btn" onclick="cancelActiveTask()">ОТМЕНА</button></div>
                        <div class="task-text" style="color: #00ffaa;">[РУЧНОЙ] ${data.manual_from} &rarr; ${data.manual_to}</div>
                        <div class="task-status">${data.is_paused === 1 ? 'ПАУЗА ПЛК' : 'В ПУТИ'}</div>
                    `;
                    queueContainer.appendChild(row);
                    return;
                }

                // Иначе выводим стандартную автоматическую очередь
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

    setInterval(updateDashboard, 1000);
</script>
</body>
</html>
)rawliteral";

  
  
// --- ФУНКЦИЯ ОТПРАВКИ МАРКЕРА АКТИВАЦИИ НА НОДУ ---
void sendCommandToNode(int trackNum) {
  String trackStr = String(trackNum);
  if (trackNum < 10) trackStr = "0" + trackStr; // Ведущий ноль для красоты
  
  String payload = "A" + trackStr; // Собрали строку "A12"
  
  int msgLen = payload.length();
  uint8_t dataBuf[16];
  payload.getBytes(dataBuf, msgLen + 1);

  // Пуляем в эфир! Наша нода поймает этот текст и включит выгрузку ПЛК
  esp_now_send(broadcastMac, dataBuf, msgLen);
}

void loop() {
  server.handleClient();
  
  // --- ЛОГИКА ФИЛЬТРА ИМПУЛЬСОВ НА ПИНЕ 27 (ФИНИШ ОТ ПЛК) ---
  bool currentPinState27 = digitalRead(BLOCK_PIN); 
  static bool lastPinState27 = HIGH;
  static int edgeCount = 0;
  static unsigned long windowStartTime = 0;
  bool taskFinishedSuccess = false;

  if (isTaskExecuting && currentPinState27 == LOW && lastPinState27 == HIGH) {
    if (edgeCount == 0) {
      windowStartTime = millis();
      edgeCount = 1;
    } 
    else if (edgeCount == 1 && (millis() - windowStartTime <= 1200)) {
      edgeCount = 0; 
      taskFinishedSuccess = true; 
    }
  }
  lastPinState27 = currentPinState27;

  if (edgeCount == 1 && (millis() - windowStartTime > 1200)) {
    edgeCount = 0; 
  }

  // --- ОБРАБОТКА УСПЕШНОГО ФИНИША ---
  if (isTaskExecuting && taskFinishedSuccess) {
    if (isManualTripActive) {
      isManualTripActive = false;
      isTaskExecuting = false;
    } else if (queueSize > 0) {
      for (int i = 0; i < 5; i++) {
        if (physicalTracks[i] == taskQueue[0].fromTrack) { trackStatuses[i] = 0; break; }
      }
      shiftQueue(); 
    }
  }

  // --- АВТОМАТИЧЕСКАЯ ОБРАБОТКА ОЧЕРЕДИ ЗАДАЧ (СТАРТ) ---
  if (!isManualTripActive && queueSize > 0 && !isTaskExecuting && digitalRead(BLOCK_PIN) != LOW) {
    edgeCount = 0; 
    for (int i = 0; i < 5; i++) {
      if (physicalTracks[i] == taskQueue[0].fromTrack) { trackStatuses[i] = 2; break; }
    }
    sendSequenceToPLC(taskQueue[0].fromTrack, taskQueue[0].toTrack, 1, 4);
    isTaskExecuting = true; 
  }

  static unsigned long lastApCheck = 0;
  if (millis() - lastApCheck > 15000) {
    lastApCheck = millis();
    if (WiFi.softAPgetStationNum() == 0 && WiFi.softAPIP().toString() == "0.0.0.0") {
      WiFi.softAP(ssid, password);
    }
  }
}
