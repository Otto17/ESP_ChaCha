// Copyright (c) 2026 Otto
// Лицензия: MIT (см. LICENSE)

#pragma once

/*
 * ESP_ChaCha — лёгкий шифрованный TCP-сервер на ChaCha20-Poly1305
 * Платформы: ESP8266 / ESP32
 *
 * Протокол пакета (одинаков в обе стороны):
 *   [uint16_t длина LE] [nonce 12 байт] [ciphertext N байт] [tag 16 байт]
 *
 * Зависимости (установить через Library Manager):
 *   - arduinolibraries/Crypto  (содержит ChaChaPoly)
 *   - bblanchon/ArduinoJson    (используется в скетче пользователя)
 */

// ЗАВИСИМОСТИ ПЛАТФОРМЫ
#ifdef ESP8266
  #include <ESP8266WiFi.h>
  // os_random() объявлена в <osapi.h>, который подтягивается через ESP8266WiFi.h
  // Своё объявление не делаем — вызывает конфликт типов (uint32_t vs unsigned long)
#else
  #include <WiFi.h>
#endif

#include <Arduino.h>
#include <LittleFS.h>
#include <ChaChaPoly.h>

// КОНСТАНТЫ
static constexpr uint8_t  CHACHA_KEY_SIZE      = 32;   // Байт, размер ключа ChaCha20
static constexpr uint8_t  CHACHA_NONCE_SIZE    = 12;   // Байт, размер nonce
static constexpr uint8_t  CHACHA_TAG_SIZE      = 16;   // Байт, размер тега Poly1305
static constexpr uint16_t CHACHA_MAX_PACKET    = 2048; // Байт, максимальный размер входного пакета
static constexpr uint8_t  CHACHA_MAX_CLIENTS   = 4;    // Максимум одновременных TCP-клиентов
static constexpr uint16_t CHACHA_RX_TIMEOUT_MS = 150;  // Ожидания полного пакета в (мс)

static constexpr const char* CHACHA_KEY_FILE = "/ChaChaSecret.key";
static constexpr const char* CHACHA_QR_FILE  = "/ChaChaQR.svg";

// ТИПЫ КОЛБЭКОВ ПОЛЬЗОВАТЕЛЯ

/*
 * Вызывается при получении расшифрованного JSON от любого клиента
 *   jsonStr     — тело JSON (нуль-терминированная строка)
 *   clientIndex — индекс клиента в пуле [0 .. CHACHA_MAX_CLIENTS-1]
 */
using ChaChaMessageCallback = void (*)(const char* jsonStr, uint8_t clientIndex);

/*
 * Вызывается при подключении или отключении клиента
 *   clientIndex — индекс клиента в пуле
 *   connected   — true при подключении, false при отключении
 */
using ChaChaClientCallback = void (*)(uint8_t clientIndex, bool connected);

// ГЛАВНЫЙ КЛАСС
class ESP_ChaCha {
public:

    // КОНСТРУКТОР / ДЕСТРУКТОР
    ESP_ChaCha();
    ~ESP_ChaCha();

    // ИНИЦИАЛИЗАЦИЯ

    /*
     * Запускает TCP-сервер на указанном порту
     * LittleFS монтируется автоматически если ещё не смонтирована
     * Если ключ не задан через useKey() или generateKey() — вызывает
     * generateKey() автоматически
     */
    bool begin(uint16_t port = 8888);

    /*
     * Задать ключ вручную (32 байта). Вызывать ДО begin()
     * Ключ не сохраняется в LittleFS
     */
    void useKey(const uint8_t key[CHACHA_KEY_SIZE]);

    /*
     * Загрузить ключ из LittleFS или сгенерировать новый аппаратным RNG
     *   forceNew = false — загружает существующий ключ, генерирует только если файла нет
     *   forceNew = true  — всегда генерирует и сохраняет новый ключ
     * Вызывать ДО begin() или в любом месте скетча
     * Возвращает true при успехе
     */
    bool generateKey(bool forceNew = false);

