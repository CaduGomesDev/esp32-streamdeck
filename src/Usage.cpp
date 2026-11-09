#include "Usage.h"

#include <LittleFS.h>

#include "Mascot.h"

namespace {

constexpr int WINDOWS = 2; // 5 horas, 7 dias
constexpr unsigned long STALE_MS = 90000; // sem dado novo do PC por esse tempo = aviso

struct Window {
    char label[16];
    bool hasPercent;
    float percent;
    bool hasReset;
    uint32_t resetSec; // segundos ate reiniciar, no momento do recebimento
};

Window gWin[WINDOWS];
bool gHasData = false;
char gError[48] = "";
char gWarn[96] = "";
int32_t gAgeSec = -1; // idade do dado no PC, no momento do recebimento (-1 = desconhecida)
unsigned long gLastUpdateMs = 0;

// Fundo (opcional) ja com os cards translucidos aplicados; nullptr = sem fundo.
uint16_t *gBase = nullptr;

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Tema claro (creme), combinando com a identidade visual do Claude.
const uint16_t C_BG = rgb(227, 218, 203);
const uint16_t C_GLASS = rgb(255, 255, 255);       // cor do "vidro" dos cards
const uint8_t GLASS_ALPHA = 110;                   // 0-255: quanto do vidro cobre o fundo
const uint16_t C_GLASS_SOLID = rgb(241, 235, 225); // vidro quando nao ha imagem de fundo
const uint16_t C_BORDER = rgb(204, 192, 173);
const uint16_t C_SHADOW = rgb(205, 194, 176);
const uint16_t C_TRACK = rgb(214, 204, 188);
const uint16_t C_TEXT = rgb(38, 32, 28);
const uint16_t C_DIM = rgb(122, 110, 96);
const uint16_t C_EYE = rgb(38, 32, 28);
const uint16_t C_ACCENT = rgb(217, 119, 87);
const uint16_t C_AMBER = rgb(204, 132, 18);
const uint16_t C_RED = rgb(200, 52, 66);

// Geometria dos cards. Fixa, pra a translucidez poder ser aplicada uma vez no
// fundo. Os cards ficam perto (vao de 14 px), no meio da tela (y 75-245 de 320).
constexpr int CARD_X = 8, CARD_H = 78, CARD_R = 10;
constexpr int CARD_Y[WINDOWS] = {75, 167};

inline uint16_t blend565(uint16_t bg, uint16_t fg, uint8_t a) {
    uint32_t r = (((fg >> 11) & 0x1F) * a + ((bg >> 11) & 0x1F) * (255 - a)) / 255;
    uint32_t g = (((fg >> 5) & 0x3F) * a + ((bg >> 5) & 0x3F) * (255 - a)) / 255;
    uint32_t b = ((fg & 0x1F) * a + (bg & 0x1F) * (255 - a)) / 255;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

// A fonte padrao do Arduino_GFX so tem ASCII: descarta o resto (a ponte do PC
// ja remove acentos, isto e so uma rede de seguranca).
void copyAscii(char *dst, size_t n, const char *src) {
    size_t j = 0;
    for (size_t i = 0; src && src[i] && j < n - 1; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c >= 32 && c < 127) dst[j++] = (char)c;
    }
    dst[j] = '\0';
}

int textW(const char *s, int size) { return (int)strlen(s) * 6 * size; }

void text(Arduino_Canvas *cv, int x, int y, int size, uint16_t color, const char *s) {
    cv->setTextSize(size);
    cv->setTextColor(color);
    cv->setCursor(x, y);
    cv->print(s);
}

void textRight(Arduino_Canvas *cv, int right, int y, int size, uint16_t color, const char *s) {
    text(cv, right - textW(s, size), y, size, color, s);
}

void fmtDuration(uint32_t sec, char *out, size_t n) {
    uint32_t d = sec / 86400, h = (sec % 86400) / 3600, m = (sec % 3600) / 60;
    if (d > 0) snprintf(out, n, "%ud %uh", (unsigned)d, (unsigned)h);
    else if (h > 0) snprintf(out, n, "%uh %um", (unsigned)h, (unsigned)m);
    else snprintf(out, n, "%um", (unsigned)m);
}

uint16_t levelColor(float pct) {
    if (pct >= 90) return C_RED;
    if (pct >= 70) return C_AMBER;
    return C_ACCENT;
}

void drawBar(Arduino_Canvas *cv, int x, int y, int w, int h, bool has, float pct) {
    cv->fillRoundRect(x, y, w, h, h / 2, C_TRACK);
    if (!has) return;
    if (pct > 100) pct = 100;
    if (pct < 0) pct = 0;
    int fw = (int)(w * pct / 100);
    if (fw < h && pct > 0) fw = h; // barra minima visivel
    if (fw > 0) cv->fillRoundRect(x, y, fw, h, h / 2, levelColor(pct));
}

// Aplica o "vidro" translucido dos cards no fundo (uma vez, ao carregar).
void applyGlass(uint16_t *px) {
    const int w = USAGE_BG_W - 2 * CARD_X;
    for (int i = 0; i < WINDOWS; i++) {
        for (int y = 0; y < CARD_H; y++) {
            // distancia vertical ao centro do canto (em meios-pixels), 0 fora dos cantos
            int dy2 = y < CARD_R ? 2 * (CARD_R - y) - 1 : (y >= CARD_H - CARD_R ? 2 * (y - (CARD_H - CARD_R)) + 1 : 0);
            for (int x = 0; x < w; x++) {
                int dx2 = x < CARD_R ? 2 * (CARD_R - x) - 1 : (x >= w - CARD_R ? 2 * (x - (w - CARD_R)) + 1 : 0);
                if (dx2 * dx2 + dy2 * dy2 > 4 * CARD_R * CARD_R) continue;
                uint16_t &p = px[(CARD_Y[i] + y) * USAGE_BG_W + CARD_X + x];
                p = blend565(p, C_GLASS, GLASS_ALPHA);
            }
        }
    }
}

void releaseBase() {
    if (gBase) {
        free(gBase);
        gBase = nullptr;
    }
}

void drawWindow(Arduino_Canvas *cv, const Window &win, int idx, int cw, unsigned long elapsedSec) {
    const int cx = CARD_X, cy = CARD_Y[idx], ch = CARD_H;
    // Com fundo, o vidro ja esta no bitmap; sem fundo, desenha um vidro opaco claro.
    if (!gBase) cv->fillRoundRect(cx, cy, cw, ch, CARD_R, C_GLASS_SOLID);
    cv->drawRoundRect(cx, cy, cw, ch, CARD_R, C_BORDER);

    // Barrinha lateral na cor do nivel (laranja / ambar / vermelho)
    uint16_t lc = win.hasPercent ? levelColor(win.percent) : C_TRACK;
    cv->fillRoundRect(cx + 9, cy + 12, 5, ch - 24, 2, lc);

    int x = cx + 24, w = cw - 24 - 16;
    text(cv, x, cy + 8, 2, C_TEXT, win.label);

    char val[8];
    if (win.hasPercent) snprintf(val, sizeof(val), "%d%%", (int)(win.percent + 0.5f));
    else snprintf(val, sizeof(val), "--");
    textRight(cv, x + w, cy + 6, 4, win.hasPercent ? lc : C_DIM, val);

    if (win.hasReset) {
        char line[40];
        if (win.resetSec > elapsedSec) {
            char dur[16];
            fmtDuration(win.resetSec - elapsedSec, dur, sizeof(dur));
            snprintf(line, sizeof(line), "reinicia em %s", dur);
        } else {
            snprintf(line, sizeof(line), "janela reiniciada");
        }
        text(cv, x, cy + 28, 2, C_DIM, line);
    }

    drawBar(cv, x, cy + ch - 24, w, 12, win.hasPercent, win.percent);
}

constexpr int MASCOT_SCALE = 3;
const Mascot::Colors MASCOT_COLORS = {C_ACCENT, rgb(168, 86, 62), C_EYE, C_SHADOW};

// Retangulo que o mascote (e a sombra) ocupa, pra animar sem redesenhar a tela
// toda: so aqui o quadro muda entre uma atualizacao e outra.
constexpr int MASCOT_CX = 32, MASCOT_GROUND_Y = 38; // centro do mascote parado (alinhado ao titulo)
constexpr int MASCOT_HALF_W = Mascot::CELLS_W * MASCOT_SCALE / 2 + 2 * MASCOT_SCALE; // com o giro
constexpr int MASCOT_AREA_X = MASCOT_CX - MASCOT_HALF_W, MASCOT_AREA_W = 2 * MASCOT_HALF_W;
constexpr int MASCOT_AREA_Y = MASCOT_GROUND_Y - Mascot::CELLS_H * MASCOT_SCALE - 2;
constexpr int MASCOT_AREA_H = Mascot::CELLS_H * MASCOT_SCALE + 2 + 6; // ate a sombra

// Tela inteira precisa ser redesenhada (dado novo, fundo novo, tela recem
// aberta) e o que estava na tela no ultimo desenho completo.
bool gFullDirty = true;
uint32_t gDrawnSignature = 0;

// Muda quando algum texto da tela muda sozinho: a contagem regressiva e a
// idade do dado andam em minutos, e o aviso de "sem atualizar" aparece depois
// de um tempo.
uint32_t textSignature(unsigned long now) {
    const unsigned long elapsedSec = (now - gLastUpdateMs) / 1000;
    uint32_t sig = (uint32_t)(now - gLastUpdateMs > STALE_MS);
    sig = sig * 31 + (uint32_t)((gAgeSec >= 0 ? (uint32_t)gAgeSec + elapsedSec : 0) / 60);
    for (int i = 0; i < WINDOWS; i++) {
        const Window &w = gWin[i];
        sig = sig * 31 + (w.resetSec > elapsedSec ? (uint32_t)((w.resetSec - elapsedSec) / 60) + 1 : 0);
    }
    return sig;
}

} // namespace

