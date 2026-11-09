#include "Notification.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <strings.h>

#include <mbedtls/base64.h>

namespace {

constexpr int SCREEN_W = 480;
constexpr int SCREEN_H = 320;
constexpr int HEADER_H = 48;
constexpr int ROW_H = 66;
constexpr int LIST_PAD_BOTTOM = 8;

constexpr int MAX_NOTES = 20;
constexpr int MAX_ICONS = 8;     // apps distintos com icone guardado
constexpr int ICON_SIZE = 32;    // lado da mascara que o notify-bridge.ps1 manda
constexpr int TILE = 44;         // quadrado escuro atras do icone
constexpr int WRAP_LINE_MAX = 80;
constexpr int TITLE_CHARS = 28;  // tamanho 2 (12 px por letra) ate o horario
constexpr int BODY_CHARS = 62;   // tamanho 1 (6 px por letra), 2 linhas

// Tela de detalhe (mensagem completa): texto em tamanho 2, rolavel.
constexpr int DETAIL_PITCH = 20;
constexpr int DETAIL_TITLE_CHARS = 33; // do texto ao lado do icone ate a borda
constexpr int DETAIL_TITLE_LINES = 3;
constexpr int DETAIL_BODY_CHARS = 37;  // largura toda da tela
constexpr int DETAIL_BODY_LINES = 28;

constexpr int TEXT_X = 70;
constexpr int PILL_W = 162, PILL_H = 26;
constexpr int PILL_X = SCREEN_W - 14 - PILL_W, PILL_Y = 11;

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Tema escuro minimalista: fundo quase preto, titulo branco, descricao em
// cinza suave e um unico acento (o ponto de "nao lida").
const uint16_t C_BG = rgb(9, 11, 15);
const uint16_t C_TILE = rgb(25, 28, 35);
const uint16_t C_HOVER = rgb(17, 20, 26);
const uint16_t C_SEP = rgb(22, 25, 31);
const uint16_t C_WHITE = rgb(255, 255, 255);
const uint16_t C_TITLE_READ = rgb(190, 196, 208);
const uint16_t C_DESC = rgb(139, 147, 161);
const uint16_t C_TIME = rgb(98, 106, 120);
const uint16_t C_DOT = rgb(96, 165, 250);
const uint16_t C_PILL = rgb(26, 30, 38);
const uint16_t C_PILL_PRESSED = rgb(38, 43, 54);
const uint16_t C_PILL_TEXT = rgb(205, 211, 222);
const uint16_t C_SCROLL = rgb(52, 58, 70);

struct Note {
    char app[24];
    char title[96];
    char body[512];
    unsigned long at; // millis() de quando chegou
    bool read;
    int8_t icon; // indice em gIcons, -1 = sem mascara
};

struct AppIcon {
    char app[24];
    uint8_t mask[ICON_SIZE * ICON_SIZE];
};

Note gNotes[MAX_NOTES];
int gCount = 0;

AppIcon gIcons[MAX_ICONS];
int gIconCount = 0;
int gIconNext = 0;
uint8_t gPending[ICON_SIZE * ICON_SIZE];
bool gHasPending = false;

float gScroll = 0, gVel = 0;   // rolagem da lista
float gDScroll = 0, gDVel = 0; // rolagem da tela de detalhe
int gDetail = -1;              // indice da notificacao aberta, -1 = lista
bool gDirty = true;
unsigned long gLastTick = 0, gLastLabelRefresh = 0;

// toque
bool gDown = false, gMoved = false;
int gStartY = 0, gLastY = 0;
uint8_t gUpStreak = 0;
int gPressRow = -1;
int gPressHdr = 0; // 0 nada, 1 voltar, 2 marcar todas

// A fonte padrao do Arduino_GFX so tem ASCII: descarta o resto (o script do
// PC ja remove acentos, isto e so uma rede de seguranca).
void copyAscii(char *dst, size_t n, const char *src) {
    size_t j = 0;
    for (size_t i = 0; src && src[i] && j < n - 1; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c >= 32 && c < 127) dst[j++] = (char)c;
    }
    dst[j] = '\0';
}

int textW(const char *s, int size) { return (int)strlen(s) * 6 * size; }

void text(Arduino_Canvas *cv, int x, int y, int size, uint16_t color, const char *s, bool bold = false) {
    cv->setTextSize(size);
    cv->setTextColor(color);
    cv->setCursor(x, y);
    cv->print(s);
    if (bold) {
        cv->setCursor(x + 1, y);
        cv->print(s);
    }
}

