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
        button:disabled { background: #222 !important; color: #555 !important; border-color: #333 !important; cursor: not-allowed !important; }
        .st-0 { color: #888; } 
.st-1 { color: #ffcc00; } /* Загружена — оранжевый */
.st-2 { color: #00ff66; } /* В пути — зеленый */
.st-3 { color: #00f0ff; } /* В очереди — голубой/синий */


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
                let plcBusy = (data.is_busy === 1);

                // Блокировка/разблокировка элементов управления при занятом ПЛК
                let autoActive = (data.queue.length > 0); 
                document.getElementById('runManualBtn').disabled = plcBusy || autoActive;
                document.getElementById('saveRoutesBtn').disabled = plcBusy;
                for(let i = 1; i <= 5; i++) {
                    document.getElementById('route_' + i).disabled = plcBusy;
                }
                document.getElementById('man_from').disabled = plcBusy;
                document.getElementById('man_to').disabled = plcBusy;
                document.getElementById('man_rollers').disabled = plcBusy;

                // Обновление статусов дорожек
                for(let i = 1; i <= 5; i++) {
                    let cell = document.getElementById('node_st_' + i);
                    let statusValue = data.statuses[i-1];
                    cell.innerText = statusValue;
                    cell.className = 'status-cell st-' + statusValue;
                }

                let queueContainer = document.getElementById('queueList');
                queueContainer.innerHTML = '';

                // Если ПЛК занят — перебиваем вывод предупреждением
                if (plcBusy) {
                     for(let i = 1; i <= 5; i++) {
    let cell = document.getElementById('node_st_' + i);
    let statusValue = data.statuses[i-1];
    cell.innerText = statusValue;
    cell.className = 'status-cell st-' + statusValue;
}

let queueContainer = document.getElementById('queueList');
queueContainer.innerHTML = '';

// ======= И ВОТ СЮДА ПЕРЕНЕСТИ ПРОВЕРКУ ЗАНЯТОСТИ =======
if (plcBusy) {
    queueContainer.innerHTML = '<div class="task-row" style="border-color: #ffaa00; background: #2b2005;"><div class="task-text" style="color: #ffaa00;">КОНВЕЙЕР ЗАНЯТ / РУЧНОЙ РЕЖИМ ПЛК</div></div>';
    return;
}
                    return;
                }

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
