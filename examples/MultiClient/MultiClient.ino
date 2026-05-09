/*
 * ESP_ChaCha — пример с несколькими клиентами
 *
 * Демонстрирует:
 *   - CHACHA_MAX_CLIENTS — пул из 4 слотов
 *   - connectedCount()   — количество активных клиентов
 *   - isConnected(idx)   — проверка конкретного слота
 *   - sendTo(idx, ...)   — адресная отправка одному клиенту
 *   - broadcast(...)     — рассылка всем (синхронизация)
 *   - Персональные приветствия и нумерация клиентов
 *
 * Логика:
 *   - Каждый клиент получает персональное welcome с его номером
 *   - Команда "who" возвращает список активных слотов
 *   - Команда "private" отправляет сообщение конкретному слоту
 *   - Все события рассылаются broadcast для синхронизации
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

const char*    WIFI_SSID   = "YourSSID";
const char*    WIFI_PASS   = "YourPassword";
const uint16_t CHACHA_PORT = 8888;
const uint16_t WEB_PORT    = 80;

ESP_ChaCha     chacha;
AsyncWebServer webServer(WEB_PORT);

// СЧЁТЧИК СЕССИЙ (ДЛЯ ПЕРСОНАЛИЗАЦИИ)
uint32_t sessionCounter = 0;
uint32_t slotSession[CHACHA_MAX_CLIENTS] = {0}; // номер сессии для каждого слота

// ФОРМИРОВАНИЕ СПИСКА АКТИВНЫХ КЛИЕНТОВ
void buildClientListJson(char* buf, size_t bufSize) {
    // Начало JSON вручную — экономим память на маленьких ESP
    size_t pos = 0;
    pos += snprintf(buf + pos, bufSize - pos, "{\"type\":\"clientList\",\"count\":%u,\"slots\":[", chacha.connectedCount());

    bool first = true;
    for (uint8_t i = 0; i < CHACHA_MAX_CLIENTS; i++) {
        if (chacha.isConnected(i)) {
            pos += snprintf(buf + pos, bufSize - pos, "%s{\"slot\":%u,\"session\":%u}", first ? "" : ",", i, slotSession[i]);
            first = false;
        }
    }
    snprintf(buf + pos, bufSize - pos, "]}");
}

// КОЛБЭКИ
void onMessage(const char* jsonStr, uint8_t clientIdx) {
    Serial.printf("[MSG] #%u: %s\n", clientIdx, jsonStr);

    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, jsonStr) != DeserializationError::Ok) return;

    const char* cmd = doc["cmd"];
    if (!cmd) return;

    if (strcmp(cmd, "ping") == 0) {
        char resp[48];
        snprintf(resp, sizeof(resp), "{\"res\":\"pong\",\"from\":%u}", clientIdx);
        chacha.broadcast(resp);
    }
    else if (strcmp(cmd, "who") == 0) {
        // Кто сейчас подключён — отправляем только запросившему
        char buf[256];
        buildClientListJson(buf, sizeof(buf));
        chacha.sendTo(clientIdx, buf);
    }
    else if (strcmp(cmd, "private") == 0) {
        // Отправить личное сообщение конкретному слоту {"cmd":"private","to":2,"text":"Привет!"}
        uint8_t     toSlot = doc["to"] | 255;
        const char* text   = doc["text"] | "";

        if (toSlot < CHACHA_MAX_CLIENTS && chacha.isConnected(toSlot)) {
            char msg[128];
            snprintf(msg, sizeof(msg), "{\"type\":\"private\",\"from\":%u,\"text\":\"%s\"}", clientIdx, text);
            chacha.sendTo(toSlot, msg);

            // Подтверждение отправителю
            char ack[48];
            snprintf(ack, sizeof(ack), "{\"res\":\"sent\",\"to\":%u}", toSlot);
            chacha.sendTo(clientIdx, ack);
        } else {
            char err[64];
            snprintf(err, sizeof(err), "{\"res\":\"error\",\"msg\":\"slot %u not connected\"}", toSlot);
            chacha.sendTo(clientIdx, err);
        }
    }
    else if (strcmp(cmd, "broadcast") == 0) {
        // Клиент просит разослать своё сообщение всем
        const char* text = doc["text"] | "";
        char msg[128];
        snprintf(msg, sizeof(msg), "{\"type\":\"broadcast\",\"from\":%u,\"text\":\"%s\"}", clientIdx, text);
        chacha.broadcast(msg);
    }
    else if (strcmp(cmd, "getStats") == 0) {
        const auto& s = chacha.stats();
        char resp[160];
        snprintf(resp, sizeof(resp),
                 "{\"type\":\"stats\","
                 "\"rx\":%u,\"ok\":%u,\"badLen\":%u,\"badTag\":%u,\"tx\":%u,"
                 "\"clients\":%u,\"sessions\":%u}",
                 s.rxPackets, s.rxOk, s.rxBadLen, s.rxBadTag, s.txPackets,
                 chacha.connectedCount(), sessionCounter);
        chacha.sendTo(clientIdx, resp);
    }
    else {
        char resp[48];
        snprintf(resp, sizeof(resp), "{\"res\":\"unknown_cmd\",\"cmd\":\"%s\"}", cmd);
        chacha.sendTo(clientIdx, resp);
    }
}

void onClient(uint8_t clientIdx, bool connected) {
    if (connected) {
        slotSession[clientIdx] = ++sessionCounter;
        Serial.printf("[CLIENT] Слот #%u подключён (сессия %u)\n", clientIdx, slotSession[clientIdx]);

        // Персональное приветствие
        char welcome[128];
        snprintf(welcome, sizeof(welcome),
                 "{\"event\":\"welcome\",\"slot\":%u,\"session\":%u,\"totalClients\":%u}",
                 clientIdx, slotSession[clientIdx], chacha.connectedCount());
        chacha.sendTo(clientIdx, welcome);

        // Список всех клиентов новому участнику
        char list[256];
        buildClientListJson(list, sizeof(list));
        chacha.sendTo(clientIdx, list);

        // Уведомление всем остальным
        char notify[80];
        snprintf(notify, sizeof(notify),
                 "{\"event\":\"peer_joined\",\"slot\":%u,\"session\":%u,\"totalClients\":%u}",
                 clientIdx, slotSession[clientIdx], chacha.connectedCount());
        chacha.broadcast(notify);

    } else {
        Serial.printf("[CLIENT] Слот #%u отключился (сессия %u)\n", clientIdx, slotSession[clientIdx]);

        // Уведомление оставшимся
        char notify[80];
        snprintf(notify, sizeof(notify),
                 "{\"event\":\"peer_left\",\"slot\":%u,\"session\":%u,\"totalClients\":%u}",
                 clientIdx, slotSession[clientIdx], chacha.connectedCount());
        chacha.broadcast(notify);
        slotSession[clientIdx] = 0;
    }
}

// WEB-СЕРВЕР (ПРОСТОЙ СТАТУС)
// Хранится в PROGMEM чтобы не занимать RAM
static const char HTML_STATUS[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>MultiClient Monitor</title>
<style>
  body{font-family:Arial,sans-serif;max-width:500px;margin:40px auto;padding:0 20px;background:#f5f5f5;}
  .card{background:white;border-radius:8px;padding:20px;margin:12px 0;box-shadow:0 2px 6px rgba(0,0,0,.1);}
  .slot{display:inline-block;width:60px;height:60px;border-radius:8px;margin:6px;
        text-align:center;line-height:60px;font-weight:bold;font-size:18px;}
  .slot.on{background:#4CAF50;color:white;}
  .slot.off{background:#e0e0e0;color:#aaa;}
  .key-text{font-family:monospace;font-size:11px;word-break:break-all;
            background:#f0f0f0;padding:8px;border-radius:4px;}
  button{background:#1976D2;color:white;border:none;padding:7px 14px;border-radius:4px;cursor:pointer;margin:3px;}
  button:hover{opacity:.85;}
  .qr-container{text-align:center;}
  .qr-container img{max-width:180px;}
</style>
</head>
<body>
<h1>&#128101; MultiClient</h1>

<div class="card">
  <h2>Слоты клиентов</h2>
  <div id="slots">Загрузка...</div>
  <p style="font-size:12px;color:#888;">Обновление каждые 2 секунды</p>
</div>

<div class="card">
  <h2>Ключ</h2>
  <div class="key-text" id="keyText">Загрузка...</div>
  <br><button onclick="copyKey()">&#128203; Копировать</button>
  <div id="msg" style="min-height:16px;font-size:12px;color:#1976D2;margin-top:4px;"></div>
</div>

<div class="card">
  <h2>QR-код</h2>

  <p style="font-size:13px;color:#666;line-height:1.5;">
    QR-код содержит 32-байтовый ключ ChaCha20 в HEX формате.<br>
    Внутри QR: 64 HEX-символа (0-9 A-F).
  </p>

  <div class="key-text" style="margin-bottom:12px;">
    7333BDD51E4687C410F139FD74CD2943B455731691ABDB59691F73C0C99F1E98
  </div>

  <div class="qr-container">
    <img src="/ChaChaQR.svg" alt="QR">
  </div>

  <hr style="margin:18px 0;border:none;border-top:1px solid #eee;">

  <div style="font-size:18px;font-weight:bold;text-align:center;">Декодирование в других языках</div><br>
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
function showMsg(t){var e=document.getElementById('msg');e.textContent=t;setTimeout(function(){e.textContent='';},2000);}
function copyKey(){
  var t=document.getElementById('keyText').textContent;
  if(navigator.clipboard){navigator.clipboard.writeText(t).then(function(){showMsg('Скопировано!');});}
  else{var ta=document.createElement('textarea');ta.value=t;document.body.appendChild(ta);ta.select();document.execCommand('copy');document.body.removeChild(ta);showMsg('Скопировано!');}
}
function loadStatus(){
  fetch('/api/status').then(function(r){return r.json();}).then(function(d){
    document.getElementById('keyText').textContent = d.key;
    var html='';
    for(var i=0;i<d.maxSlots;i++){
      var on=d.slots.indexOf(i)>=0;
      html+='<div class="slot '+(on?'on':'off')+'">'+(on?'#'+i:'—')+'</div>';
    }
    document.getElementById('slots').innerHTML = html;
  });
}
loadStatus();
setInterval(loadStatus,2000);
</script>
</body>
</html>
)rawhtml";

void setupWebServer() {
    webServer.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send_P(200, "text/html", HTML_STATUS);
    });

    webServer.on("/ChaChaQR.svg", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(LittleFS, CHACHA_QR_FILE, "image/svg+xml");
    });

    webServer.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* req) {
        StaticJsonDocument<256> doc;
        doc["key"]      = chacha.keyAsHexString();
        doc["maxSlots"] = CHACHA_MAX_CLIENTS;
        doc["sessions"] = sessionCounter;

        JsonArray slots = doc.createNestedArray("slots");
        for (uint8_t i = 0; i < CHACHA_MAX_CLIENTS; i++) {
            if (chacha.isConnected(i)) slots.add(i);
        }

        String out; serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    webServer.begin();
    Serial.println(F("[WEB] Запущен"));
}

void setup() {
    Serial.begin(115200);
    delay(3000);
    Serial.println(F("\n[MultiClient] Старт"));

    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print(F("Wi-Fi"));
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print('.'); }
    Serial.print(F("\nIP: ")); Serial.println(WiFi.localIP());

    chacha.generateKey(false);
    chacha.onMessage(onMessage);
    chacha.onClient(onClient);
    chacha.begin(CHACHA_PORT);
    chacha.generateQR();

    setupWebServer();

    Serial.print(F("Heap: ")); Serial.println(ESP.getFreeHeap());
    Serial.print(F("Ключ: ")); Serial.println(chacha.keyAsHexString());
    Serial.printf("Веб: http://%s/\n", WiFi.localIP().toString().c_str());
}

void loop() {
    chacha.loop();
}