inline uint16_t blend565(uint16_t bg, uint16_t fg, uint8_t a) {
    uint32_t r = (((fg >> 11) & 0x1F) * a + ((bg >> 11) & 0x1F) * (255 - a)) / 255;
    uint32_t g = (((fg >> 5) & 0x3F) * a + ((bg >> 5) & 0x3F) * (255 - a)) / 255;
    uint32_t b = ((fg & 0x1F) * a + (bg & 0x1F) * (255 - a)) / 255;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

// Copia `s` cortando em maxChars e trocando o fim por "..." se passou.
void clipText(char *out, size_t outN, const char *s, int maxChars) {
    int len = (int)strlen(s);
    if (len <= maxChars) {
        snprintf(out, outN, "%s", s);
    } else {
        snprintf(out, outN, "%.*s...", maxChars - 3, s);
    }
}

// Quebra `s` em linhas de ate maxChars caracteres (cortando num espaco quando
// da, pra nao partir palavra no meio), no maximo maxLines; a ultima ganha
// reticencias se sobrou texto.
int wrapText(const char *s, int maxChars, char out[][WRAP_LINE_MAX], int maxLines) {
    if (maxChars > WRAP_LINE_MAX - 1) maxChars = WRAP_LINE_MAX - 1;
    int lines = 0;
    int len = (int)strlen(s);
    int pos = 0;
    while (pos < len && s[pos] == ' ') pos++;
    while (pos < len && lines < maxLines) {
        int remaining = len - pos;
        int take = remaining < maxChars ? remaining : maxChars;
        if (take == maxChars && pos + take < len) {
            int cut = take;
            while (cut > 0 && s[pos + cut] != ' ') cut--;
            if (cut > 0) take = cut;
        }
        strncpy(out[lines], s + pos, take);
        out[lines][take] = '\0';
        for (int k = take - 1; k >= 0 && out[lines][k] == ' '; k--) out[lines][k] = '\0';
        lines++;
        pos += take;
        while (pos < len && s[pos] == ' ') pos++;
    }
    if (pos < len && lines > 0) {
        char *last = out[lines - 1];
        int l = (int)strlen(last);
        if (l + 3 > maxChars) l = maxChars - 3;
        if (l < 0) l = 0;
        last[l] = last[l + 1] = last[l + 2] = '.';
        last[l + 3] = '\0';
    }
    return lines;
}

// "agora", "5 min", "2 h", "3 d" (a placa nao tem relogio: conta desde que
// a notificacao chegou).
void timeLabel(char *out, size_t n, unsigned long at) {
    unsigned long sec = (millis() - at) / 1000;
    if (sec < 60) snprintf(out, n, "agora");
    else if (sec < 3600) snprintf(out, n, "%lu min", sec / 60);
    else if (sec < 86400) snprintf(out, n, "%lu h", sec / 3600);
    else snprintf(out, n, "%lu d", sec / 86400);
}

// ---- icones brancos -------------------------------------------------------
//
// Telegram (e Unigram): o logo oficial (aviao de papel num disco) todo
// branco, com o aviao vazado, guardado como mascara 32x32 (renderizada do
// SVG oficial). Os demais sao tracos no estilo Lucide (bell, mail,
// message-square), desenhados numa grade de 24x24 e escalados. Apps com
// icone proprio usam a mascara branca que o PC manda.

enum class Kind { TELEGRAM, MAIL, CHAT, BELL };

struct Pt {
    float x, y;
};

struct Glyph {
    Arduino_Canvas *cv;
    float ox, oy, s;
    int X(float v) const { return (int)lroundf(ox + v * s); }
    int Y(float v) const { return (int)lroundf(oy + v * s); }
};

void line2(Arduino_Canvas *cv, int x0, int y0, int x1, int y1, uint16_t c) {
    cv->drawLine(x0, y0, x1, y1, c);
    cv->drawLine(x0 + 1, y0, x1 + 1, y1, c);
    cv->drawLine(x0, y0 + 1, x1, y1 + 1, c);
}

void polyline(const Glyph &g, const Pt *p, int n, uint16_t c) {
    for (int i = 0; i + 1 < n; i++) line2(g.cv, g.X(p[i].x), g.Y(p[i].y), g.X(p[i + 1].x), g.Y(p[i + 1].y), c);
}

const Pt BELL_BODY[] = {{18, 8}, {18.6, 13}, {21, 17}, {3, 17}, {5.4, 13}, {6, 8}};
const Pt BELL_CLAPPER[] = {{10.3, 20.4}, {11, 21.5}, {12, 21.9}, {13, 21.5}, {13.7, 20.4}};
const Pt MAIL_FLAP[] = {{2.5, 7}, {12, 13.3}, {21.5, 7}};
const Pt CHAT_BOX[] = {{5, 3}, {19, 3}, {21, 5}, {21, 15}, {19, 17}, {7, 17}, {3, 21}, {3, 5}, {5, 3}};

// Logo do Telegram: 0 = fundo, 255 = branco.
const uint8_t TELEGRAM_MASK[ICON_SIZE * ICON_SIZE] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 10, 81, 147, 194, 227, 250, 250, 226, 197, 147, 82, 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 29, 146, 241, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 244, 151, 34, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 5, 128, 247, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 249, 136, 8, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 28, 203, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 209, 35, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 40, 230, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 232, 47, 0, 0, 0, 0,
    0, 0, 0, 27, 229, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 233, 38, 0, 0, 0,
    0, 0, 5, 201, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 212, 10, 0, 0,
    0, 0, 126, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 143, 0, 0,
    0, 28, 247, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 251, 39, 0,
    0, 145, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 227, 174, 243, 255, 255, 255, 255, 255, 255, 161, 0,
    9, 241, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 248, 165, 66, 1, 0, 145, 255, 255, 255, 255, 255, 255, 248, 17,
    79, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 224, 117, 20, 0, 0, 0, 0, 158, 255, 255, 255, 255, 255, 255, 255, 95,
    145, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 185, 78, 3, 0, 0, 0, 0, 0, 0, 190, 255, 255, 255, 255, 255, 255, 255, 161,
    195, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 244, 150, 41, 0, 0, 0, 11, 136, 20, 0, 0, 0, 227, 255, 255, 255, 255, 255, 255, 255, 210,
    225, 255, 255, 255, 255, 255, 255, 255, 255, 223, 116, 16, 0, 0, 0, 0, 66, 220, 93, 0, 0, 0, 10, 255, 255, 255, 255, 255, 255, 255, 255, 244,
    248, 255, 255, 255, 255, 255, 255, 194, 82, 3, 0, 0, 0, 0, 12, 149, 251, 86, 0, 0, 0, 0, 49, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    249, 255, 255, 255, 255, 255, 81, 0, 0, 0, 0, 0, 0, 63, 223, 247, 81, 0, 0, 0, 0, 0, 86, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    226, 255, 255, 255, 255, 255, 178, 76, 9, 0, 0, 10, 145, 254, 243, 62, 0, 0, 0, 0, 0, 0, 130, 255, 255, 255, 255, 255, 255, 255, 255, 246,
    198, 255, 255, 255, 255, 255, 255, 255, 245, 187, 164, 230, 255, 244, 60, 0, 0, 0, 0, 0, 0, 0, 173, 255, 255, 255, 255, 255, 255, 255, 255, 212,
    148, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 175, 0, 0, 0, 0, 0, 0, 0, 0, 215, 255, 255, 255, 255, 255, 255, 255, 255, 165,
    84, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 254, 131, 5, 0, 0, 0, 0, 0, 9, 254, 255, 255, 255, 255, 255, 255, 255, 255, 99,
    11, 244, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 206, 46, 0, 0, 0, 0, 51, 255, 255, 255, 255, 255, 255, 255, 255, 250, 21,
    0, 152, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 249, 122, 4, 0, 0, 101, 255, 255, 255, 255, 255, 255, 255, 255, 169, 0,
    0, 35, 250, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 200, 46, 4, 192, 255, 255, 255, 255, 255, 255, 255, 253, 46, 0,
    0, 0, 137, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 247, 255, 255, 255, 255, 255, 255, 255, 255, 153, 0, 0,
    0, 0, 8, 210, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 221, 14, 0, 0,
    0, 0, 0, 36, 233, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 242, 44, 0, 0, 0,
    0, 0, 0, 0, 48, 233, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 242, 60, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 38, 212, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 221, 44, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 10, 141, 251, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 252, 151, 14, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 38, 159, 248, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 249, 168, 45, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 16, 94, 160, 208, 244, 255, 255, 246, 210, 164, 98, 21, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

void drawGlyph(Arduino_Canvas *cv, Kind kind, int cx, int cy, int size) {
    Glyph g{cv, cx - size / 2.0f, cy - size / 2.0f, size / 24.0f};
    switch (kind) {
        case Kind::TELEGRAM: // desenhado por drawIcon (mascara); nao passa por aqui
            break;
        case Kind::MAIL: {
            int x0 = g.X(2), y0 = g.Y(4), w = g.X(22) - x0, h = g.Y(20) - y0;
            cv->drawRoundRect(x0, y0, w, h, 3, C_WHITE);
            cv->drawRoundRect(x0 + 1, y0 + 1, w - 2, h - 2, 2, C_WHITE);
            polyline(g, MAIL_FLAP, 3, C_WHITE);
            break;
        }
        case Kind::CHAT:
            polyline(g, CHAT_BOX, 9, C_WHITE);
            break;
        case Kind::BELL: {
            Pt arc[9];
            for (int i = 0; i < 9; i++) {
                float a = 3.14159265f * i / 8.0f;
                arc[i] = {12 - 6 * cosf(a), 8 - 6 * sinf(a)};
            }
            polyline(g, arc, 9, C_WHITE);
            polyline(g, BELL_BODY, 6, C_WHITE);
            polyline(g, BELL_CLAPPER, 5, C_WHITE);
            break;
        }
    }
}

Kind kindFor(const char *app) {
    char low[24];
    size_t i = 0;
    for (; app[i] && i < sizeof(low) - 1; i++) low[i] = (char)tolower((unsigned char)app[i]);
    low[i] = '\0';
    if (strstr(low, "telegram") || strstr(low, "unigram")) return Kind::TELEGRAM;
    static const char *const MAIL[] = {"mail", "outlook", "correio"};
    static const char *const CHAT[] = {"discord", "whatsapp", "slack", "teams", "messag", "mensag", "signal", "sms"};
    for (const char *m : MAIL) if (strstr(low, m)) return Kind::MAIL;
    for (const char *m : CHAT) if (strstr(low, m)) return Kind::CHAT;
    return Kind::BELL;
}

// Mascara de transparencia do app (32x32) pintada de branco sobre o quadrado.
void drawMask(Arduino_Canvas *cv, int x, int y, const uint8_t *mask) {
    uint16_t buf[ICON_SIZE * ICON_SIZE];
    for (int i = 0; i < ICON_SIZE * ICON_SIZE; i++) buf[i] = blend565(C_TILE, C_WHITE, mask[i]);
    cv->draw16bitRGBBitmap(x, y, buf, ICON_SIZE, ICON_SIZE);
}

void drawIcon(Arduino_Canvas *cv, int cx, int cy, const Note &n) {
    Kind kind = kindFor(n.app);
    const int x = cx - ICON_SIZE / 2, y = cy - ICON_SIZE / 2;
    if (kind == Kind::TELEGRAM) {
        drawMask(cv, x, y, TELEGRAM_MASK);
    } else if (n.icon >= 0 && n.icon < gIconCount) {
        drawMask(cv, x, y, gIcons[n.icon].mask);
    } else {
        drawGlyph(cv, kind, cx, cy, 26);
    }
}

// ---- lista ----------------------------------------------------------------

// Posicao dos blocos da tela de detalhe (antes da rolagem): icone + titulo
// em cima, corpo embaixo.
struct DetailLayout {
    char title[DETAIL_TITLE_LINES][WRAP_LINE_MAX];
    int titleLines;
    char body[DETAIL_BODY_LINES][WRAP_LINE_MAX];
    int bodyLines;
    int bodyY;
    int contentH; // altura total do conteudo, a partir do topo da lista
};

void layoutDetail(const Note &n, DetailLayout &d) {
    d.titleLines = wrapText(n.title[0] ? n.title : n.app, DETAIL_TITLE_CHARS, d.title, DETAIL_TITLE_LINES);
    d.bodyLines = n.body[0] ? wrapText(n.body, DETAIL_BODY_CHARS, d.body, DETAIL_BODY_LINES) : 0;
    int titleH = d.titleLines * DETAIL_PITCH;
    if (titleH < TILE) titleH = TILE;
    d.bodyY = 16 + titleH + 18;
    d.contentH = d.bodyY + d.bodyLines * DETAIL_PITCH + 20;
}

int maxScroll() {
    int content, view = SCREEN_H - HEADER_H;
    if (gDetail >= 0) {
        static DetailLayout d;
        layoutDetail(gNotes[gDetail], d);
        content = d.contentH;
    } else {
        content = gCount * ROW_H + LIST_PAD_BOTTOM;
    }
    return content > view ? content - view : 0;
}

float &curScroll() { return gDetail >= 0 ? gDScroll : gScroll; }
float &curVel() { return gDetail >= 0 ? gDVel : gVel; }

void clampScroll() {
    float &sc = curScroll();
    if (sc < 0) {
        sc = 0;
        curVel() = 0;
    }
    float m = (float)maxScroll();
    if (sc > m) {
        sc = m;
        curVel() = 0;
    }
}

int rowAt(int y) {
    if (y < HEADER_H) return -1;
    int idx = (int)((y - HEADER_H + gScroll) / ROW_H);
    return idx >= 0 && idx < gCount ? idx : -1;
}

void markAll() {
    for (int i = 0; i < gCount; i++) gNotes[i].read = true;
}

void drawRow(Arduino_Canvas *cv, int i, int y, bool pressed) {
    const Note &n = gNotes[i];
    if (pressed) cv->fillRect(0, y, SCREEN_W, ROW_H, C_HOVER);

    const int tx = 14, ty = y + (ROW_H - TILE) / 2;
    cv->fillRoundRect(tx, ty, TILE, TILE, 12, C_TILE);
    drawIcon(cv, tx + TILE / 2, ty + TILE / 2, n);

    char label[24];
    timeLabel(label, sizeof(label), n.at);
    text(cv, SCREEN_W - 14 - textW(label, 1), y + 15, 1, C_TIME, label);

    char title[WRAP_LINE_MAX];
    clipText(title, sizeof(title), n.title[0] ? n.title : n.app, TITLE_CHARS);
    text(cv, TEXT_X, y + 11, 2, n.read ? C_TITLE_READ : C_WHITE, title, !n.read);

    char lines[2][WRAP_LINE_MAX];
    int count = n.body[0] ? wrapText(n.body, BODY_CHARS, lines, 2) : 0;
    for (int l = 0; l < count; l++) text(cv, TEXT_X, y + 34 + l * 12, 1, C_DESC, lines[l]);

    if (!n.read) cv->fillCircle(SCREEN_W - 18, y + 38, 4, C_DOT);

    cv->drawFastHLine(TEXT_X, y + ROW_H - 1, SCREEN_W - TEXT_X - 14, C_SEP);
}

void drawEmpty(Arduino_Canvas *cv) {
    const int cx = SCREEN_W / 2, cy = HEADER_H + 82;
    cv->fillCircle(cx, cy, 34, C_TILE);
    drawGlyph(cv, Kind::BELL, cx, cy, 30);
    const char *l1 = "Nenhuma notificacao";
    text(cv, (SCREEN_W - textW(l1, 2)) / 2, cy + 52, 2, C_WHITE, l1, true);
    const char *l2 = "Voce esta em dia";
    text(cv, (SCREEN_W - textW(l2, 1)) / 2, cy + 78, 1, C_DESC, l2);
}

void drawHeader(Arduino_Canvas *cv) {
    cv->fillRect(0, 0, SCREEN_W, HEADER_H, C_BG);
    cv->drawFastHLine(0, HEADER_H - 1, SCREEN_W, C_SEP);

    // voltar (chevron)
    uint16_t back = gPressHdr == 1 ? C_DESC : C_WHITE;
    line2(cv, 27, 16, 19, 24, back);
    line2(cv, 19, 24, 27, 32, back);

    text(cv, 42, 16, 2, C_WHITE, "Notificacoes", true);

    int unread = Notification::unreadCount();
    if (unread > 0) {
        char num[8];
        if (unread > 99) snprintf(num, sizeof(num), "99+");
        else snprintf(num, sizeof(num), "%d", unread);
        int w = textW(num, 1) + 14;
        cv->fillRoundRect(42 + 144 + 10, 15, w, 18, 9, C_DOT);
        text(cv, 42 + 144 + 10 + 7, 20, 1, C_WHITE, num);
    }

    // marcar todas como lidas
    cv->fillRoundRect(PILL_X, PILL_Y, PILL_W, PILL_H, PILL_H / 2, gPressHdr == 2 ? C_PILL_PRESSED : C_PILL);
    const char *label = "Marcar todas como lidas";
    text(cv, PILL_X + (PILL_W - textW(label, 1)) / 2, PILL_Y + 9, 1, unread > 0 ? C_PILL_TEXT : C_TIME, label);
}

void drawScrollbar(Arduino_Canvas *cv) {
    int m = maxScroll();
    if (m <= 0) return;
    int visible = SCREEN_H - HEADER_H;
    int view = visible - 8;
    int content = visible + m;
    int thumb = view * visible / content;
    if (thumb < 24) thumb = 24;
    int y = HEADER_H + 4 + (int)((view - thumb) * (curScroll() / m));
    cv->fillRoundRect(SCREEN_W - 4, y, 3, thumb, 1, C_SCROLL);
}

// Tela de detalhe: a notificacao inteira, rolavel. O cabecalho (voltar, app e
// horario) fica por cima do conteudo, como na lista.
void drawDetail(Arduino_Canvas *cv) {
    const Note &n = gNotes[gDetail];
    static DetailLayout d;
    layoutDetail(n, d);
    int off = (int)lroundf(gDScroll);
    int top = HEADER_H - off; // y do conteudo com a rolagem aplicada

    cv->fillScreen(C_BG);
    const int ty = top + 16;
    cv->fillRoundRect(14, ty, TILE, TILE, 12, C_TILE);
    drawIcon(cv, 14 + TILE / 2, ty + TILE / 2, n);
    int titleH = d.titleLines * DETAIL_PITCH;
    int titleY = ty + (TILE > titleH ? (TILE - titleH) / 2 : 0);
    for (int l = 0; l < d.titleLines; l++) text(cv, TEXT_X, titleY + l * DETAIL_PITCH, 2, C_WHITE, d.title[l], true);

    int by = top + d.bodyY;
    cv->drawFastHLine(14, by - 10, SCREEN_W - 28, C_SEP);
    if (d.bodyLines == 0) {
        text(cv, 14, by, 2, C_TIME, "(sem texto)");
    } else {
        for (int l = 0; l < d.bodyLines; l++) {
            int y = by + l * DETAIL_PITCH;
            if (y + DETAIL_PITCH < HEADER_H || y > SCREEN_H) continue;
            text(cv, 14, y, 2, C_TITLE_READ, d.body[l]);
        }
    }
    drawScrollbar(cv);

    // cabecalho: voltar + app, horario na direita
    cv->fillRect(0, 0, SCREEN_W, HEADER_H, C_BG);
    cv->drawFastHLine(0, HEADER_H - 1, SCREEN_W, C_SEP);
    uint16_t back = gPressHdr == 1 ? C_DESC : C_WHITE;
    line2(cv, 27, 16, 19, 24, back);
    line2(cv, 19, 24, 27, 32, back);
    char app[32];
    clipText(app, sizeof(app), n.app[0] ? n.app : "Notificacao", 18);
    text(cv, 42, 16, 2, C_WHITE, app, true);
    char label[24];
    timeLabel(label, sizeof(label), n.at);
    text(cv, SCREEN_W - 14 - textW(label, 1), 20, 1, C_TIME, label);
}

} // namespace

