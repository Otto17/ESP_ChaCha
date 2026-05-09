// Copyright (c) 2026 Otto
// Лицензия: MIT (см. LICENSE)

/*
 * QR-генератор для ESP_ChaCha
 *
 * QR-движок портирован из qrcode.c (Richard Moore, "https://github.com/ricmoo/QRCode", MIT License)
 *
 * Кодируемые данные: HEX(key) = 64 символа (только 0-9, A-F)
 * QR версия 4, ECC Low, режим Byte — матрица 33×33 модулей
 *
 * Параметры v4L (ISO 18004):
 *   size          = 33  (4*4+17)
 *   moduleCount   = 807
 *   totalCodewords = floor(807/8) = 100 (+ 7 remainder bits)
 *   ECcodewords   = 20  (1 блок)
 *   dataCodewords = 80
 *   numBlocks     = 1
 *   remainderBits = 7
 *
 * Клиент читает 64 HEX-символа из QR и преобразует в 32 байта ключа
 * стандартной функцией: for(i=0;i<32;i++) key[i]=strtol(hex+i*2, NULL, 16);
 */

#include "ESP_ChaCha_QR.h"
#include <stdlib.h>
#include <string.h>

// ФИКСИРОВАННЫЕ ПАРАМЕТРЫ QR v4, ECC Low
#define QR_VERSION        4u
#define QR_SIZE           33u   // 4*4+17
#define QR_ECC_FORMAT     1u    // ECC Low → индикатор битов формата = 01b = 1

// Из таблиц ISO 18004 / ricmoo для v4L:
#define QR_EC_CODEWORDS   20u   // NUM_ERROR_CORRECTION_CODEWORDS[Low][3] = 20
#define QR_NUM_BLOCKS     1u    // NUM_ERROR_CORRECTION_BLOCKS[Low][3] = 1
#define QR_RAW_MODULES    807u  // NUM_RAW_DATA_MODULES[3] = 807
#define QR_DATA_CAPACITY  80u   // floor(807/8)=100 кодовых слов, 100-20 EC = 80 байт данных

// Размер сетки: ceil(33*33/8) = ceil(1089/8) = 137 байт
#define QR_GRID_BYTES     ((QR_SIZE * QR_SIZE + 7u) / 8u)

// Размер буфера кодовых слов: ceil(807/8) = 101 байт
#define QR_CW_BYTES       ((QR_RAW_MODULES + 7u) / 8u)

// Данные: HEX(32 байта) = 64 символа < QR_DATA_CAPACITY(80)
#define QR_DATA_LEN       64u

// СТАТИЧЕСКИЕ БУФЕРЫ (В BSS, НЕ ЗАНИМАЮТ СТЕК)
static uint8_t s_modules [QR_GRID_BYTES];
static uint8_t s_isFunc  [QR_GRID_BYTES];
static uint8_t s_codewords[QR_CW_BYTES];

// БИТОВЫЙ БУФЕР
typedef struct {
    uint32_t bitOffsetOrWidth; // Смещение в битах (режим буфера) или ширина сетки (режим сетки)
    uint16_t capacityBytes;    // Ёмкость буфера в байтах
    uint8_t* data;             // Указатель на данные
} BitBucket;

static void bb_initBuffer(BitBucket* b, uint8_t* data, uint16_t capBytes) {
    b->bitOffsetOrWidth = 0;
    b->capacityBytes    = capBytes;
    b->data             = data;
    memset(data, 0, capBytes);
}

static void bb_initGrid(BitBucket* b, uint8_t* data, uint8_t size) {
    b->bitOffsetOrWidth = size;
    b->capacityBytes    = (uint16_t)((size * size + 7u) / 8u);
    b->data             = data;
    memset(data, 0, b->capacityBytes);
}

static void bb_appendBits(BitBucket* b, uint32_t val, uint8_t length) {
    uint32_t offset = b->bitOffsetOrWidth;
    for (int8_t i = (int8_t)(length - 1); i >= 0; i--, offset++) {
        b->data[offset >> 3] |= ((val >> i) & 1u) << (7u - (offset & 7u));
    }
    b->bitOffsetOrWidth = offset;
}

static void bb_setBit(BitBucket* b, uint8_t x, uint8_t y, bool on) {
    uint32_t offset = (uint32_t)y * b->bitOffsetOrWidth + x;
    uint8_t  mask   = (uint8_t)(1u << (7u - (offset & 7u)));
    if (on) b->data[offset >> 3] |=  mask;
    else    b->data[offset >> 3] &= ~mask;
}

