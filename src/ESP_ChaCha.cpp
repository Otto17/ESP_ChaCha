// Copyright (c) 2026 Otto
// Лицензия: MIT (см. LICENSE)

#include "ESP_ChaCha.h"
#include "ESP_ChaCha_QR.h"

// КОНСТРУКТОР / ДЕСТРУКТОР

ESP_ChaCha::ESP_ChaCha() {
    memset(_key,    0, sizeof(_key));
    memset(_hexBuf, 0, sizeof(_hexBuf));
    // _slots НЕ обнуляем через memset — WiFiClient содержит внутренние указатели и инициализируется своим конструктором автоматически
    // memset поверх объекта класса → повреждение указателей → Exception 28
}

ESP_ChaCha::~ESP_ChaCha() {
    if (_server) {
        _server->stop();
        delete _server;
        _server = nullptr;
    }
}

// ПУБЛИЧНЫЙ API
void ESP_ChaCha::useKey(const uint8_t key[CHACHA_KEY_SIZE]) {
    memcpy(_key, key, CHACHA_KEY_SIZE);
    _keyReady = true;
}

bool ESP_ChaCha::generateKey(bool forceNew) {
    if (!_mountFS()) return false;

    if (!forceNew && LittleFS.exists(CHACHA_KEY_FILE)) {
        if (_loadKeyFromFile()) {
            _keyReady = true;
            return true;
        }
        // Файл есть но не читается — перегенерируем
    }

    _fillRandom(_key, CHACHA_KEY_SIZE);

    if (!_saveKeyToFile()) return false;

    _keyReady = true;
    return true;
}

bool ESP_ChaCha::begin(uint16_t port) {
    _mountFS();

    if (!_keyReady) {
        if (!generateKey(false)) {
            Serial.println(F("[ChaCha] WARN: ключ недоступен, используется нулевой!"));
        }
    }

    _server = new WiFiServer(port);
    _server->begin();

    Serial.print(F("[ChaCha] TCP сервер запущен на порту "));
    Serial.println(port);
    return true;
}

void ESP_ChaCha::loop() {
    if (!_server) return;

    // ПРИНИМАЕМ НОВЫЕ ПОДКЛЮЧЕНИЯ
    if (_server->hasClient()) {
        WiFiClient incoming = _server->available();
        bool placed = false;

        for (uint8_t i = 0; i < CHACHA_MAX_CLIENTS; i++) {
            if (!_slots[i].active) {
                _slots[i].client = incoming;
                _slots[i].active = true;
                placed = true;

                Serial.print(F("[ChaCha] Клиент подключён, слот "));
                Serial.println(i);
                if (_cliCb) _cliCb(i, true);
                break;
            }
        }

        if (!placed) {
            Serial.println(F("[ChaCha] WARN: все слоты заняты, клиент отклонён"));
            incoming.stop();
        }
    }

    // ОБРАБАТЫВАЕМ АКТИВНЫЕ СЛОТЫ
    for (uint8_t i = 0; i < CHACHA_MAX_CLIENTS; i++) {
        if (_slots[i].active) {
            _processSlot(i);
        }
    }
}

// ОТПРАВКА

void ESP_ChaCha::broadcast(const char* jsonStr) {
    size_t len = strlen(jsonStr);
    for (uint8_t i = 0; i < CHACHA_MAX_CLIENTS; i++) {
        if (_slots[i].active && _slots[i].client.connected()) {
            _encryptAndSend(_slots[i].client, jsonStr, len);
        }
    }
}

void ESP_ChaCha::sendTo(uint8_t clientIndex, const char* jsonStr) {
    if (clientIndex >= CHACHA_MAX_CLIENTS)          return;
    if (!_slots[clientIndex].active)                return;
    if (!_slots[clientIndex].client.connected())    return;

    _encryptAndSend(_slots[clientIndex].client, jsonStr, strlen(jsonStr));
}

// СОСТОЯНИЕ

uint8_t ESP_ChaCha::connectedCount() const {
    uint8_t cnt = 0;
    for (uint8_t i = 0; i < CHACHA_MAX_CLIENTS; i++) {
        if (_slots[i].active) cnt++;
    }
    return cnt;
}

bool ESP_ChaCha::isConnected(uint8_t clientIndex) const {
    if (clientIndex >= CHACHA_MAX_CLIENTS) return false;
    return _slots[clientIndex].active;
}

// КЛЮЧ

const char* ESP_ChaCha::keyAsHexString() const {
    char* p = _hexBuf;
    for (uint8_t i = 0; i < CHACHA_KEY_SIZE; i++) {
        p += sprintf(p, "0x%02X%s", _key[i], i < CHACHA_KEY_SIZE - 1 ? "," : "");
    }
    return _hexBuf;
}