    // РЕГИСТРАЦИ КОЛБЭКОВ
    void onMessage(ChaChaMessageCallback cb) { _msgCb = cb; }
    void onClient (ChaChaClientCallback  cb) { _cliCb = cb; }

    // ГЛАВНЫЙ ЦИКЛ
    // Вызывать в каждой итерации loop() скетча
    void loop();

    // ОТПРАВКА ДАННЫХ

    /*
     * Отправить зашифрованный пакет всем подключённым клиентам
     * Обеспечивает полную синхронизацию состояния между всеми клиентами
     */
    void broadcast(const char* jsonStr);

    /*
     * Отправить зашифрованный пакет конкретному клиенту по его индексу
     */
    void sendTo(uint8_t clientIndex, const char* jsonStr);

    // СОСТОЯНИЕ СОЕДИНЕНИЙ
    uint8_t connectedCount()                 const;
    bool    isConnected(uint8_t clientIndex) const;

    // РАБОТА С КЛЮЧОМ

    /*
     * Возвращает текущий ключ в виде строки формата "0x01,0x02,...,0x20"
     * Удобно вывести на веб-страницу для копирования клиентом
     * Строка хранится во внутреннем буфере — не вызывать из ISR и не кэшировать указатель
     */
    const char* keyAsHexString() const;

    /*
     * Возвращает указатель на 32 байта ключа (read-only)
     */
    const uint8_t* rawKey() const { return _key; }

    // QR-КОД

    /*
     * Генерирует QR-код из текущего ключа и сохраняет как SVG в LittleFS
     * Файл: CHACHA_QR_FILE ("/ChaChaQR.svg")
     * QR кодирует HEX(key) — 64 символа, версия 4, уровень коррекции L
     * Возвращает true при успехе
     * После вызова SVG-файл можно отдать через веб-сервер для отображения
     */
    bool generateQR();

    // СТАТИСТИКА
    struct Stats {
        uint32_t rxPackets = 0; // Принято пакетов всего
        uint32_t rxOk      = 0; // Успешно расшифровано
        uint32_t rxBadLen  = 0; // Отброшено из-за неверной длины
        uint32_t rxBadTag  = 0; // Отброшено из-за неверного тега аутентификации
        uint32_t txPackets = 0; // Отправлено пакетов
    };

    const Stats& stats()    const { return _stats; }
    void         resetStats()     { memset(&_stats, 0, sizeof(_stats)); }

// ПРИВАТНАЯ ЧАСТЬ
private:

    // Два отдельных экземпляра ChaChaPoly — для шифрования и расшифровки
    // Размещены в куче объекта (не на стеке вызова) во избежание переполнения стека на ESP8266 (~4 KB) при вызове из колбэков
    ChaChaPoly _chachaEnc;
    ChaChaPoly _chachaDec;

    // Контекст одного TCP-соединения
    struct ClientSlot {
        WiFiClient client;
        bool       active = false;
    };

    WiFiServer*  _server   = nullptr;
    ClientSlot   _slots[CHACHA_MAX_CLIENTS]; // memset запрещён — WiFiClient имеет конструктор
    uint8_t      _key[CHACHA_KEY_SIZE];
    bool         _keyReady = false;
    bool         _fsReady  = false;

    ChaChaMessageCallback _msgCb = nullptr;
    ChaChaClientCallback  _cliCb = nullptr;
    Stats                 _stats;

    // Внутренний буфер для keyAsHexString(): "0xNN," * 32 + '\0'
    mutable char _hexBuf[CHACHA_KEY_SIZE * 6 + 1];

    // ПРИВАТНЫЕ МЕТОДЫ
    bool   _mountFS();
    bool   _loadKeyFromFile();
    bool   _saveKeyToFile();
    void   _fillRandom(uint8_t* buf, size_t len);
    void   _processSlot(uint8_t idx);
    size_t _decrypt(const uint8_t* packet, size_t packetLen,
                    uint8_t* plainOut, size_t plainBufLen);
    bool   _encryptAndSend(WiFiClient& client, const char* plainStr, size_t plainLen);
};