static void bb_invertBit(BitBucket* b, uint8_t x, uint8_t y, bool invert) {
    uint32_t offset = (uint32_t)y * b->bitOffsetOrWidth + x;
    uint8_t  mask   = (uint8_t)(1u << (7u - (offset & 7u)));
    bool     on     = (b->data[offset >> 3] & mask) != 0;
    if (on ^ invert) b->data[offset >> 3] |=  mask;
    else             b->data[offset >> 3] &= ~mask;
}

static bool bb_getBit(BitBucket* b, uint8_t x, uint8_t y) {
    uint32_t offset = (uint32_t)y * b->bitOffsetOrWidth + x;
    return (b->data[offset >> 3] & (1u << (7u - (offset & 7u)))) != 0;
}

// РИД-СОЛОМОН
static uint8_t rs_multiply(uint8_t x, uint8_t y) {
    uint16_t z = 0;
    for (int8_t i = 7; i >= 0; i--) {
        z = (uint16_t)((z << 1) ^ ((z >> 7) * 0x11Du));
        z ^= (uint16_t)(((y >> i) & 1u) * x);
    }
    return (uint8_t)z;
}

static void rs_init(uint8_t degree, uint8_t* coeff) {
    memset(coeff, 0, degree);
    coeff[degree - 1] = 1;
    uint16_t root = 1;
    for (uint8_t i = 0; i < degree; i++) {
        for (uint8_t j = 0; j < degree; j++) {
            coeff[j] = rs_multiply(coeff[j], (uint8_t)root);
            if (j + 1 < degree) coeff[j] ^= coeff[j + 1];
        }
        root = (uint16_t)((root << 1) ^ ((root >> 7) * 0x11Du));
    }
}

static void rs_getRemainder(uint8_t degree, const uint8_t* coeff, const uint8_t* data, uint8_t length,uint8_t* result) {
    memset(result, 0, degree);
    for (uint8_t i = 0; i < length; i++) {
        uint8_t factor = data[i] ^ result[0];
        memmove(result, result + 1, degree - 1);
        result[degree - 1] = 0;
        for (uint8_t j = 0; j < degree; j++) {
            result[j] ^= rs_multiply(coeff[j], factor);
        }
    }
}

// ФУНКЦИОНАЛЬНЫЕ ПАТТЕРНЫ
static void setFunctionModule(BitBucket* mod, BitBucket* isf, uint8_t x, uint8_t y, bool on) {
    bb_setBit(mod, x, y, on);
    bb_setBit(isf, x, y, true);
}

static void drawFinderPattern(BitBucket* mod, BitBucket* isf, uint8_t cx, uint8_t cy) {
    for (int8_t dy = -4; dy <= 4; dy++) {
        for (int8_t dx = -4; dx <= 4; dx++) {
            int16_t x = cx + dx, y = cy + dy;
            if (x < 0 || x >= (int16_t)QR_SIZE ||
                y < 0 || y >= (int16_t)QR_SIZE) continue;
            uint8_t dist = (uint8_t)(abs(dx) > abs(dy) ? abs(dx) : abs(dy));
            setFunctionModule(mod, isf, (uint8_t)x, (uint8_t)y,
                              dist != 2 && dist != 4);
        }
    }
}

static void drawAlignmentPattern(BitBucket* mod, BitBucket* isf, uint8_t cx, uint8_t cy) {
    for (int8_t dy = -2; dy <= 2; dy++) {
        for (int8_t dx = -2; dx <= 2; dx++) {
            uint8_t dist = (uint8_t)(abs(dx) > abs(dy) ? abs(dx) : abs(dy));
            setFunctionModule(mod, isf,
                              (uint8_t)(cx + dx), (uint8_t)(cy + dy),
                              dist != 1);
        }
    }
}

