/*
 * ESP_ChaCha — пример сервера датчиков
 *
 * Демонстрирует:
 *   - broadcast() — рассылка данных датчиков всем клиентам
 *   - sendTo()    — ответ конкретному клиенту
 *   - isConnected() / connectedCount() — проверка состояния клиентов
 *   - stats() — статистика пакетов
 *   - Веб-страница с QR и ключом
 *
 * Симулирует 3 датчика: температура, влажность, давление.
 * Реальные датчики подключаются вместо симуляции.
 *
 * Команды от клиентов:
 *   {"cmd":"ping"}                        → {"res":"pong"}
 *   {"cmd":"getSensors"}                  → текущие показания всем
 *   {"cmd":"setInterval","ms":1000}       → сменить интервал отправки
 *   {"cmd":"getStats"}                    → статистика пакетов
 *   {"cmd":"resetStats"}                  → сброс статистики
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

uint32_t sensorIntervalMs = 2000;  // Интервал отправки показаний (меняется клиентом)

ESP_ChaCha chacha;
AsyncWebServer webServer(WEB_PORT);

// СИМУЛЯЦИЯ ДАТЧИКОВ
struct SensorData {
  float temperature;  // °C
  float humidity;     // %
  float pressure;     // hPa
  float voltage;      // V (АЦП питания)
} sensors;

void readSensors() {
  // Симуляция: плавное изменение значений
  static float t = 23.5f, h = 55.0f, p = 1013.0f;
  t += ((float)(random(-10, 11))) * 0.05f;
  h += ((float)(random(-10, 11))) * 0.1f;
  p += ((float)(random(-5, 6))) * 0.1f;

  t = constrain(t, 15.0f, 35.0f);
  h = constrain(h, 30.0f, 90.0f);
  p = constrain(p, 990.0f, 1040.0f);

  sensors.temperature = t;
  sensors.humidity = h;
  sensors.pressure = p;

#ifdef ESP8266
  sensors.voltage = (float)ESP.getVcc() / 1000.0f;
#else
  sensors.voltage = 3.3f;
#endif
}

// Формируем JSON с показаниями датчиков
void buildSensorJson(char* buf, size_t bufSize) {
  snprintf(buf, bufSize,
           "{\"type\":\"sensors\","
           "\"temp\":%.1f,"
           "\"hum\":%.1f,"
           "\"pres\":%.1f,"
           "\"vcc\":%.2f,"
           "\"uptime\":%lu,"
           "\"heap\":%u,"
           "\"interval\":%u}",
           sensors.temperature,
           sensors.humidity,
           sensors.pressure,
           sensors.voltage,
           millis() / 1000UL,
           (unsigned)ESP.getFreeHeap(),
           (unsigned)sensorIntervalMs);
}

// HTML СТРАНИЦА
// Хранится в PROGMEM чтобы не занимать RAM
static const char HTML_PAGE[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP Sensor Server</title>
<style>
  body { font-family: Arial, sans-serif; max-width: 640px; margin: 40px auto; padding: 0 20px; background:#f5f5f5; }
  h1 { color: #333; }
  .card { background:white; border-radius:8px; padding:20px; margin:16px 0; box-shadow:0 2px 6px rgba(0,0,0,0.1); }
  .sensor-grid { display:grid; grid-template-columns:1fr 1fr; gap:12px; }
  .sensor-box { background:#e8f5e9; border-radius:6px; padding:14px; text-align:center; }
  .sensor-box .val { font-size:28px; font-weight:bold; color:#2e7d32; }
  .sensor-box .lbl { font-size:12px; color:#666; margin-top:4px; }
  .key-text { font-family:monospace; font-size:11px; word-break:break-all;
              background:#f0f0f0; padding:10px; border-radius:4px; }
  button { background:#4CAF50; color:white; border:none; padding:8px 16px;
           border-radius:4px; cursor:pointer; font-size:13px; margin:3px; }
  button:hover { opacity:0.85; }
  .copy { background:#1976D2; }
  .qr-container { text-align:center; }
  .qr-container img { max-width:200px; }
  #status { font-size:12px; color:#888; margin-top:6px; }
</style>
</head>
<body>
<h1>&#127777; Сервер датчиков</h1>

<div class="card">
  <h2>Показания датчиков</h2>
  <div class="sensor-grid">
    <div class="sensor-box"><div class="val" id="temp">—</div><div class="lbl">Температура, °C</div></div>
    <div class="sensor-box"><div class="val" id="hum">—</div><div class="lbl">Влажность, %</div></div>
    <div class="sensor-box"><div class="val" id="pres">—</div><div class="lbl">Давление, hPa</div></div>
    <div class="sensor-box"><div class="val" id="vcc">—</div><div class="lbl">Питание, В</div></div>
  </div>
  <div id="status">Обновление...</div>
</div>

<div class="card">
  <h2>Ключ шифрования</h2>
  <div class="key-text" id="keyText">Загрузка...</div>
  <br>
  <button class="copy" onclick="copyKey()">&#128203; Копировать</button>
  <div id="msg" style="min-height:18px;font-size:12px;color:#1976D2;margin-top:6px;"></div>
</div>

<div class="card">
  <h2>QR-код ключа</h2>

  <p style="font-size:13px;color:#666;line-height:1.5;">
    QR-код содержит 32-байтовый ключ ChaCha20 в HEX формате.<br>
    Внутри QR: 64 HEX-символа (0-9 A-F).
  </p>

  <div class="key-text" style="margin-bottom:12px;">
    7333BDD51E4687C410F139FD74CD2943B455731691ABDB59691F73C0C99F1E98
  </div>

  <div class="qr-container">
    <img src="/ChaChaQR.svg" alt="QR" id="qr">
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
  </div>

  <br>

  <div style="font-size:13px;font-weight:bold;">Go</div>
  <div class="key-text">
    key, err := hex.DecodeString(qrText)
  </div>

  <br>

  <div style="font-size:13px;font-weight:bold;">C#</div>
  <div class="key-text">
    byte[] key = Convert.FromHexString(qrText);
  </div>

  <br>

  <div style="font-size:13px;font-weight:bold;">Python</div>
  <div class="key-text">
    key = bytes.fromhex(qr_text)
  </div>
</div>

<script>
function showMsg(t){ var e=document.getElementById('msg'); e.textContent=t; setTimeout(function(){e.textContent='';},2500); }

function loadSensors(){
  fetch('/api/sensors')
    .then(function(r){return r.json();})
    .then(function(d){
      document.getElementById('temp').textContent = d.temp.toFixed(1);
      document.getElementById('hum').textContent  = d.hum.toFixed(1);
      document.getElementById('pres').textContent = d.pres.toFixed(1);
      document.getElementById('vcc').textContent  = d.vcc.toFixed(2);
      document.getElementById('status').textContent =
        'Uptime: ' + d.uptime + 'с | Heap: ' + d.heap + ' байт | Интервал TCP: ' + d.interval + 'мс';
    });
}

function loadKey(){
  fetch('/api/key')
    .then(function(r){return r.json();})
    .then(function(d){ document.getElementById('keyText').textContent = d.key; });
}

function copyKey(){
  var t = document.getElementById('keyText').textContent;
  if(navigator.clipboard){ navigator.clipboard.writeText(t).then(function(){showMsg('Скопировано!');}); }
  else {
    var ta=document.createElement('textarea'); ta.value=t;
    document.body.appendChild(ta); ta.select(); document.execCommand('copy');
    document.body.removeChild(ta); showMsg('Скопировано!');
  }
}

loadKey();
loadSensors();
setInterval(loadSensors, 2000);
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

  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, jsonStr) != DeserializationError::Ok) return;

  const char* cmd = doc["cmd"];
  if (!cmd) return;

  if (strcmp(cmd, "ping") == 0) {
    chacha.broadcast("{\"res\":\"pong\"}");
  } else if (strcmp(cmd, "getSensors") == 0) {
    readSensors();
    char buf[200];
    buildSensorJson(buf, sizeof(buf));
    chacha.broadcast(buf);  // Все клиенты получают свежие данные
  } else if (strcmp(cmd, "setInterval") == 0) {
    uint32_t ms = doc["ms"] | 2000;
    ms = constrain(ms, 100, 60000);
    sensorIntervalMs = ms;

    char resp[64];
    snprintf(resp, sizeof(resp), "{\"event\":\"intervalChanged\",\"ms\":%u}", (unsigned)ms);
    chacha.broadcast(resp);  // Все клиенты узнают о смене интервала
    Serial.print(F("[CFG] Интервал: "));
    Serial.println(ms);
  } else if (strcmp(cmd, "getStats") == 0) {
    const auto& s = chacha.stats();
    char resp[128];
    snprintf(resp, sizeof(resp),
             "{\"type\":\"stats\",\"rx\":%u,\"ok\":%u,\"badLen\":%u,\"badTag\":%u,\"tx\":%u}",
             s.rxPackets, s.rxOk, s.rxBadLen, s.rxBadTag, s.txPackets);
    chacha.sendTo(clientIdx, resp);
  } else if (strcmp(cmd, "resetStats") == 0) {
    chacha.resetStats();
    chacha.sendTo(clientIdx, "{\"res\":\"stats_reset\"}");
  } else {
    chacha.sendTo(clientIdx, "{\"res\":\"unknown_cmd\"}");
  }
}

void onClient(uint8_t clientIdx, bool connected) {
  Serial.print(F("[CLIENT] #"));
  Serial.print(clientIdx);
  Serial.println(connected ? F(" подключён") : F(" отключён"));

  if (connected) {
    // Сразу отправляем текущие данные датчиков новому клиенту
    char buf[200];
    buildSensorJson(buf, sizeof(buf));
    chacha.sendTo(clientIdx, buf);

    // Уведомляем всех
    char notify[32];
    snprintf(notify, sizeof(notify), "{\"event\":\"peer_joined\",\"slot\":%d}", clientIdx);
    chacha.broadcast(notify);
  } else {
    char notify[32];
    snprintf(notify, sizeof(notify), "{\"event\":\"peer_left\",\"slot\":%d}", clientIdx);
    chacha.broadcast(notify);
  }
}

// WEB-СЕРВЕР
void setupWebServer() {
  webServer.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send_P(200, "text/html", HTML_PAGE);
  });

  webServer.on("/ChaChaQR.svg", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(LittleFS, CHACHA_QR_FILE, "image/svg+xml");
  });

  webServer.on("/api/sensors", HTTP_GET, [](AsyncWebServerRequest* req) {
    char buf[200];
    buildSensorJson(buf, sizeof(buf));
    req->send(200, "application/json", buf);
  });

  webServer.on("/api/key", HTTP_GET, [](AsyncWebServerRequest* req) {
    StaticJsonDocument<256> doc;
    doc["key"] = chacha.keyAsHexString();
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  webServer.begin();
  Serial.println(F("[WEB] Запущен"));
}

void setup() {
  Serial.begin(115200);
  delay(3000);
  Serial.println(F("\n[SensorServer] Старт"));

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print(F("Wi-Fi"));
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
  }
  Serial.print(F("\nIP: "));
  Serial.println(WiFi.localIP());

  chacha.generateKey(false);
  chacha.onMessage(onMessage);
  chacha.onClient(onClient);
  chacha.begin(CHACHA_PORT);
  chacha.generateQR();

  setupWebServer();

  Serial.print(F("Heap: "));
  Serial.println(ESP.getFreeHeap());
  Serial.print(F("Ключ: "));
  Serial.println(chacha.keyAsHexString());
  Serial.printf("Веб: http://%s/\n", WiFi.localIP().toString().c_str());
}

void loop() {
  chacha.loop();

  // Периодическая отправка данных датчиков всем клиентам
  static uint32_t lastSensor = 0;
  if (millis() - lastSensor >= sensorIntervalMs && chacha.connectedCount() > 0) {
    lastSensor = millis();
    readSensors();
    char buf[200];
    buildSensorJson(buf, sizeof(buf));
    chacha.broadcast(buf);
  }
}