namespace Usage {

void invalidate() {
    gFullDirty = true;
}

// Fundo comprimido (RLE): sequencia de (n, cor RGB565 little-endian), 3 bytes
// por trecho, n = 1..255 pixels seguidos da mesma cor, em linhas de 480.
void reloadBackground() {
    releaseBase();
    gFullDirty = true;

    File f = LittleFS.open(USAGE_BG_PATH, "r");
    if (!f) return;
    size_t size = f.size();
    if (size == 0 || size % 3 != 0 || size > USAGE_BG_MAX_BYTES) {
        f.close();
        return;
    }

    uint8_t *raw = (uint8_t *)ps_malloc(size);
    uint16_t *px = (uint16_t *)ps_malloc(USAGE_BG_BYTES);
    if (!raw || !px || f.read(raw, size) != (int)size) {
        f.close();
        free(raw);
        free(px);
        return;
    }
    f.close();

    size_t n = 0;
    bool ok = true;
    for (size_t i = 0; i < size && ok; i += 3) {
        size_t run = raw[i];
        uint16_t color = (uint16_t)(raw[i + 1] | (raw[i + 2] << 8));
        if (run == 0 || n + run > USAGE_BG_PIXELS) {
            ok = false;
            break;
        }
        for (size_t k = 0; k < run; k++) px[n++] = color;
    }
    free(raw);
    if (!ok || n != USAGE_BG_PIXELS) {
        free(px);
        return;
    }

    applyGlass(px);
    gBase = px;
}

void update(JsonDocument &doc) {
    gFullDirty = true;
    const char *err = doc["error"] | (const char *)nullptr;
    if (err) {
        copyAscii(gError, sizeof(gError), err);
        return; // mantem o ultimo dado bom na tela
    }
    gError[0] = '\0';

    JsonArrayConst arr = doc["w"].as<JsonArrayConst>();
    int i = 0;
    for (JsonObjectConst o : arr) {
        if (i >= WINDOWS) break;
        Window &w = gWin[i++];
        copyAscii(w.label, sizeof(w.label), o["l"] | "?");
        w.hasPercent = !o["p"].isNull();
        w.percent = o["p"] | 0.0f;
        w.hasReset = !o["r"].isNull();
        w.resetSec = o["r"] | 0;
    }
    for (; i < WINDOWS; i++) gWin[i] = Window{};

    gAgeSec = doc["a"].isNull() ? -1 : (int32_t)(doc["a"] | 0);
    copyAscii(gWarn, sizeof(gWarn), doc["warn"] | "");
    gHasData = true;
    gLastUpdateMs = millis();
}

void draw(Arduino_Canvas *cv) {
    const int W = cv->width(), H = cv->height();
    const unsigned long now = millis();

    if (gBase && W == USAGE_BG_W && H == USAGE_BG_H) cv->draw16bitRGBBitmap(0, 0, gBase, W, H);
    else cv->fillScreen(C_BG);

    // Topo: mascote pulando + titulo
    Mascot::draw(cv, MASCOT_CX, MASCOT_GROUND_Y, MASCOT_SCALE, now, MASCOT_COLORS);
    text(cv, 66, 8, 3, C_TEXT, "CLAUDE");
    text(cv, 66, 34, 1, C_DIM, "uso da assinatura");

    const unsigned long elapsedSec = (now - gLastUpdateMs) / 1000;

    // Cabecalho (direita): erro do PC > dado do PC velho > idade do dado
    const char *hdr = "";
    uint16_t hc = C_DIM;
    char ageBuf[32];
    if (gError[0]) {
        hdr = gError;
        hc = C_RED;
    } else if (gHasData && now - gLastUpdateMs > STALE_MS) {
        hdr = "SEM ATUALIZAR";
        hc = C_AMBER;
    } else if (gHasData && gAgeSec >= 0) {
        char dur[16];
        fmtDuration((uint32_t)gAgeSec + elapsedSec, dur, sizeof(dur));
        snprintf(ageBuf, sizeof(ageBuf), "atualizado ha %s", dur);
        hdr = ageBuf;
    }
    textRight(cv, W - 12, 14, 1, hc, hdr);

    if (!gHasData) {
        const char *l1 = gError[0] ? "Nao foi possivel obter os dados" : "Aguardando dados do PC...";
        text(cv, (W - textW(l1, 2)) / 2, H / 2 - 20, 2, C_TEXT, l1);
        const char *l2 = "Rode tools/bridge.ps1 no PC";
        text(cv, (W - textW(l2, 1)) / 2, H / 2 + 10, 1, C_DIM, l2);
    } else {
        for (int i = 0; i < WINDOWS; i++) drawWindow(cv, gWin[i], i, W - 2 * CARD_X, elapsedSec);
        if (gWarn[0]) {
            char buf[80];
            strncpy(buf, gWarn, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            text(cv, CARD_X, CARD_Y[1] + CARD_H + 8, 1, C_AMBER, buf);
        }
    }

    const char *foot = "toque na tela para voltar";
    text(cv, (W - textW(foot, 1)) / 2, H - 11, 1, C_DIM, foot);

    gFullDirty = false;
    gDrawnSignature = textSignature(now);
}

void drawFrame(Arduino_Canvas *cv) {
    const unsigned long now = millis();
    if (gFullDirty || textSignature(now) != gDrawnSignature) {
        draw(cv);
        return;
    }

    // So o mascote muda: devolve o fundo atras dele e desenha o novo quadro.
    const int W = cv->width(), H = cv->height();
    if (gBase && W == USAGE_BG_W && H == USAGE_BG_H) {
        for (int y = MASCOT_AREA_Y; y < MASCOT_AREA_Y + MASCOT_AREA_H; y++) {
            cv->draw16bitRGBBitmap(MASCOT_AREA_X, y, gBase + y * USAGE_BG_W + MASCOT_AREA_X, MASCOT_AREA_W, 1);
        }
    } else {
        cv->fillRect(MASCOT_AREA_X, MASCOT_AREA_Y, MASCOT_AREA_W, MASCOT_AREA_H, C_BG);
    }
    Mascot::draw(cv, MASCOT_CX, MASCOT_GROUND_Y, MASCOT_SCALE, now, MASCOT_COLORS);
}

} // namespace Usage