// QR-КОД

bool ESP_ChaCha::generateQR() {
    if (!_keyReady) {
        Serial.println(F("[ChaCha] generateQR: ключ не задан!"));
        return false;
    }
    if (!_mountFS()) return false;
    return espChaChaGenerateQR(_key, CHACHA_QR_FILE);
}

// ПРИВАТНЫЕ МЕТОДЫ

bool ESP_ChaCha::_mountFS() {
    if (_fsReady) return true;

#ifdef ESP8266
    // На ESP8266 begin() не принимает аргументов
    // При неудаче форматируем раздел и пробуем снова
    if (!LittleFS.begin()) {
        Serial.println(F("[ChaCha] LittleFS: форматируем раздел..."));
        if (!LittleFS.format() || !LittleFS.begin()) {
            Serial.println(F("[ChaCha] ERROR: LittleFS недоступна!"));
            return false;
        }
    }
#else
    // На ESP32 begin(true) форматирует раздел автоматически при необходимости
    if (!LittleFS.begin(true)) {
        Serial.println(F("[ChaCha] ERROR: LittleFS недоступна!"));
        return false;
    }
#endif

    _fsReady = true;
    return true;
}

bool ESP_ChaCha::_loadKeyFromFile() {
    File f = LittleFS.open(CHACHA_KEY_FILE, "r");
    if (!f) return false;

    bool ok = (f.read(_key, CHACHA_KEY_SIZE) == CHACHA_KEY_SIZE);
    f.close();
    return ok;
}

bool ESP_ChaCha::_saveKeyToFile() {
    File f = LittleFS.open(CHACHA_KEY_FILE, "w");
    if (!f) {
        Serial.println(F("[ChaCha] ERROR: не могу записать ключ!"));
        return false;
    }
    f.write(_key, CHACHA_KEY_SIZE);
    f.close();
    return true;
}

void ESP_ChaCha::_fillRandom(uint8_t* buf, size_t len) {
    for (size_t i = 0; i < len; i += 4) {
#ifdef ESP8266
        uint32_t r = (uint32_t)os_random();
#else
        uint32_t r = esp_random();
#endif
        for (size_t j = i; j < i + 4 && j < len; j++) {
            buf[j] = (uint8_t)(r & 0xFF);
            r >>= 8;
        }
    }
}

// ОБРАБОТКА СЛОТА

void ESP_ChaCha::_processSlot(uint8_t idx) {
    WiFiClient& client = _slots[idx].client;

    if (!client.connected()) {
        Serial.print(F("[ChaCha] Клиент отключился, слот "));
        Serial.println(idx);
        client.stop();
        _slots[idx].active = false;
        if (_cliCb) _cliCb(idx, false);
        return;
    }

    if (client.available() < 2) return;

    // Читаем 2 байта длины payload (little-endian)
    uint8_t  lenBuf[2];
    client.read(lenBuf, 2);
    uint16_t packetSize = (uint16_t)lenBuf[0] | ((uint16_t)lenBuf[1] << 8);

    // Минимальный валидный размер: nonce(12) + 1 байт данных + tag(16) = 29
    if (packetSize < (CHACHA_NONCE_SIZE + 1 + CHACHA_TAG_SIZE) ||
        packetSize > CHACHA_MAX_PACKET) {
        Serial.print(F("[ChaCha] WARN: неверный размер пакета: "));
        Serial.println(packetSize);
        _stats.rxBadLen++;
        while (client.available()) { client.read(); yield(); }
        return;
    }

    // Ждём полного пакета. Используем только yield() — delay() блокирует watchdog на ESP8266 при интенсивном трафике
    uint32_t tStart = millis();
    while ((size_t)client.available() < packetSize) {
        if (millis() - tStart > CHACHA_RX_TIMEOUT_MS) {
            Serial.println(F("[ChaCha] WARN: тайм-аут пакета"));
            _stats.rxBadLen++;
            client.stop();
            _slots[idx].active = false;
            if (_cliCb) _cliCb(idx, false);
            return;
        }
        yield();
    }

    uint8_t* pktBuf = new (std::nothrow) uint8_t[packetSize];
    if (!pktBuf) {
        Serial.print(F("[ChaCha] ERROR: нет памяти! heap="));
        Serial.println(ESP.getFreeHeap());
        while (client.available()) { client.read(); yield(); }
        return;
    }
    client.read(pktBuf, packetSize);
    _stats.rxPackets++;

    size_t   plainMaxLen = packetSize - CHACHA_NONCE_SIZE - CHACHA_TAG_SIZE;
    uint8_t* plain       = new (std::nothrow) uint8_t[plainMaxLen + 1];

    if (!plain) {
        Serial.print(F("[ChaCha] ERROR: нет памяти! heap="));
        Serial.println(ESP.getFreeHeap());
        delete[] pktBuf;
        return;
    }

    size_t plainLen = _decrypt(pktBuf, packetSize, plain, plainMaxLen + 1);
    delete[] pktBuf;

    if (plainLen > 0) {
        _stats.rxOk++;
        plain[plainLen] = '\0';
        if (_msgCb) _msgCb((const char*)plain, idx);
    } else {
        _stats.rxBadTag++;
        Serial.println(F("[ChaCha] WARN: неверный тег, пакет отброшен"));
    }

    delete[] plain;
}

