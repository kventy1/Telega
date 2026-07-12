#include "PlcProto.h"

static PlcReceiver* receiverInstance = nullptr;

void IRAM_ATTR plcReceiverISRBridge() {
    if (receiverInstance != nullptr) {
        receiverInstance->isrHandler();
    }
}

PlcReceiver::PlcReceiver(uint8_t pin) {
    _pin = pin;
    _pulse_count = 0;       // Здесь мы будем считать количество полезных переключений (CHANGE)
    _last_pulse_time = 0;
    _digit_index = 0;
    _new_data_flag = false;
    _out_integer = 0;
    _out_fraction = 0;
    
    _last_isr_time_us = 0;  // Переменная микросекунд
    memset(_received_digits, 0, sizeof(_received_digits));
}

void PlcReceiver::begin() {
    receiverInstance = this;
    pinMode(_pin, INPUT_PULLUP);
    // а) Жестко ловим и спады, и падения через CHANGE
    attachInterrupt(digitalPinToInterrupt(_pin), plcReceiverISRBridge, CHANGE);
}

void IRAM_ATTR PlcReceiver::isrHandler() {
    uint64_t now_us = esp_timer_get_time();
    uint32_t diff_ms = (uint32_t)((now_us - _last_isr_time_us) / 1000);

    // б) МУСОР И ШУМ: Всё, что короче 20 мс — нафиг, вообще игнорируем
    if (diff_ms < 20) return;

    _last_isr_time_us = now_us; // Запоминаем время только ЧИСТОГО переключения
    _last_pulse_time = millis(); // Для контроля тишины в функции update()

    // в) ПОЛЕЗНЫЙ СИГНАЛ: Все перепады с разницей меньше 100 мс — это импульсы и паузы цифры
    if (diff_ms < 100) {
        _pulse_count++;
    }
}

char PlcReceiver::decodePulses(uint32_t changes) {
    // Так как мы считаем переключения (и LOW, и HIGH), переворачиваем их обратно в цифру:
    // (Количество переключений + 1) делить на 2.
    int digit = (changes + 1) / 2;

    // В коде ПЛК ноль — это 10 импульсов (то есть 19 переключений)
    if (digit == 10 || changes == 19) digit = 0;
    
    if (digit > 9 || digit < 0) return 'x'; 
    return '0' + digit;
}

void PlcReceiver::resetBuffer() {
    _digit_index = 0;
    memset(_received_digits, 0, sizeof(_received_digits));
    noInterrupts();
    _pulse_count = 0;
    interrupts();
}

void PlcReceiver::update() {
    unsigned long now = millis();
    unsigned long silence = now - (_last_isr_time_us / 1000);

    // ГЛОБАЛЬНЫЙ КОНЕЦ ПОСЫЛКИ: Если тишина затянулась дольше 1 секунды
    if (silence > 1000) {
        _digit_index = 0;
        _pulse_count = 0;
    }

    // г) КОНЕЦ ЦИФРЫ: Всё, что длиннее 150 мс тишины — закрываем прием цифры
    if (_pulse_count > 0 && silence >= 150) {
        
        noInterrupts();
        uint32_t changes = _pulse_count;
        _pulse_count = 0; // Мгновенно обнуляем счетчик для следующей цифры пакета
        interrupts();

        char c = decodePulses(changes);
        
        if (c != 'x' && _digit_index < 3) {
            _received_digits[_digit_index] = c;
            _digit_index++;

            // МГНОВЕННАЯ УСКОРЕННАЯ СБОРКА: Накопилось 3 цифры — выдаем наружу
            if (_digit_index == 3) {
                int d1 = _received_digits[0] - '0';
                int d2 = _received_digits[1] - '0';
                int d3 = _received_digits[2] - '0';

                _out_integer = (d1 * 10) + d2; 
                _out_fraction = d3;            
                _new_data_flag = true; // Выставляем available() = true для loop
                
                _digit_index = 99; // Замок до конца пакета (пока не наступит секундная тишина)
                memset(_received_digits, 0, sizeof(_received_digits));
            }
        }
    }
}

bool PlcReceiver::available() { return _new_data_flag; }
int PlcReceiver::getInteger() { _new_data_flag = false; return _out_integer; }
int PlcReceiver::getFraction() { return _out_fraction; }


// --- РЕАЛИЗАЦИЯ ОТПРАВКИ (RMT ОСТАЕТСЯ ПРЕЖНИМ) ---
PlcSender::PlcSender(uint8_t pin, rmt_channel_t channel) {
    _pin = pin; _channel = channel; _initialized = false;
}
void PlcSender::begin() {
    rmt_config_t config = RMT_DEFAULT_CONFIG_TX((gpio_num_t)_pin, _channel);
    config.clk_div = 80; config.tx_config.loop_en = false; config.tx_config.idle_output_en = true; config.tx_config.idle_level = RMT_IDLE_LEVEL_LOW;
    rmt_config(&config); rmt_driver_install(_channel, 0, 0);
    for (int i = 0; i < MAX_PULSES; i++) { _pulseItems[i].level0 = 1; _pulseItems[i].duration0 = 50; _pulseItems[i].level1 = 0; _pulseItems[i].duration1 = 100; }
    _initialized = true;
}
void PlcSender::sendPulseBlock(uint32_t pulseCount) {
    if (!_initialized || pulseCount == 0) return;
    if (pulseCount > MAX_PULSES) pulseCount = MAX_PULSES;
    rmt_write_items(_channel, _pulseItems, pulseCount, true);
}
void PlcSender::sendString(String val) {
    if (val.length() != 8) return;
    for (int i = 0; i < 8; i++) { int digit = val.charAt(i) - '0'; long pulseCount = (digit * 100) + 50; sendPulseBlock((uint32_t)pulseCount); delay(35); }
}
