#include "Background.h"

#include <LittleFS.h>

namespace {

uint16_t *gPixels = nullptr;

void release() {
    if (gPixels) {
        free(gPixels);
        gPixels = nullptr;
    }
}

} // namespace

namespace Background {

void reload() {
    release();

    File f = LittleFS.open(BG_PATH, "r");
    if (!f) return;
    if (f.size() != (int)BG_BYTES) {
        f.close();
        return;
    }

    gPixels = (uint16_t *)ps_malloc(BG_BYTES);
    if (!gPixels) {
        f.close();
        return;
    }
    if (f.read((uint8_t *)gPixels, BG_BYTES) != (int)BG_BYTES) release();
    f.close();
}

const uint16_t *pixels() { return gPixels; }

} // namespace Background
