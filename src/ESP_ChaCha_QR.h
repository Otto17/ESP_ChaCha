// Copyright (c) 2026 Otto
// Лицензия: MIT (см. LICENSE)

#pragma once

/*
* ESP_ChaCha_QR — встроенный генератор QR-кода в формате SVG
*
* НЕ требует сторонних библиотек.
* Код QR-движка портирован из qrcode.c (Richard Moore, MIT License)
*
* Кодируемые данные: HEX(key) = 64 символа (только 0-9, A-F)
* QR версия 4, ECC Low, режим Byte — матрица 33×33 модулей
*
* Совместимость: ESP8266 / ESP32
*/

#include <Arduino.h>
#include <LittleFS.h>
#include <stdint.h>

/*
* Генерирует QR-код из 32-байтового ключа и сохраняет SVG по указанному пути
* Возвращает true при успехе
*/
bool espChaChaGenerateQR(const uint8_t key[32], const char* svgPath);