// ШИФРОВАНИЕ / РАСШИФРОВКА

size_t ESP_ChaCha::_decrypt(const uint8_t* packet, size_t packetLen,
                             uint8_t* plainOut, size_t plainBufLen) {
    if (packetLen < CHACHA_NONCE_SIZE + 1 + CHACHA_TAG_SIZE) return 0;

    const uint8_t* nonce      = packet;
    size_t         cipherLen  = packetLen - CHACHA_NONCE_SIZE - CHACHA_TAG_SIZE;
    const uint8_t* cipherText = packet + CHACHA_NONCE_SIZE;
    const uint8_t* tag        = packet + CHACHA_NONCE_SIZE + cipherLen;

    if (cipherLen > plainBufLen) return 0;

    _chachaDec.clear();
    _chachaDec.setKey(_key, CHACHA_KEY_SIZE);
    _chachaDec.setIV(nonce, CHACHA_NONCE_SIZE);
    _chachaDec.decrypt(plainOut, cipherText, cipherLen);

    if (!_chachaDec.checkTag(tag, CHACHA_TAG_SIZE)) {
        memset(plainOut, 0, cipherLen);
        return 0;
    }

    return cipherLen;
}

bool ESP_ChaCha::_encryptAndSend(WiFiClient& client,
                                  const char* plainStr, size_t plainLen) {
    if (!client.connected()) return false;

    uint8_t nonce[CHACHA_NONCE_SIZE];
    uint8_t tag  [CHACHA_TAG_SIZE];
    _fillRandom(nonce, CHACHA_NONCE_SIZE);

    uint8_t* cipher = new (std::nothrow) uint8_t[plainLen];
    if (!cipher) {
        Serial.println(F("[ChaCha] ERROR: нет памяти для шифртекста!"));
        return false;
    }

    _chachaEnc.clear();
    _chachaEnc.setKey(_key, CHACHA_KEY_SIZE);
    _chachaEnc.setIV(nonce, CHACHA_NONCE_SIZE);
    _chachaEnc.encrypt(cipher, (const uint8_t*)plainStr, plainLen);
    _chachaEnc.computeTag(tag, CHACHA_TAG_SIZE);

    // Собираем весь пакет в один буфер и отправляем одним вызовом write()
    // Несколько последовательных write() на ESP8266 могут дать несколько TCP-сегментов, что усложняет чтение на стороне клиента
    size_t   totalLen = 2 + CHACHA_NONCE_SIZE + plainLen + CHACHA_TAG_SIZE;
    uint8_t* outBuf   = new (std::nothrow) uint8_t[totalLen];
    if (!outBuf) {
        Serial.println(F("[ChaCha] ERROR: нет памяти для исходящего пакета!"));
        delete[] cipher;
        return false;
    }

    uint16_t fullLen = (uint16_t)(CHACHA_NONCE_SIZE + plainLen + CHACHA_TAG_SIZE);
    size_t pos = 0;
    outBuf[pos++] = (uint8_t)(fullLen & 0xFF);
    outBuf[pos++] = (uint8_t)(fullLen >> 8);
    memcpy(outBuf + pos, nonce,  CHACHA_NONCE_SIZE); pos += CHACHA_NONCE_SIZE;
    memcpy(outBuf + pos, cipher, plainLen);           pos += plainLen;
    memcpy(outBuf + pos, tag,    CHACHA_TAG_SIZE);

    delete[] cipher;

    size_t written = client.write(outBuf, totalLen);
    delete[] outBuf;

    if (written != totalLen) {
        Serial.print(F("[ChaCha] WARN: отправлено "));
        Serial.print(written);
        Serial.print(F(" из "));
        Serial.println(totalLen);
        return false;
    }

    _stats.txPackets++;
    return true;
}
