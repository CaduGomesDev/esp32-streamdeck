/**
 * JC3248W535 Touch Driver
 * Minimal driver for AXS15231B I2C touch controller
 *
 * Source: https://github.com/me-processware/JC3248W535-Driver (MIT)
 */

#ifndef JC3248W535_TOUCH_H
#define JC3248W535_TOUCH_H

#include <Arduino.h>
#include <Wire.h>

// Touch hardware configuration
#define JC3248_TOUCH_SDA   4
#define JC3248_TOUCH_SCL   8
#define JC3248_TOUCH_INT   3
#define JC3248_TOUCH_ADDR  0x3B

struct TouchPoint {
    uint16_t x;
    uint16_t y;
    bool touched;
};

class JC3248W535_Touch {
public:
    JC3248W535_Touch();

    bool begin();
    void setRotation(uint8_t rotation, int16_t logicalWidth, int16_t logicalHeight);
    bool read(TouchPoint &point);
    bool isTouched();
    TouchPoint getLastTouch() { return lastTouch; }

private:
    TouchPoint lastTouch;
    uint8_t currentRotation = 0;
    int16_t logicalWidth = 320;
    int16_t logicalHeight = 480;

    static const uint8_t AXS_READ_TOUCHPAD[11];
};

#endif // JC3248W535_TOUCH_H
