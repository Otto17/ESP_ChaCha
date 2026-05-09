/*
 * ESP_ChaCha — пример базового сервера
 *
 * Демонстрирует:
 *   - автогенерацию ключа с сохранением в LittleFS
 *   - ручное задание ключа через useKey()
 *   - обработку команд от клиентов через колбэк onMessage
 *   - отправку ответов одному клиенту (sendTo) и всем клиентам (broadcast)
 *   - периодическую телеметрию
 *   - генерацию QR-кода ключа в LittleFS
 *
 * Протестировано на ESP8266 и ESP32.
 */

#include <ESP_ChaCha.h>
#include <ArduinoJson.h>

// НАСТРОЙКИ
const char*    WIFI_SSID = "YourSSID";
const char*    WIFI_PASS = "YourPassword";
const uint16_t PORT      = 8888;

// Опция A: задать ключ вручную — раскомментируйте и вставьте свои байты
// Ключ не сохраняется в LittleFS при использовании useKey()
// const uint8_t MY_KEY[32] = {
//     0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
//     0x09,0x10,0x11,0x12,0x13,0x14,0x15,0x16,
//     0x17,0x18,0x19,0x20,0x21,0x22,0x23,0x24,
//     0x25,0x26,0x27,0x28,0x29,0x30,0x31,0x32
// };

// LED_BUILTIN
// На некоторых платах ESP32 LED_BUILTIN не определён. Тогда задать вручную.
#ifndef LED_BUILTIN
  #define LED_BUILTIN 2  // GPIO2 — стандартный встроенный LED для ESP8266 и большинства ESP32
#endif

// ОБЪЕКТ БИБЛИОТЕКИ
ESP_ChaCha chacha;

// КОЛБЭК ВХОДЯЩИХ СООБЩЕНИЙ
void onMessage(const char* jsonStr, uint8_t clientIdx) {
    Serial.print(F("[MSG] Клиент #"));
    Serial.print(clientIdx);
    Serial.print(F(": "));
    Serial.println(jsonStr);

    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, jsonStr) != DeserializationError::Ok) {
        Serial.println(F("[WARN] Невалидный JSON"));
        return;
    }

    const char* cmd = doc["cmd"];
    if (!cmd) return;

    if (strcmp(cmd, "ping") == 0) {
        // broadcast: все клиенты получают одинаковый ответ → синхронизация состояния
        chacha.broadcast("{\"res\":\"pong\"}");
    }
    else if (strcmp(cmd, "getKey") == 0) {
        // sendTo: ответ только запросившему клиенту
        char resp[200];
        snprintf(resp, sizeof(resp), "{\"key\":\"%s\"}", chacha.keyAsHexString());
        chacha.sendTo(clientIdx, resp);
    }
    else if (strcmp(cmd, "relay") == 0) {
        bool state = doc["state"] | false;
        // LED активен на LOW для большинства ESP8266/ESP32 плат
        digitalWrite(LED_BUILTIN, state ? LOW : HIGH);

        // broadcast: все клиенты узнают о новом состоянии реле
        char resp[64];
        snprintf(resp, sizeof(resp), "{\"event\":\"relay\",\"state\":%s}", state ? "true" : "false");
        chacha.broadcast(resp);
    }
    else {
        chacha.sendTo(clientIdx, "{\"res\":\"unknown_cmd\"}");
    }
}

// КОЛБЭК ПОДКЛЮЧЕНИЯ/ ОТКЛЮЧЕНИЯ
void onClient(uint8_t clientIdx, bool connected) {
    Serial.print(F("[CLIENT] Слот #"));
    Serial.print(clientIdx);
    Serial.println(connected ? F(" подключён") : F(" отключён"));

    if (connected) {
        // Приветствие новому клиенту
        chacha.sendTo(clientIdx, "{\"event\":\"welcome\",\"v\":1}");

        // Уведомление всем о новом подключении
        char notify[48];
        snprintf(notify, sizeof(notify), "{\"event\":\"peer_joined\",\"slot\":%d}", clientIdx);
        chacha.broadcast(notify);
    }
}

void setup() {
    Serial.begin(115200);
    delay(3000); // Пауза чтобы успеть открыть монитор порта
    Serial.println(F("\n[ESP_ChaCha] Старт"));

    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, HIGH); // LED выключен (активный LOW)

    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print(F("Подключаемся к Wi-Fi"));
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print('.');
    }
    Serial.print(F("\nIP: "));
    Serial.println(WiFi.localIP());

    // Опция A: ручной ключ
	// chacha.useKey(MY_KEY);
	
    // Опция B: автогенерация (загружает из LittleFS или создаёт новый)
    // Принудительно новый ключ: chacha.generateKey(true)
    chacha.generateKey(false);

    chacha.onMessage(onMessage);
    chacha.onClient(onClient);

    chacha.begin(PORT);

    Serial.print(F("Свободно heap после старта: "));
    Serial.println(ESP.getFreeHeap());

    Serial.print(F("Ключ: "));
    Serial.println(chacha.keyAsHexString());

    if (chacha.generateQR()) {
        Serial.println(F("QR сохранён в /ChaChaQR.svg"));
    }

    Serial.print(F("Порт: "));
    Serial.println(PORT);
}

void loop() {
    chacha.loop();

    // Телеметрия каждые 2 секунды при наличии подключённых клиентов
    static uint32_t lastTelem = 0;
    if (millis() - lastTelem >= 2000 && chacha.connectedCount() > 0) {
        lastTelem = millis();
        char telem[80];
        snprintf(telem, sizeof(telem),
                 "{\"type\":\"telemetry\",\"uptime\":%lu,\"freeHeap\":%u}",
                 millis() / 1000UL,
                 (unsigned)ESP.getFreeHeap());
        chacha.broadcast(telem);
    }

    // Статистика каждые 30 секунд
    static uint32_t lastStats = 0;
    if (millis() - lastStats >= 30000) {
        lastStats = millis();
        const auto& s = chacha.stats();
        Serial.printf("[STATS] rx:%u ok:%u badLen:%u badTag:%u tx:%u clients:%u\n",
                      s.rxPackets, s.rxOk, s.rxBadLen, s.rxBadTag,
                      s.txPackets, chacha.connectedCount());
    }
}
