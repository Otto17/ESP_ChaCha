/*
 * ESP_ChaCha — пример управления ключами
 *
 * Демонстрирует:
 * - useKey() — задать ключ вручную в коде
 * - generateKey() — автогенерация и сохранение в LittleFS
 * - generateKey(true)— принудительная перегенерация ключа
 * - rawKey() — доступ к сырым байтам ключа
 * - keyAsHexString() — ключ в виде строки для вывода
 * - generateQR() — QR-код в LittleFS
 * - resetStats() — сброс статистики
 *
 * Веб-страница: http://<IP>/
 * - показывает текущий ключ
 * - кнопка "Копировать ключ"
 * - QR-код ключа (SVG из LittleFS)
 * - кнопка "Сгенерировать новый ключ"
 */

#ifdef ESP8266
#include <ESPAsyncTCP.h>
#include <Hash.h>
#else
#include <AsyncTCP.h>
#endif
#include <ESP_ChaCha.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

// НАСТРОЙКИ
const char* WIFI_SSID = "YourSSID";
const char* WIFI_PASS = "YourPassword";
const uint16_t CHACHA_PORT = 8888;
const uint16_t WEB_PORT = 80;

// Пример ручного ключа (Опция A)
// Раскомментируйте useKey(MY_KEY) в setup() для использования
const uint8_t MY_KEY[32] = {
  0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
  0x09, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16,
  0x17, 0x18, 0x19, 0x20, 0x21, 0x22, 0x23, 0x24,
  0x25, 0x26, 0x27, 0x28, 0x29, 0x30, 0x31, 0x32
};

ESP_ChaCha chacha;
AsyncWebServer webServer(WEB_PORT);

// Флаги отложенных задач.
// Устанавливаются в обработчиках HTTP-запросов (системный стек), выполняются в loop() (пользовательский стек, без риска краша watchdog)
static volatile bool pendingRegenKey = false;

