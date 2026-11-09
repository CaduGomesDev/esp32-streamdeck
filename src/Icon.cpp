#include "Icon.h"

#include <LittleFS.h>

#include "Config.h"

namespace Icon {

bool exists(uint8_t index) {
    File f = LittleFS.open(Config::iconPath(index), "r");
    if (!f) return false;
    bool ok = f.size() == BYTES;
    f.close();
    return ok;
}

bool draw(Arduino_Canvas *canvas, uint8_t index, int16_t x, int16_t y, uint16_t bgColor565) {
    File f = LittleFS.open(Config::iconPath(index), "r");
    if (!f) return false;
    if (f.size() != (int)BYTES) {
        f.close();
        return false;
    }

    uint8_t bgR = bgColor565 >> 11 << 3;
    uint8_t bgG = (bgColor565 >> 5 & 0x3F) << 2;
    uint8_t bgB = (bgColor565 & 0x1F) << 3;

    uint8_t rowBuf[SIZE * 4];
    uint16_t line[SIZE];

    for (int row = 0; row < SIZE; row++) {
        if (f.read(rowBuf, sizeof(rowBuf)) != (int)sizeof(rowBuf)) {
            f.close();
            return false;
        }
        for (int col = 0; col < SIZE; col++) {
            uint8_t r = rowBuf[col * 4 + 0];
            uint8_t g = rowBuf[col * 4 + 1];
            uint8_t b = rowBuf[col * 4 + 2];
            uint16_t a = rowBuf[col * 4 + 3];

            uint8_t outR = (uint16_t(r) * a + uint16_t(bgR) * (255 - a)) / 255;
            uint8_t outG = (uint16_t(g) * a + uint16_t(bgG) * (255 - a)) / 255;
            uint8_t outB = (uint16_t(b) * a + uint16_t(bgB) * (255 - a)) / 255;

            line[col] = (uint16_t)(((outR & 0xF8) << 8) | ((outG & 0xFC) << 3) | (outB >> 3));
        }
        canvas->draw16bitRGBBitmap(x, y + row, line, SIZE, 1);
    }

    f.close();
    return true;
}

bool blendInto(uint16_t *buf, int bufW, int bufH, uint8_t index, int x, int y) {
    File f = LittleFS.open(Config::iconPath(index), "r");
    if (!f) return false;
    if (f.size() != (int)BYTES) {
        f.close();
        return false;
    }

    uint8_t rowBuf[SIZE * 4];
    for (int row = 0; row < SIZE; row++) {
        if (f.read(rowBuf, sizeof(rowBuf)) != (int)sizeof(rowBuf)) {
            f.close();
            return false;
        }
        int py = y + row;
        if (py < 0 || py >= bufH) continue;
        for (int col = 0; col < SIZE; col++) {
            int px = x + col;
            if (px < 0 || px >= bufW) continue;

            uint16_t a = rowBuf[col * 4 + 3];
            if (a == 0) continue;
            uint16_t &dst = buf[py * bufW + px];
            uint8_t bgR = dst >> 11 << 3;
            uint8_t bgG = (dst >> 5 & 0x3F) << 2;
            uint8_t bgB = (dst & 0x1F) << 3;

            uint8_t outR = (uint16_t(rowBuf[col * 4 + 0]) * a + uint16_t(bgR) * (255 - a)) / 255;
            uint8_t outG = (uint16_t(rowBuf[col * 4 + 1]) * a + uint16_t(bgG) * (255 - a)) / 255;
            uint8_t outB = (uint16_t(rowBuf[col * 4 + 2]) * a + uint16_t(bgB) * (255 - a)) / 255;
            dst = (uint16_t)(((outR & 0xF8) << 8) | ((outG & 0xFC) << 3) | (outB >> 3));
        }
    }
    f.close();
    return true;
}

} // namespace Icon