static void drawFormatBits(BitBucket* mod, BitBucket* isf, uint8_t ecc, uint8_t mask) {
    uint32_t data = ((uint32_t)ecc << 3) | mask;
    uint32_t rem  = data;
    for (uint8_t i = 0; i < 10; i++) {
        rem = (rem << 1) ^ ((rem >> 9) * 0x537u);
    }
    uint32_t bits = ((data << 10) | rem) ^ 0x5412u;

    // Копия 1
    for (uint8_t i = 0; i <= 5; i++)
        setFunctionModule(mod, isf, 8, i, (bits >> i) & 1u);
    setFunctionModule(mod, isf, 8, 7, (bits >> 6) & 1u);
    setFunctionModule(mod, isf, 8, 8, (bits >> 7) & 1u);
    setFunctionModule(mod, isf, 7, 8, (bits >> 8) & 1u);
    for (int8_t i = 9; i < 15; i++)
        setFunctionModule(mod, isf, (uint8_t)(14 - i), 8, (bits >> i) & 1u);

    // Копия 2
    for (int8_t i = 0; i <= 7; i++)
        setFunctionModule(mod, isf,
                          (uint8_t)(QR_SIZE - 1 - i), 8, (bits >> i) & 1u);
    for (int8_t i = 8; i < 15; i++)
        setFunctionModule(mod, isf,
                          8, (uint8_t)(QR_SIZE - 15 + i), (bits >> i) & 1u);

    // Тёмный модуль
    setFunctionModule(mod, isf, 8, (uint8_t)(QR_SIZE - 8), true);
}

static void drawFunctionPatterns(BitBucket* mod, BitBucket* isf) {
    // Синхронизирующие полосы
    for (uint8_t i = 0; i < QR_SIZE; i++) {
        setFunctionModule(mod, isf, 6, i, (i & 1u) == 0);
        setFunctionModule(mod, isf, i, 6, (i & 1u) == 0);
    }

    // 3 поисковых паттерна (по углам)
    drawFinderPattern(mod, isf,          3,          3);
    drawFinderPattern(mod, isf, QR_SIZE-4,          3);
    drawFinderPattern(mod, isf,          3, QR_SIZE-4);

    // Паттерн выравнивания для v4:
    // alignCount = 4/7+2 = 2
    // step = (4*4 + 2*2 + 1) / (2*2-2) * 2 = 21/2*2 = 20 (ceil→10*2=20? нет)
    // По алгоритму ricmoo: size=33, pos = size-7 = 26
    // alignPosition[0]=6, alignPosition[1]=26
    // Пары (i,j): (0,0) пропуск, (0,1) пропуск, (1,0) пропуск, (1,1) = (26,26)
    drawAlignmentPattern(mod, isf, 26, 26);

    // Биты формата (заглушка — перезапишем при выборе маски)
    drawFormatBits(mod, isf, QR_ECC_FORMAT, 0);
    // Биты версии не нужны: v4 < 7
}

// РАЗМЕЩЕНИЕ КОДОВЫХ СЛОВ
static void drawCodewords(BitBucket* mod, BitBucket* isf, BitBucket* cw) {
    uint32_t bitLength = cw->bitOffsetOrWidth;
    uint8_t* data      = cw->data;
    uint32_t i = 0;

    for (int16_t right = (int16_t)(QR_SIZE - 1); right >= 1; right -= 2) {
        if (right == 6) right = 5;
        for (uint8_t vert = 0; vert < QR_SIZE; vert++) {
            for (uint8_t j = 0; j < 2; j++) {
                uint8_t x       = (uint8_t)(right - j);
                bool    upwards = ((right & 2) == 0) ^ (x < 6);
                uint8_t y       = upwards ? (uint8_t)(QR_SIZE - 1 - vert) : vert;
                if (!bb_getBit(isf, x, y) && i < bitLength) {
                    bb_setBit(mod, x, y,
                              ((data[i >> 3] >> (7u - (i & 7u))) & 1u) != 0);
                    i++;
                }
            }
        }
    }
}

// МАСКИРОВАНИЕ
static void applyMask(BitBucket* mod, BitBucket* isf, uint8_t mask) {
    for (uint8_t y = 0; y < QR_SIZE; y++) {
        for (uint8_t x = 0; x < QR_SIZE; x++) {
            if (bb_getBit(isf, x, y)) continue;
            bool inv = false;
            switch (mask) {
                case 0: inv = (x + y) % 2 == 0; break;
                case 1: inv = y % 2 == 0; break;
                case 2: inv = x % 3 == 0; break;
                case 3: inv = (x + y) % 3 == 0; break;
                case 4: inv = (x / 3 + y / 2) % 2 == 0; break;
                case 5: inv = (x * y) % 2 + (x * y) % 3 == 0; break;
                case 6: inv = ((x * y) % 2 + (x * y) % 3) % 2 == 0; break;
                case 7: inv = ((x + y) % 2 + (x * y) % 3) % 2 == 0; break;
            }
            bb_invertBit(mod, x, y, inv);
        }
    }
}