// HTML СТРАНИЦА
// Хранится в PROGMEM чтобы не занимать RAM
static const char HTML_PAGE[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="ru">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ESP ChaCha — Управление ключом</title>
  <style>
    body      { font-family: Arial, sans-serif; max-width: 600px; margin: 40px auto;
                padding: 0 20px; background: #f5f5f5; }
    h1        { color: #333; }
    .card     { background: white; border-radius: 8px; padding: 20px; margin: 16px 0;
                box-shadow: 0 2px 6px rgba(0,0,0,0.1); }
    .key-text { font-family: monospace; font-size: 12px; word-break: break-all;
                background: #f0f0f0; padding: 10px; border-radius: 4px; }
    button    { background: #4CAF50; color: white; border: none; padding: 10px 20px;
                border-radius: 4px; cursor: pointer; font-size: 14px; margin: 4px; }
    button:hover         { background: #45a049; }
    button.danger        { background: #e53935; }
    button.danger:hover  { background: #c62828; }
    button.copy          { background: #1976D2; }
    button.copy:hover    { background: #1565C0; }
    .qr-container        { text-align: center; margin: 10px 0; }
    .qr-container img    { max-width: 220px; }
    .stats   { font-size: 13px; color: #666; }
    .ok      { color: #4CAF50; font-weight: bold; }
    #msg     { min-height: 20px; color: #1976D2; font-size: 13px; margin-top: 8px; }
  </style>
</head>
<body>
<h1>&#128274; ESP ChaCha</h1>

<div class="card">
  <h2>Текущий ключ</h2>
  <div class="key-text" id="keyText">Загрузка...</div>
  <br>
  <button class="copy" onclick="copyKey()">&#128203; Копировать ключ</button>
  <div id="msg"></div>
</div>

<div class="card">
  <h2>QR-код ключа</h2>

  <p style="font-size:13px;color:#666;line-height:1.5;">
    QR-код содержит 32-байтовый ключ ChaCha20 в HEX формате.<br>
    Внутри QR: 64 HEX-символа (0–9, A–F).
  </p>

  <div class="qr-container">
    <img src="/ChaChaQR.svg" alt="QR-код ключа" id="qrImg">
  </div>

  <hr style="margin:18px 0;border:none;border-top:1px solid #eee;">

  <div style="font-size:18px;font-weight:bold;text-align:center;">
    Декодирование в других языках
  </div><br>

  <div style="font-size:13px;font-weight:bold;">Kotlin</div>
  <div class="key-text">
    val key = ByteArray(32)<br>
    for (i in 0 until 32) {<br>
    &nbsp;&nbsp;key[i] = hex.substring(i*2, i*2+2).toInt(16).toByte()<br>
    }
  </div><br>

  <div style="font-size:13px;font-weight:bold;">Go</div>
  <div class="key-text">key, err := hex.DecodeString(qrText)</div><br>

  <div style="font-size:13px;font-weight:bold;">C#</div>
  <div class="key-text">byte[] key = Convert.FromHexString(qrText);</div><br>

  <div style="font-size:13px;font-weight:bold;">Python</div>
  <div class="key-text">key = bytes.fromhex(qr_text)</div>
</div>

<div class="card">
  <h2>Управление</h2>
  <button class="danger" onclick="regenKey()">&#9888; Сгенерировать новый ключ</button>
  <p style="font-size:12px;color:#e53935;">
    Внимание: после смены ключа все клиенты должны использовать новый ключ!
  </p>
</div>

<div class="card">
  <h2>Статистика</h2>
  <div class="stats" id="statsDiv">Загрузка...</div>
</div>

<script>
function showMsg(text, isError) {
  var el = document.getElementById('msg');
  el.style.color = isError ? '#e53935' : '#1976D2';
  el.textContent = text;
  setTimeout(function(){ el.textContent = ''; }, 3000);
}

function loadKey() {
  fetch('/api/key')
    .then(function(r){ return r.json(); })
    .then(function(d){
      document.getElementById('keyText').textContent = d.key;
    });
}

function copyKey() {
  var text = document.getElementById('keyText').textContent;
  if (navigator.clipboard) {
    navigator.clipboard.writeText(text)
      .then(function(){ showMsg('Ключ скопирован!', false); })
      .catch(function(){ fallbackCopy(text); });
  } else {
    fallbackCopy(text);
  }
}

function fallbackCopy(text) {
  var ta = document.createElement('textarea');
  ta.value = text;
  document.body.appendChild(ta);
  ta.select();
  document.execCommand('copy');
  document.body.removeChild(ta);
  showMsg('Ключ скопирован!', false);
}

// Опрашиваем ESP до тех пор, пока QR-файл не будет готов,
// затем перезагружаем картинку и обновляем текст ключа.
function pollUntilReady() {
  fetch('/api/status')
    .then(function(r){ return r.json(); })
    .then(function(d){
      if (d.ready) {
        // QR готов — перезагружаем картинку (timestamp сбивает кэш браузера)
        document.getElementById('qrImg').src = '/ChaChaQR.svg?' + Date.now();
        loadKey();
        showMsg('Новый ключ сгенерирован!', false);
      } else {
        // Ещё не готов — повторяем через 500 мс
        setTimeout(pollUntilReady, 500);
      }
    })
    .catch(function(){
      setTimeout(pollUntilReady, 500);
    });
}

function regenKey() {
  if (!confirm('Сгенерировать новый ключ? Текущий ключ будет утерян!')) return;
  showMsg('Генерация ключа…', false);
  fetch('/api/regenkey', {method:'POST'})
    .then(function(r){ return r.json(); })
    .then(function(d){
      if (d.accepted) {
        // Запрос принят, ждём завершения в loop()
        setTimeout(pollUntilReady, 800);
      } else {
        showMsg('Ошибка!', true);
      }
    });
}

function loadStats() {
  fetch('/api/stats')
    .then(function(r){ return r.json(); })
    .then(function(d){
      document.getElementById('statsDiv').innerHTML =
        'Принято пакетов: <span class="ok">' + d.rx     + '</span> &nbsp;|&nbsp; ' +
        'Успешно: <span class="ok">'          + d.ok     + '</span> &nbsp;|&nbsp; ' +
        'Ошибок тега: '                       + d.badTag + ' &nbsp;|&nbsp; '        +
        'Отправлено: <span class="ok">'       + d.tx     + '</span> &nbsp;|&nbsp; ' +
        'Клиентов: <span class="ok">'         + d.clients+ '</span>';
    });
}

// При загрузке страницы
loadKey();
loadStats();
setInterval(loadStats, 5000); // обновляем статистику каждые 5 сек
</script>
</body>
</html>
)rawhtml";

// КОЛБЭКИ
void onMessage(const char* jsonStr, uint8_t clientIdx) {
  Serial.print(F("[MSG] #"));
  Serial.print(clientIdx);
  Serial.print(F(": "));
  Serial.println(jsonStr);

  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, jsonStr) != DeserializationError::Ok) return;

  const char* cmd = doc["cmd"];
  if (!cmd) return;

  if (strcmp(cmd, "ping") == 0) {
    chacha.broadcast("{\"res\":\"pong\"}");
  } else {
    chacha.sendTo(clientIdx, "{\"res\":\"unknown_cmd\"}");
  }
}

void onClient(uint8_t clientIdx, bool connected) {
  Serial.print(F("[CLIENT] Слот #"));
  Serial.print(clientIdx);
  Serial.println(connected ? F(" подключён") : F(" отключён"));

  if (connected) {
    char welcome[220];
    snprintf(welcome, sizeof(welcome),
             "{\"event\":\"welcome\",\"key\":\"%s\"}",
             chacha.keyAsHexString());
    chacha.sendTo(clientIdx, welcome);
  }
}

// WEB-СЕРВЕР
void setupWebServer() {

  // Главная страница
  webServer.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send_P(200, "text/html", HTML_PAGE);
  });

  // SVG QR-код из LittleFS
  webServer.on("/ChaChaQR.svg", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(LittleFS, CHACHA_QR_FILE, "image/svg+xml");
  });

  // API: получить ключ
  webServer.on("/api/key", HTTP_GET, [](AsyncWebServerRequest* req) {
    StaticJsonDocument<256> doc;
    doc["key"] = chacha.keyAsHexString();
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  // API: статус готовности (используется JS-поллингом после regenKey)
  webServer.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* req) {
    StaticJsonDocument<32> doc;
    // pendingRegenKey == false означает, что loop() уже завершил задачу
    doc["ready"] = !pendingRegenKey;
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  // API: запросить генерацию нового ключа.
  // Только ставит флаг — тяжёлая работа выполняется в loop()
  // Мгновенно возвращает {"accepted":true}, не блокируя системный стек
  webServer.on("/api/regenkey", HTTP_POST, [](AsyncWebServerRequest* req) {
    pendingRegenKey = true;  // задача принята
    StaticJsonDocument<32> resp;
    resp["accepted"] = true;
    String out;
    serializeJson(resp, out);
    req->send(200, "application/json", out);
  });

  // API: статистика
  webServer.on("/api/stats", HTTP_GET, [](AsyncWebServerRequest* req) {
    const auto& s = chacha.stats();
    StaticJsonDocument<128> doc;
    doc["rx"] = s.rxPackets;
    doc["ok"] = s.rxOk;
    doc["badLen"] = s.rxBadLen;
    doc["badTag"] = s.rxBadTag;
    doc["tx"] = s.txPackets;
    doc["clients"] = chacha.connectedCount();
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  webServer.onNotFound([](AsyncWebServerRequest* req) {
    req->send(404, "text/plain", "Not found");
  });

  webServer.begin();
  Serial.println(F("[WEB] Сервер запущен"));
}

void setup() {
  Serial.begin(115200);
  delay(3000);
  Serial.println(F("\n[KeyManagement] Старт"));

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print(F("Wi-Fi"));
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
  }
  Serial.print(F("\nIP: "));
  Serial.println(WiFi.localIP());

  // ВЫБОР СПОСОБА ЗАДАНИЯ КЛЮЧА

  // Опция A: ручной ключ (не сохраняется в LittleFS)
  // chacha.useKey(MY_KEY);

  // Опция B: загрузить из LittleFS или сгенерировать новый
  chacha.generateKey(false);

  // Опция C: всегда генерировать новый при старте
  // chacha.generateKey(true);

  chacha.onMessage(onMessage);
  chacha.onClient(onClient);
  chacha.begin(CHACHA_PORT);
  chacha.resetStats();

  // Генерируем QR при старте (выполняется здесь безопасно — это setup(), не колбэк)
  chacha.generateQR();

  setupWebServer();

  Serial.print(F("Heap: "));
  Serial.println(ESP.getFreeHeap());
  Serial.print(F("Ключ: "));
  Serial.println(chacha.keyAsHexString());
  Serial.printf("Веб: http://%s/\n", WiFi.localIP().toString().c_str());
  Serial.printf("TCP: %s:%d\n", WiFi.localIP().toString().c_str(), CHACHA_PORT);
}

void loop() {
  chacha.loop();

  // ОТЛОЖЕННАЯ ЗАДАЧА: генерация ключа + QR
  if (pendingRegenKey) {
    pendingRegenKey = false;  // Сбрасываем флаг ДО работы, чтобы /api/status не вернул ready=true раньше времени
    bool ok = chacha.generateKey(true);
    if (ok) {
      chacha.generateQR();

      // Уведомляем всех TCP-клиентов о смене ключа
      char notify[220];
      snprintf(notify, sizeof(notify),
               "{\"event\":\"key_changed\",\"key\":\"%s\"}",
               chacha.keyAsHexString());
      chacha.broadcast(notify);

      Serial.print(F("[TASK] Новый ключ: "));
      Serial.println(chacha.keyAsHexString());
    } else {
      Serial.println(F("[TASK] ERROR: generateKey вернул false"));
    }
    pendingRegenKey = false;  // Снова false — теперь /api/status вернёт ready=true
  }

  static uint32_t lastStats = 0;
  if (millis() - lastStats >= 30000) {
    lastStats = millis();
    const auto& s = chacha.stats();
    Serial.printf("[STATS] rx:%u ok:%u badLen:%u badTag:%u tx:%u clients:%u\n",
                  s.rxPackets, s.rxOk, s.rxBadLen, s.rxBadTag,
                  s.txPackets, chacha.connectedCount());
  }
}
