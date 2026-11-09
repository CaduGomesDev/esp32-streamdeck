#include "Screensaver.h"

#include <AnimatedGIF.h>
#include <LittleFS.h>

namespace {

AnimatedGIF gif;
Arduino_Canvas *gCanvas = nullptr;
File gFile;
int16_t gWidth = 0, gHeight = 0;
bool gOpen = false;

void *gifOpen(const char *fname, int32_t *pSize) {
    gFile = LittleFS.open(fname, "r");
    if (!gFile) return nullptr;
    *pSize = gFile.size();
    return &gFile;
}

void gifClose(void *handle) {
    File *f = static_cast<File *>(handle);
    if (f) f->close();
}

int32_t gifRead(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen) {
    File *f = static_cast<File *>(pFile->fHandle);
    int32_t avail = pFile->iSize - pFile->iPos;
    int32_t toRead = iLen < avail ? iLen : avail;
    if (toRead <= 0) return 0;
    toRead = (int32_t)f->read(pBuf, toRead);
    pFile->iPos = f->position();
    return toRead;
}

int32_t gifSeek(GIFFILE *pFile, int32_t iPosition) {
    File *f = static_cast<File *>(pFile->fHandle);
    f->seek(iPosition);
    pFile->iPos = (int32_t)f->position();
    return pFile->iPos;
}

void gifDraw(GIFDRAW *pDraw) {
    if (!gCanvas) return;

    uint16_t usTemp[480];
    int iWidth = pDraw->iWidth;
    if (iWidth + pDraw->iX > gWidth) iWidth = gWidth - pDraw->iX;

    uint16_t *usPalette = pDraw->pPalette;
    int y = pDraw->iY + pDraw->y;
    if (y >= gHeight || pDraw->iX >= gWidth || iWidth < 1) return;

    uint8_t *s = pDraw->pPixels;
    if (pDraw->ucDisposalMethod == 2) {
        for (int x = 0; x < iWidth; x++) {
            if (s[x] == pDraw->ucTransparent) s[x] = pDraw->ucBackground;
        }
        pDraw->ucHasTransparency = 0;
    }

    if (pDraw->ucHasTransparency) {
        uint8_t *pEnd = s + iWidth;
        uint8_t ucTransparent = pDraw->ucTransparent;
        int x = 0, iCount = 0;
        while (x < iWidth) {
            uint8_t c = ucTransparent - 1;
            uint16_t *d = usTemp;
            while (c != ucTransparent && s < pEnd) {
                c = *s++;
                if (c == ucTransparent) {
                    s--;
                } else {
                    *d++ = usPalette[c];
                    iCount++;
                }
            }
            if (iCount) {
                gCanvas->draw16bitBeRGBBitmap(pDraw->iX + x, y, usTemp, iCount, 1);
                x += iCount;
                iCount = 0;
            }
            c = ucTransparent;
            while (c == ucTransparent && s < pEnd) {
                c = *s++;
                if (c == ucTransparent) iCount++; else s--;
            }
            if (iCount) {
                x += iCount;
                iCount = 0;
            }
        }
    } else {
        s = pDraw->pPixels;
        for (int x = 0; x < iWidth; x++) usTemp[x] = usPalette[*s++];
        gCanvas->draw16bitBeRGBBitmap(pDraw->iX, y, usTemp, iWidth, 1);
    }
}

} // namespace

namespace Screensaver {

void begin(Arduino_Canvas *canvas) {
    gCanvas = canvas;
    gWidth = canvas->width();
    gHeight = canvas->height();
    gif.begin(BIG_ENDIAN_PIXELS);
}

bool hasGif() {
    return LittleFS.exists(GIF_PATH);
}

bool start() {
    stop();
    if (!hasGif()) return false;
    gOpen = gif.open(GIF_PATH, gifOpen, gifClose, gifRead, gifSeek, gifDraw);
    return gOpen;
}

void stop() {
    if (gOpen) {
        gif.close();
        gOpen = false;
    }
}

bool step() {
    if (!gOpen) return false;
    int more = gif.playFrame(true, nullptr);
    gCanvas->flush();
    if (!more) {
        stop();
        return false;
    }
    return true;
}

} // namespace Screensaver