namespace Notification {

void setIcon(const char *b64) {
    gHasPending = false;
    if (!b64 || !b64[0]) return;
    size_t outLen = 0;
    if (mbedtls_base64_decode(gPending, sizeof(gPending), &outLen, (const unsigned char *)b64, strlen(b64)) == 0 &&
        outLen == sizeof(gPending)) {
        gHasPending = true;
    }
}

void add(const char *app, const char *title, const char *body) {
    Note n;
    memset(&n, 0, sizeof(n));
    copyAscii(n.app, sizeof(n.app), app);
    copyAscii(n.title, sizeof(n.title), title);
    copyAscii(n.body, sizeof(n.body), body);
    if (gDetail >= 0) gDetail = gDetail + 1 < MAX_NOTES ? gDetail + 1 : -1; // a lista deslocou
    n.at = millis();
    n.read = false;
    n.icon = -1;

    for (int i = 0; i < gIconCount; i++) {
        if (strcasecmp(gIcons[i].app, n.app) == 0) n.icon = (int8_t)i;
    }
    if (gHasPending) {
        if (n.icon < 0) {
            int slot;
            if (gIconCount < MAX_ICONS) {
                slot = gIconCount++;
            } else {
                slot = gIconNext++ % MAX_ICONS;
                for (int i = 0; i < gCount; i++) {
                    if (gNotes[i].icon == slot) gNotes[i].icon = -1;
                }
            }
            copyAscii(gIcons[slot].app, sizeof(gIcons[slot].app), n.app);
            n.icon = (int8_t)slot;
        }
        memcpy(gIcons[n.icon].mask, gPending, sizeof(gPending));
        gHasPending = false;
    }

    int keep = gCount < MAX_NOTES ? gCount : MAX_NOTES - 1;
    memmove(&gNotes[1], &gNotes[0], sizeof(Note) * keep);
    gNotes[0] = n;
    if (gCount < MAX_NOTES) gCount++;

    gScroll = 0;
    gVel = 0;
    gDirty = true;
}

void open() {
    gScroll = 0;
    gVel = 0;
    gDScroll = 0;
    gDVel = 0;
    gDetail = -1;
    gDown = false;
    gMoved = false;
    gUpStreak = 0;
    gPressRow = -1;
    gPressHdr = 0;
    gLastTick = millis();
    gLastLabelRefresh = gLastTick;
    gDirty = true;
}

Action handleTouch(bool touched, int x, int y) {
    Action act = Action::NONE;
    if (touched) {
        gUpStreak = 0;
        if (!gDown) {
            gDown = true;
            gMoved = false;
            gStartY = y;
            gLastY = y;
            curVel() = 0;
            gPressHdr = 0;
            if (y < HEADER_H) {
                if (x < 70) gPressHdr = 1;
                else if (gDetail < 0 && x >= PILL_X - 8) gPressHdr = 2;
            }
            gPressRow = gDetail < 0 ? rowAt(y) : -1;
            gDirty = true;
        } else {
            if (!gMoved && abs(y - gStartY) > 10) {
                gMoved = true;
                gPressRow = -1;
                gPressHdr = 0;
                gDirty = true;
            }
            if (gMoved && gStartY >= HEADER_H) {
                float dy = (float)(gLastY - y);
                curScroll() += dy;
                curVel() = dy;
                clampScroll();
                gDirty = true;
            }
            gLastY = y;
        }
    } else if (gDown && ++gUpStreak >= 2) { // soltou (ignora 1 leitura falha do touch)
        gDown = false;
        if (!gMoved) {
            if (gPressHdr == 1) {
                if (gDetail >= 0) gDetail = -1; // do detalhe volta pra lista
                else act = Action::CLOSE;
            } else if (gPressHdr == 2) {
                markAll();
            } else if (gPressRow >= 0 && gPressRow < gCount) {
                gNotes[gPressRow].read = true; // abre a mensagem completa
                gDetail = gPressRow;
                gDScroll = 0;
                gDVel = 0;
            }
        }
        gPressRow = -1;
        gPressHdr = 0;
        gDirty = true;
    }
    return act;
}

bool tick(unsigned long now) {
    unsigned long dt = now - gLastTick;
    gLastTick = now;
    if (dt > 100) dt = 100;
    if (!gDown && fabsf(curVel()) > 0.4f) {
        curScroll() += curVel() * dt / 16.0f; // inercia depois de soltar o dedo
        curVel() *= powf(0.92f, dt / 16.0f);
        clampScroll();
        gDirty = true;
    } else if (!gDown) {
        curVel() = 0;
    }
    if (now - gLastLabelRefresh >= 15000) { // "agora" -> "1 min"...
        gLastLabelRefresh = now;
        gDirty = true;
    }
    bool d = gDirty;
    gDirty = false;
    return d;
}

void draw(Arduino_Canvas *cv) {
    if (gDetail >= 0) {
        drawDetail(cv);
        return;
    }
    cv->fillScreen(C_BG);
    if (gCount == 0) {
        drawEmpty(cv);
    } else {
        int offset = (int)lroundf(gScroll);
        for (int i = 0; i < gCount; i++) {
            int y = HEADER_H + i * ROW_H - offset;
            if (y + ROW_H <= HEADER_H || y >= SCREEN_H) continue;
            drawRow(cv, i, y, i == gPressRow);
        }
        drawScrollbar(cv);
    }
    drawHeader(cv); // por cima da lista, tampando o que rola por baixo dele
}

bool inDetail() { return gDetail >= 0; }

int count() { return gCount; }

int unreadCount() {
    int n = 0;
    for (int i = 0; i < gCount; i++) {
        if (!gNotes[i].read) n++;
    }
    return n;
}

} // namespace Notification