// ШТРАФ
static uint32_t getPenalty(BitBucket* mod) {
    uint32_t result = 0;
    uint16_t black  = 0;

    for (uint8_t y = 0; y < QR_SIZE; y++) {
        bool colorX = bb_getBit(mod, 0, y);
        for (uint8_t x = 1, runX = 1; x < QR_SIZE; x++) {
            bool cx = bb_getBit(mod, x, y);
            if (cx != colorX) { colorX = cx; runX = 1; }
            else { runX++; if (runX == 5) result += 3; else if (runX > 5) result++; }
        }
    }
    for (uint8_t x = 0; x < QR_SIZE; x++) {
        bool colorY = bb_getBit(mod, x, 0);
        for (uint8_t y = 1, runY = 1; y < QR_SIZE; y++) {
            bool cy = bb_getBit(mod, x, y);
            if (cy != colorY) { colorY = cy; runY = 1; }
            else { runY++; if (runY == 5) result += 3; else if (runY > 5) result++; }
        }
    }

    for (uint8_t y = 0; y < QR_SIZE; y++) {
        uint16_t bitsRow = 0, bitsCol = 0;
        for (uint8_t x = 0; x < QR_SIZE; x++) {
            bool color = bb_getBit(mod, x, y);
            if (color) black++;

            if (x > 0 && y > 0) {
                bool ul = bb_getBit(mod, x-1, y-1);
                bool ur = bb_getBit(mod, x,   y-1);
                bool l  = bb_getBit(mod, x-1, y  );
                if (color == ul && color == ur && color == l) result += 3;
            }

            bitsRow = (uint16_t)(((bitsRow << 1) & 0x7FFu) | color);
            bitsCol = (uint16_t)(((bitsCol << 1) & 0x7FFu)
                                 | bb_getBit(mod, y, x));
            if (x >= 10) {
                if (bitsRow == 0x05Du || bitsRow == 0x5D0u) result += 40;
                if (bitsCol == 0x05Du || bitsCol == 0x5D0u) result += 40;
            }
        }
    }

    uint16_t total = QR_SIZE * QR_SIZE;
    for (uint16_t k = 0;
         black * 20 < (uint32_t)(9 - k) * total ||
         black * 20 > (uint32_t)(11 + k) * total;
         k++) {
        result += 10;
    }
    return result;
}

// ERROR CORRECTION для v4L (1 блок, 20 EC байт, 80 байт данных)
static void performECC(BitBucket* cw) {
    uint8_t coeff[QR_EC_CODEWORDS];
    rs_init(QR_EC_CODEWORDS, coeff);

    // 1 блок: 80 байт данных + 20 байт EC = 100 байт
    // (807 бит нужно; 100 байт = 800 бит + 7 remainder bits = нули)
    uint8_t result[QR_CW_BYTES];
    memset(result, 0, sizeof(result));

    // Данные без интерливки (1 блок)
    memcpy(result, cw->data, QR_DATA_CAPACITY);

    // EC байты сразу после данных
    rs_getRemainder(QR_EC_CODEWORDS, coeff, cw->data, QR_DATA_CAPACITY, result + QR_DATA_CAPACITY);

    memcpy(cw->data, result, QR_CW_BYTES);
    cw->bitOffsetOrWidth = QR_RAW_MODULES; // 807 бит
}

// SVG
static void writeSVG(File& f, BitBucket* mod) {
    static const uint8_t MOD_PX = 8;
    static const uint8_t QUIET  = 4;

    uint16_t totalPx = (QR_SIZE + QUIET * 2u) * MOD_PX; // (33+8)*8 = 328

    char buf[96];

    f.print(F("<?xml version=\"1.0\"?>"
              "<svg xmlns=\"http://www.w3.org/2000/svg\""));

    snprintf(buf, sizeof(buf),
             " width=\"%u\" height=\"%u\" viewBox=\"0 0 %u %u\"",
             totalPx, totalPx, totalPx, totalPx);
    f.print(buf);

    f.print(F(" shape-rendering=\"crispEdges\">"));

    snprintf(buf, sizeof(buf),
             "<rect width=\"%u\" height=\"%u\" fill=\"white\"/>",
             totalPx, totalPx);
    f.print(buf);

    for (uint8_t r = 0; r < QR_SIZE; r++) {
        for (uint8_t c = 0; c < QR_SIZE; c++) {
            if (!bb_getBit(mod, c, r)) continue;
            uint16_t px = (QUIET + c) * MOD_PX;
            uint16_t py = (QUIET + r) * MOD_PX;
            snprintf(buf, sizeof(buf),
                     "<rect x=\"%u\" y=\"%u\""
                     " width=\"%u\" height=\"%u\" fill=\"black\"/>",
                     px, py, (uint16_t)MOD_PX, (uint16_t)MOD_PX);
            f.print(buf);
        }
    }

    f.print(F("</svg>"));
}

// ТОЧКА ВХОДА
bool espChaChaGenerateQR(const uint8_t key[32], const char* svgPath) {

    // 1. Формируем HEX-строку ключа (64 символа, только 0-9, A-F)
    // Пример: "7333BDD5...1E98" — легко читается и декодируется везде
    char keyHex[QR_DATA_LEN + 1];
    for (uint8_t i = 0; i < 32u; i++) {
        // Верхний регистр — лучше сканируется и читается
        const char hex[] = "0123456789ABCDEF";
        keyHex[i * 2    ] = hex[key[i] >> 4];
        keyHex[i * 2 + 1] = hex[key[i] & 0x0Fu];
    }
    keyHex[QR_DATA_LEN] = '\0';
    // keyHex содержит 64 символа верхнего регистра, по два символа на каждый байт ключа
	// Пример: 7333BDD51E4687C410F139FD74CD2943B455731691ABDB59691F73C0C99F1E98

    // 2. Формируем поток данных QR в s_codewords
    BitBucket cwBuf;
    bb_initBuffer(&cwBuf, s_codewords, QR_CW_BYTES);

    // Индикатор режима: Byte = 0b0100
    bb_appendBits(&cwBuf, 0x4u, 4);
    // Количество символов: 8 бит (версии 1-9, режим Byte)
    bb_appendBits(&cwBuf, QR_DATA_LEN, 8);
    // Данные
    for (uint8_t i = 0; i < QR_DATA_LEN; i++) {
        bb_appendBits(&cwBuf, (uint8_t)keyHex[i], 8);
    }

    // Терминатор (до 4 нулей)
    uint32_t capBits = (uint32_t)QR_DATA_CAPACITY * 8u; // 640 бит
    uint32_t used    = cwBuf.bitOffsetOrWidth;           // 4+8+64*8 = 524 бит
    uint8_t  term    = (uint8_t)((capBits - used) >= 4u ? 4u : capBits - used);
    bb_appendBits(&cwBuf, 0u, term);

    // Выравнивание до байта
    uint8_t rem = (uint8_t)(cwBuf.bitOffsetOrWidth % 8u);
    if (rem) bb_appendBits(&cwBuf, 0u, 8u - rem);

    // Байты заполнения 0xEC / 0x11
    for (uint8_t pad = 0xEC;
         cwBuf.bitOffsetOrWidth < capBits;
         pad ^= (uint8_t)(0xEC ^ 0x11u)) {
        bb_appendBits(&cwBuf, pad, 8);
    }

    // 3. Помехоустойчивое кодирование
    performECC(&cwBuf);

    // 4. Инициализируем сетки
    BitBucket modGrid, isfGrid;
    bb_initGrid(&modGrid, s_modules, QR_SIZE);
    bb_initGrid(&isfGrid, s_isFunc,  QR_SIZE);

    // 5. Функциональные паттерны
    drawFunctionPatterns(&modGrid, &isfGrid);

    // 6. Размещаем кодовые слова (807 бит)
    drawCodewords(&modGrid, &isfGrid, &cwBuf);

    // 7. Перебираем маски
    uint8_t savedMod[QR_GRID_BYTES];
    memcpy(savedMod, s_modules, QR_GRID_BYTES);

    uint8_t  bestMask   = 0;
    uint32_t minPenalty = 0xFFFFFFFFu;

    for (uint8_t m = 0; m < 8u; m++) {
        memcpy(s_modules, savedMod, QR_GRID_BYTES);
        drawFormatBits(&modGrid, &isfGrid, QR_ECC_FORMAT, m);
        applyMask(&modGrid, &isfGrid, m);
        uint32_t pen = getPenalty(&modGrid);
        if (pen < minPenalty) { minPenalty = pen; bestMask = m; }
        yield();
    }

    // 8. Финальный рендер
    memcpy(s_modules, savedMod, QR_GRID_BYTES);
    drawFormatBits(&modGrid, &isfGrid, QR_ECC_FORMAT, bestMask);
    applyMask(&modGrid, &isfGrid, bestMask);

    // 9. Пишем SVG
    File f = LittleFS.open(svgPath, "w");
    if (!f) {
        Serial.println(F("[ChaCha QR] ERROR: не могу создать SVG!"));
        return false;
    }
    writeSVG(f, &modGrid);
    f.close();

    Serial.print(F("[ChaCha QR] OK v4L HEX mask="));
    Serial.println(bestMask);
    return true;
}
