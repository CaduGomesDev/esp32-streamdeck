/**
 * Stream Deck caseiro — ESP32-S3 JC3248W535 (3.5" 320x480 touch capacitivo)
 *
 * Grade de botoes touch que envia teclas de funcao F13-F24 via USB HID.
 * F13-F24 nao existem em teclados fisicos comuns, entao nao conflitam com
 * atalhos do sistema: associe cada tecla a uma acao dentro do OBS, Discord,
 * PowerPoint etc. (ex: OBS > Configuracoes > Atalhos).
 *
 * A configuracao dos botoes (rotulo/tecla/cor) e o GIF da tela de descanso
 * ficam gravados em LittleFS e podem ser editados sem recompilar, via app
 * de configuracao no PC (protocolo serial, ver SerialProtocol.h).
 */

#include <Arduino.h>
#include <JC3248W535.h>
#include <LittleFS.h>
#include <USB.h>
#include <USBHIDConsumerControl.h>
#include <USBHIDKeyboard.h>

#include "Background.h"
#include "Config.h"
#include "Icon.h"
#include "Notification.h"
#include "Mascot.h"
#include "Screensaver.h"
#include "SerialProtocol.h"
#include "Status.h"
#include "Usage.h"
#include "FreeSansBold10pt7b.h"

JC3248W535_Display display;
JC3248W535_Touch touch;
USBHIDKeyboard Keyboard;
USBHIDConsumerControl ConsumerControl;

DeckConfig deckConfig;

const int COLS = 3;
const int COLS_FEW = 2; // ate 4 botoes: grade 2x2 em vez de 3 + 1 sozinho
const int MARGIN = 10;
const int GAP = 10;

int btnX[MAX_BUTTONS], btnY[MAX_BUTTONS], btnW, btnH;
int pressedIndex = -1;

// O touch capacitivo as vezes erra uma unica leitura no meio de um toque
// (reporta solto por 1 ciclo e volta a reportar tocado em seguida) — isso
// disparava a acao do botao, soltava e re-apertava sozinho, e disparava de
// novo na soltura de verdade (ex: mutar desmutava na hora). So confirma a
// SOLTURA depois de ve-la se repetir por alguns ciclos seguidos; o aperto
// continua instantaneo (sem isso, um toque rapido/leve exigia encostar o
// dedo inteiro pra ser notado).
const uint8_t RELEASE_DEBOUNCE_READS = 2;
uint8_t releaseStreak = 0;

enum class Mode { BUTTONS, SCREENSAVER, STATUS, USAGE, NOTIFICATION };
Mode mode = Mode::BUTTONS;
unsigned long lastActivityMs = 0;

// Tela de status (lista rolavel, ver Status.h): pede dados ao PC a cada
// STATUS_REFRESH_MS. So sai no botao voltar (fica fixa, sem timeout automatico).
const unsigned long STATUS_REFRESH_MS = 2000;
const unsigned long STATUS_FRAME_MS = 90; // quadro das animacoes dos cards (LED, cursor)
unsigned long lastStatusRequestMs = 0;
unsigned long lastStatusDrawMs = 0;
uint8_t statusIdleReads = 0; // como notifyIdleReads: so aceita toque depois de ver o dedo solto

// Tela de uso do Claude: igual a de status (pede ao PC, fica fixa ate tocar,
// nunca cai na tela de descanso). O dado so muda a cada ~30s no PC, entao
// pede com menos frequencia; ja o desenho roda em quadros (mascote animado).
const unsigned long USAGE_REFRESH_MS = 15000;
const unsigned long USAGE_FRAME_MS = 30;
unsigned long lastUsageRequestMs = 0;
unsigned long lastUsageDrawMs = 0;
bool usageTouching = false;

// Tela de notificacoes (lista, ver Notification.h): aparece por cima de
// qualquer modo quando o PC manda {"cmd":"notify",...} (ver
// tools/notify-bridge.ps1) e fecha sozinha depois de um tempo sem toque
// (curto se abriu por causa de uma notificacao nova, longo se foi aberta pelo
// botao); o botao voltar da tela tambem fecha.
const unsigned long NOTIFY_AUTO_CLOSE_MS = 10000;
const unsigned long NOTIFY_MANUAL_CLOSE_MS = 60000;
unsigned long notifyLastTouchMs = 0;
bool notifyAutoOpened = false;
Mode notifyReturnMode = Mode::BUTTONS; // tela em que estavamos antes de abrir as notificacoes

// O touch capacitivo as vezes ainda reporta contato por um ou dois ciclos
// depois do dedo sair, e o toque que abriu/fechou a notificacao era lido de
// novo na tela seguinte (fechava na hora, ou apertava o botao de baixo).
// Entao a notificacao so aceita toque depois de ver o dedo solto, e depois
// de fechar o grid ignora o toque ate o dedo soltar de novo.
uint8_t notifyIdleReads = 0;
bool swallowTouch = false;
uint8_t swallowIdleReads = 0;

void layoutButtons() {
    int screenW = display.width();
    int screenH = display.height();
    const int cols = deckConfig.buttonCount <= 4 ? COLS_FEW : COLS;
    int rows = (deckConfig.buttonCount + cols - 1) / cols;
    if (rows < 1) rows = 1;

    btnW = (screenW - 2 * MARGIN - (cols - 1) * GAP) / cols;
    btnH = (screenH - 2 * MARGIN - (rows - 1) * GAP) / rows;

    for (int i = 0; i < deckConfig.buttonCount; i++) {
        int col = i % cols;
        int row = i / cols;
        btnX[i] = MARGIN + col * (btnW + GAP);
        btnY[i] = MARGIN + row * (btnH + GAP);
    }
}

// Cinza claro pra borda dos botoes (RGB565 de ~rgb(180,180,190)).
const uint16_t BORDER_GRAY = 0xB5B7;

// Mistura `fg` sobre `bg` (RGB565) com opacidade `a` (0-255) do fg.
inline uint16_t blend565(uint16_t bg, uint16_t fg, uint8_t a) {
    uint32_t r = (((fg >> 11) & 0x1F) * a + ((bg >> 11) & 0x1F) * (255 - a)) / 255;
    uint32_t g = (((fg >> 5) & 0x3F) * a + ((bg >> 5) & 0x3F) * (255 - a)) / 255;
    uint32_t b = ((fg & 0x1F) * a + (bg & 0x1F) * (255 - a)) / 255;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

// Botoes das telas do sistema (Claude, Health, Avisos, Voltar): o icone traz so
// o desenho; a barra fina e o nome (o rotulo do botao, em maiusculas, em fonte
// bold) sao desenhados aqui, centralizados no botao.
bool isCaptioned(int i) {
    const char *t = deckConfig.buttons[i].type;
    return strcmp(t, "usage") == 0 || strcmp(t, "status") == 0 ||
           strcmp(t, "notifications") == 0 || strcmp(t, "screensaver") == 0;
}

// Icones das telas do sistema que sao desenhados (e animados) pelo firmware em
// vez de vir da imagem gravada na placa — cada um com um movimento que combina
// com o desenho: terminal digitando, sino tocando, seta disparando, mascote.
enum class AnimIcon { NONE, HEALTH, BELL, BACK, CLAUDE };

AnimIcon animKind(int i) {
    const char *t = deckConfig.buttons[i].type;
    if (strcmp(t, "status") == 0) return AnimIcon::HEALTH;
    if (strcmp(t, "notifications") == 0) return AnimIcon::BELL;
    if (strcmp(t, "screensaver") == 0) return AnimIcon::BACK;
    if (strcmp(t, "usage") == 0) return AnimIcon::CLAUDE;
    return AnimIcon::NONE;
}

void drawAnimIcon(int i, bool pressed, unsigned long now); // definido junto do mascote

const int STACK_H = 70; // desenho (y 2..32) + barra (y 44) + nome (y 57..72)

// Topo da imagem do icone (64x64) dentro do botao.
int iconTopFor(int i) {
    return isCaptioned(i) ? btnY[i] + (btnH - STACK_H) / 2 - 2 : btnY[i] + (btnH - Icon::SIZE) / 2;
}

void drawCaption(int i, bool pressed) {
    Arduino_Canvas *canvas = display.getCanvas();
    const uint16_t col = pressed ? BLACK : WHITE;
    const int cx = btnX[i] + btnW / 2;
    const int top = iconTopFor(i);

    canvas->fillRect(cx - 18, top + 44, 36, 2, col);

    char label[16];
    size_t n = 0;
    for (; deckConfig.buttons[i].label[n] && n < sizeof(label) - 1; n++) {
        label[n] = (char)toupper((unsigned char)deckConfig.buttons[i].label[n]);
    }
    label[n] = '\0';

    canvas->setFont(&FreeSansBold10pt7b);
    canvas->setTextSize(1);
    canvas->setTextColor(col);
    int16_t tx, ty;
    uint16_t tw, th;
    canvas->getTextBounds(label, 0, 0, &tx, &ty, &tw, &th);
    canvas->setCursor(cx - tw / 2 - tx, top + 57 - ty); // ty < 0: topo das maiusculas acima da linha de base
    canvas->print(label);
    canvas->setFont();
}

// Botao com imagem de fundo: a caixa e uma cor translucida (boxOpacity)
// misturada com a foto — sempre a partir dos pixels originais da foto, entao
// apertar/soltar nao acumula mistura. Monta o retangulo num buffer (cantos
// arredondados mantem a foto) e empurra de uma vez. Retorna false se faltar
// memoria (o chamador desenha o botao opaco normal).
bool drawButtonOverBackground(int i, bool pressed, bool doFlush = true) {
    Arduino_Canvas *canvas = display.getCanvas();
    const uint16_t *bg = Background::pixels();
    const int w = btnW, h = btnH, r = 12;
    uint16_t *buf = (uint16_t *)ps_malloc((size_t)w * h * sizeof(uint16_t));
    if (!buf) return false;

    // Com fundo a caixa nao tem cor propria: e so um vidro escuro translucido
    // (a opacidade e o unico ajuste); a cor do botao vale so sem fundo.
    uint16_t color = pressed ? WHITE : BLACK;
    uint8_t alpha = pressed ? 235 : (uint8_t)(deckConfig.boxOpacity * 255 / 100);

    for (int y = 0; y < h; y++) {
        // distancia vertical ao centro do canto (em meios-pixels), 0 fora dos cantos
        int dy2 = y < r ? 2 * (r - y) - 1 : (y >= h - r ? 2 * (y - (h - r)) + 1 : 0);
        for (int x = 0; x < w; x++) {
            int dx2 = x < r ? 2 * (r - x) - 1 : (x >= w - r ? 2 * (x - (w - r)) + 1 : 0);
            uint16_t photo = bg[(btnY[i] + y) * BG_WIDTH + btnX[i] + x];
            bool inside = dx2 * dx2 + dy2 * dy2 <= 4 * r * r;
            buf[y * w + x] = inside ? blend565(photo, color, alpha) : photo;
        }
    }

    bool drewIcon = animKind(i) == AnimIcon::NONE &&
                    Icon::blendInto(buf, w, h, i, (w - Icon::SIZE) / 2, iconTopFor(i) - btnY[i]);
    canvas->draw16bitRGBBitmap(btnX[i], btnY[i], buf, w, h);
    free(buf);
    canvas->drawRoundRect(btnX[i], btnY[i], w, h, r, BORDER_GRAY);
    drawAnimIcon(i, pressed, millis());

    if (isCaptioned(i)) {
        drawCaption(i, pressed);
    } else if (!drewIcon) {
        canvas->setTextColor(pressed ? BLACK : WHITE);
        canvas->setTextSize(3);
        const char *label = deckConfig.buttons[i].label;
        int16_t tx, ty;
        uint16_t tw, th;
        canvas->getTextBounds(label, 0, 0, &tx, &ty, &tw, &th);
        canvas->setCursor(btnX[i] + (btnW - tw) / 2, btnY[i] + (btnH - th) / 2);
        canvas->print(label);
    }
    if (doFlush) display.flush();
    return true;
}

// O fundo so serve se a tela for a paisagem 480x320 que o app converte.
bool hasBackground() {
    return Background::pixels() && display.width() == BG_WIDTH && display.height() == BG_HEIGHT;
}

void drawButton(int i, bool pressed, bool doFlush = true) {
    if (hasBackground() && drawButtonOverBackground(i, pressed, doFlush)) return;

    Arduino_Canvas *canvas = display.getCanvas();
    uint16_t baseColor = Config::colorHexToRGB565(deckConfig.buttons[i].colorHex);
    uint16_t fill = pressed ? WHITE : baseColor;
    uint16_t text = pressed ? BLACK : WHITE;

    canvas->fillRoundRect(btnX[i], btnY[i], btnW, btnH, 12, fill);
    canvas->drawRoundRect(btnX[i], btnY[i], btnW, btnH, 12, BORDER_GRAY);

    int ix = btnX[i] + (btnW - Icon::SIZE) / 2;
    int iy = iconTopFor(i);
    bool drewIcon = animKind(i) == AnimIcon::NONE && Icon::exists(i) && Icon::draw(canvas, i, ix, iy, fill);
    drawAnimIcon(i, pressed, millis());

    if (isCaptioned(i)) {
        drawCaption(i, pressed);
    } else if (!drewIcon) {
        canvas->setTextColor(text);
        canvas->setTextSize(3);
        const char *label = deckConfig.buttons[i].label;
        int16_t tx, ty;
        uint16_t tw, th;
        canvas->getTextBounds(label, 0, 0, &tx, &ty, &tw, &th);
        int cx = btnX[i] + (btnW - tw) / 2;
        int cy = btnY[i] + (btnH - th) / 2;
        canvas->setCursor(cx, cy);
        canvas->print(label);
    }

    if (doFlush) display.flush();
}

// Quadro das animacoes dos icones: cada quadro redesenha o icone de todos os
// botoes que tem um animado (animButtons) e empurra a tela uma unica vez.
const unsigned long ANIM_FRAME_MS = 33;
unsigned long animDrawnAt = 0;
int animButtons[MAX_BUTTONS];
int animCount = 0;

// Mascote do Claude Code (o mesmo da tela de uso, ver Mascot.h) dancando no
// botao "usage". O icone do botao e vazio (a barra e o nome vem do
// drawCaption); o mascote e desenhado por cima a cada ANIM_FRAME_MS.
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

void drawMascot(int i, unsigned long now) {
    static const Mascot::Colors colors = {
        rgb565(217, 119, 87),  // laranja do Claude
        rgb565(168, 86, 62),   // lateral
        rgb565(38, 32, 28),
        blend565(BLACK, WHITE, 90),
    };
    // pes acima da barra do nome (y 44), mesmo tamanho da tela de uso
    Mascot::draw(display.getCanvas(), btnX[i] + btnW / 2, iconTopFor(i) + 31, 3, now, colors);
}

// Verde do icone do Health Server (e uma versao escura pra caber no botao
// branco do toque).
const uint16_t HEALTH_GREEN = ((52 & 0xF8) << 8) | ((211 & 0xFC) << 3) | (153 >> 3);
const uint16_t HEALTH_GREEN_DARK = ((4 & 0xF8) << 8) | ((120 & 0xFC) << 3) | (87 >> 3);

// Terminal do Health: a borda respira, o ">" fica fixo e a linha "digita"
// tracinhos com o cursor piscando na frente.
void drawHealthIcon(Arduino_Canvas *cv, int cx, int top, bool pressed, unsigned long now) {
    const uint16_t bg = pressed ? WHITE : BLACK;
    const uint16_t fg = pressed ? HEALTH_GREEN_DARK : HEALTH_GREEN;
    float pulse = 0.5f + 0.5f * sinf(now * 0.0035f);
    uint16_t frame = blend565(bg, fg, (uint8_t)(150 + 105 * pulse));

    cv->drawRoundRect(cx - 15, top + 2, 30, 30, 5, frame);
    cv->drawRoundRect(cx - 14, top + 3, 28, 28, 4, frame);

    for (int d = 0; d < 2; d++) { // ">"
        cv->drawLine(cx - 9 + d, top + 11, cx - 4 + d, top + 16, fg);
        cv->drawLine(cx - 9 + d, top + 21, cx - 4 + d, top + 16, fg);
    }

    int typed = (int)((now / 420) % 4); // 0..3 tracinhos ja digitados
    for (int k = 0; k < typed; k++) cv->fillRect(cx - 1 + k * 4, top + 22, 2, 2, fg);
    bool cursorOn = (now / 300) % 2 == 0;
    if (cursorOn) cv->fillRect(cx - 1 + typed * 4, top + 22, 3, 2, fg);
}

// Sino do Avisos: formato fixo (silhueta do lado direito, espelhada) girado em
// torno do topo. Toca com oscilacao amortecida e depois descansa; enquanto
// toca, aparecem ondinhas dos lados.
const int8_t BELL_PTS[][2] = {{0, 2}, {4, 3}, {7, 6}, {9, 11}, {10, 17}, {11, 21}, {13, 24}, {14, 26}};
const int BELL_N = sizeof(BELL_PTS) / sizeof(BELL_PTS[0]);

void drawBellIcon(Arduino_Canvas *cv, int cx, int top, bool pressed, unsigned long now) {
    const uint16_t fg = pressed ? BLACK : WHITE;

    float t = (now % 2600) / 1000.0f; // 1,2 s tocando + descanso
    float energy = t < 1.2f ? expf(-2.2f * t) : 0.0f;
    float ang = t < 1.2f ? 0.5f * energy * sinf(2.0f * PI * 3.0f * t) : 0.0f;
    float s = sinf(ang), c = cosf(ang);
    const int px = cx, py = top + 3;

    auto rot = [&](int x, int y, int16_t &ox, int16_t &oy) {
        ox = px + (int16_t)lroundf(x * c - y * s);
        oy = py + (int16_t)lroundf(x * s + y * c);
    };

    int16_t pts[2 * BELL_N][2];
    int n = 0;
    for (int k = 0; k < BELL_N; k++) rot(BELL_PTS[k][0], BELL_PTS[k][1], pts[n][0], pts[n][1]), n++;
    for (int k = BELL_N - 1; k >= 0; k--) rot(-BELL_PTS[k][0], BELL_PTS[k][1], pts[n][0], pts[n][1]), n++;

    int16_t mx, my;
    rot(0, 14, mx, my); // centro: a silhueta e "estrela" em torno dele
    for (int k = 0; k < n; k++) {
        int16_t *a = pts[k], *b = pts[(k + 1) % n];
        cv->fillTriangle(mx, my, a[0], a[1], b[0], b[1], fg);
    }
    int16_t kx, ky, lx, ly;
    rot(0, 0, kx, ky);
    cv->fillCircle(kx, ky, 2, fg); // argola do topo
    rot(0, 30, lx, ly);
    cv->fillCircle(lx - (int)(s * 5), ly, 3, fg); // badalo, atrasado em relacao ao sino

    if (energy > 0.3f) {
        for (int side = -1; side <= 1; side += 2) {
            for (int k = 0; k < (energy > 0.55f ? 2 : 1); k++) {
                int x = cx + side * (21 + k * 5), y = top + 17;
                int h = 5 + k * 2;
                for (int d = 0; d < 2; d++) {
                    cv->drawLine(x + d, y - h, x + side * 2 + d, y, fg);
                    cv->drawLine(x + side * 2 + d, y, x + d, y + h, fg);
                }
            }
        }
    }
}

// Chevron "<" com a ponta em tipX; thick = espessura em pixels.
void drawChevron(Arduino_Canvas *cv, int tipX, int cy, int half, int thick, uint16_t col) {
    for (int d = 0; d < thick; d++) {
        cv->drawLine(tipX + d, cy, tipX + half + d, cy - half, col);
        cv->drawLine(tipX + d, cy, tipX + half + d, cy + half, col);
    }
}

// Seta do Voltar: de tempos em tempos dispara pra esquerda e volta, deixando
// um rastro de chevrons que somem.
void drawBackIcon(Arduino_Canvas *cv, int cx, int top, bool pressed, unsigned long now) {
    const uint16_t fg = pressed ? BLACK : WHITE;
    const uint16_t bg = pressed ? WHITE : BLACK;
    const unsigned long CYCLE = 1500, MOVE = 520;

    unsigned long ph = now % CYCLE;
    float m = ph < MOVE ? sinf(PI * ph / MOVE) : 0.0f; // 0 -> 1 -> 0
    int dx = -(int)(7 * m + 0.5f);
    const int tip = cx - 6 + dx, cy = top + 17;

    if (m > 0.05f) {
        drawChevron(cv, tip + 14, cy, 12, 1, blend565(bg, fg, 55));
        drawChevron(cv, tip + 7, cy, 12, 1, blend565(bg, fg, 110));
    }
    drawChevron(cv, tip, cy, 12, 4, fg);
}

// Desenha o icone animado do botao i (se ele tiver um) na fase `now`.
void drawAnimIcon(int i, bool pressed, unsigned long now) {
    AnimIcon kind = animKind(i);
    if (kind == AnimIcon::NONE) return;
    Arduino_Canvas *cv = display.getCanvas();
    const int cx = btnX[i] + btnW / 2;
    const int top = iconTopFor(i);
    switch (kind) {
        case AnimIcon::HEALTH: drawHealthIcon(cv, cx, top, pressed, now); break;
        case AnimIcon::BELL: drawBellIcon(cv, cx, top, pressed, now); break;
        case AnimIcon::BACK: drawBackIcon(cv, cx, top, pressed, now); break;
        case AnimIcon::CLAUDE: drawMascot(i, now); break;
        default: break;
    }
}

// Quadro de animacao do botao i: em vez de refazer o botao inteiro (mistura
// do vidro com a foto em todos os pixels), repoe so a faixa do desenho do
// icone — a barra e o nome, mais embaixo, nao mudam — e redesenha o icone.
void drawAnimFrame(int i) {
    Arduino_Canvas *canvas = display.getCanvas();
    const int cx = btnX[i] + btnW / 2;
    const int top = iconTopFor(i);
    int x0 = cx - 32, x1 = cx + 32;
    if (x0 < btnX[i] + 3) x0 = btnX[i] + 3;
    if (x1 > btnX[i] + btnW - 3) x1 = btnX[i] + btnW - 3;
    const int y0 = top, h = 42; // a barra do nome comeca em top + 44
    const int w = x1 - x0;

    uint16_t *buf = hasBackground() ? (uint16_t *)ps_malloc((size_t)w * h * sizeof(uint16_t)) : nullptr;
    if (buf) {
        const uint16_t *bg = Background::pixels();
        const uint8_t alpha = (uint8_t)(deckConfig.boxOpacity * 255 / 100);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                buf[y * w + x] = blend565(bg[(y0 + y) * BG_WIDTH + x0 + x], BLACK, alpha);
            }
        }
        canvas->draw16bitRGBBitmap(x0, y0, buf, w, h);
        free(buf);
    } else {
        canvas->fillRect(x0, y0, w, h, hasBackground() ? BLACK : Config::colorHexToRGB565(deckConfig.buttons[i].colorHex));
    }
    drawAnimIcon(i, false, millis());
}

void drawAllButtons() {
    if (hasBackground()) {
        display.getCanvas()->draw16bitRGBBitmap(0, 0, (uint16_t *)Background::pixels(), BG_WIDTH, BG_HEIGHT);
    } else {
        display.getCanvas()->fillScreen(BLACK);
    }
    layoutButtons();
    for (int i = 0; i < deckConfig.buttonCount; i++) {
        drawButton(i, false);
    }
    animCount = 0;
    for (int i = 0; i < deckConfig.buttonCount; i++) {
        if (animKind(i) != AnimIcon::NONE) animButtons[animCount++] = i;
    }
    animDrawnAt = 0; // desenha o primeiro quadro na proxima volta do loop
}

int hitTestButton(int x, int y) {
    for (int i = 0; i < deckConfig.buttonCount; i++) {
        if (x >= btnX[i] && x < btnX[i] + btnW &&
            y >= btnY[i] && y < btnY[i] + btnH) {
            return i;
        }
    }
    return -1;
}

// O USBHIDKeyboard manda codigos de tecla (posicao fisica) do layout US, mas o
// PC usa teclado ABNT2: sem traducao, ':' vira 'Ç' e '/' vira ';'. Estes sao
// os caracteres que mudam de tecla do US pro ABNT2 (usage HID + se precisa
// de Shift). Os demais (letras, digitos, . , - = & etc.) caem na mesma tecla.
// Nao cobre as teclas mortas (~ ^ ` ´ ¨), que dependem de composicao.
struct Abnt2Key {
    char c;
    uint8_t usage;
    bool shift;
};
const Abnt2Key kAbnt2Keys[] = {
    {'/', 0x87, false}, {'?', 0x87, true},   // tecla extra ao lado do Shift direito
    {';', 0x38, false}, {':', 0x38, true},
    {'\\', 0x64, false}, {'|', 0x64, true},  // tecla extra ao lado do Shift esquerdo
    {'\'', 0x35, false}, {'"', 0x35, true},
    {'[', 0x30, false}, {'{', 0x30, true},
    {']', 0x32, false}, {'}', 0x32, true},
};

void typeChar(char c) {
    for (const Abnt2Key &k : kAbnt2Keys) {
        if (k.c != c) continue;
        if (k.shift) Keyboard.press(KEY_LEFT_SHIFT);
        Keyboard.pressRaw(k.usage);
        delay(10);
        Keyboard.releaseAll();
        delay(5);
        return;
    }
    Keyboard.write((uint8_t)c);
}

// Digita um macro: opcionalmente abre o dialogo "Executar" do Windows
// (Win+R) antes, digita o texto configurado e opcionalmente finaliza com
// Enter. Usado por ex. pra abrir um site/app direto de um botao.
void runMacro(const ButtonConfig &btn) {
    if (btn.macroWinR) {
        Keyboard.press(KEY_LEFT_GUI);
        Keyboard.press('r');
        delay(50);
        Keyboard.releaseAll();
        delay(300); // espera a caixa "Executar" abrir antes de digitar
    }
    for (const char *p = btn.macroText; *p; p++) typeChar(*p);
    if (btn.macroEnter) {
        Keyboard.press(KEY_RETURN);
        delay(15);
        Keyboard.release(KEY_RETURN);
    }
}

// Avisa o script no PC (tools/status-bridge.ps1) que queremos dados novos.
// Ele responde com o comando serial "status" (ver SerialProtocol.cpp).
void requestStatus() {
    Serial.println("{\"event\":\"status_request\"}");
    lastStatusRequestMs = millis();
}

void enterStatus() {
    mode = Mode::STATUS;
    statusIdleReads = 0;
    lastActivityMs = millis();
    Status::open();
    requestStatus();
    Status::draw(display.getCanvas());
    display.flush();
}

void exitStatus(bool touchedNow) {
    mode = Mode::BUTTONS;
    drawAllButtons();
    swallowTouch = touchedNow; // o dedo do "voltar" nao pode apertar um botao do grid
    swallowIdleReads = 0;
}

// Avisa a ponte no PC (tools/bridge.ps1) que queremos o uso do Claude. Ela
// responde com o comando serial "usage" (ver SerialProtocol.cpp).
void requestUsage() {
    Serial.println("{\"event\":\"usage_request\"}");
    lastUsageRequestMs = millis();
}

void drawUsage() {
    Usage::drawFrame(display.getCanvas());
    display.flush();
    lastUsageDrawMs = millis();
}

void enterUsage() {
    mode = Mode::USAGE;
    usageTouching = false;
    lastActivityMs = millis();
    requestUsage();
    Usage::invalidate(); // o canvas estava com outra tela
    drawUsage();
}

void exitUsage() {
    mode = Mode::BUTTONS;
    usageTouching = false;
    drawAllButtons();
}

void enterScreensaver(); // definido mais abaixo (usado por triggerAction)
void enterNotificationScreen(); // idem

void triggerAction(int idx) {
    const ButtonConfig &btn = deckConfig.buttons[idx];

    if (strcmp(btn.type, "status") == 0) {
        enterStatus();
    } else if (strcmp(btn.type, "usage") == 0) {
        enterUsage();
    } else if (strcmp(btn.type, "screensaver") == 0) {
        enterScreensaver();
    } else if (strcmp(btn.type, "notifications") == 0) {
        enterNotificationScreen();
    } else if (strcmp(btn.type, "macro") == 0) {
        runMacro(btn);
    } else if (strcmp(btn.type, "volume_up") == 0) {
        ConsumerControl.press(CONSUMER_CONTROL_VOLUME_INCREMENT);
        delay(15);
        ConsumerControl.release();
    } else if (strcmp(btn.type, "volume_down") == 0) {
        ConsumerControl.press(CONSUMER_CONTROL_VOLUME_DECREMENT);
        delay(15);
        ConsumerControl.release();
    } else if (strcmp(btn.type, "volume_mute") == 0) {
        ConsumerControl.press(CONSUMER_CONTROL_MUTE);
        delay(15);
        ConsumerControl.release();
    } else { // "key" (tambem usado como fallback pra configs antigas)
        uint8_t keycode = Config::keyNameToCode(btn.key);
        if (keycode != 0) {
            Keyboard.press(keycode);
            delay(15);
            Keyboard.release(keycode);
        }
    }
}

// Chamado pelo SerialProtocol quando o app PC grava uma config nova.
void onConfigApplied(const DeckConfig &cfg) {
    deckConfig = cfg;
    pressedIndex = -1;
    if (mode == Mode::BUTTONS) drawAllButtons();
}

// Chamado pelo SerialProtocol depois de enviar/remover icone ou fundo.
void onAssetsChanged() {
    Background::reload();
    Usage::reloadBackground();
    pressedIndex = -1;
    if (mode == Mode::BUTTONS) drawAllButtons();
}

void drawNotifications() {
    Notification::draw(display.getCanvas());
    display.flush();
}

// Chamado pelo SerialProtocol ao receber {"cmd":"notify",...}. Guarda na
// lista e, se a tela de notificacoes ainda nao estava aberta, interrompe
// qualquer modo atual (botoes, status ou tela de descanso) pra mostra-la na
// hora (e lembra qual era, pra voltar pra ela ao fechar); se ja estava, so
// atualiza a lista.
void onNotify(const char *app, const char *title, const char *body) {
    Notification::add(app, title, body);
    notifyLastTouchMs = millis();
    lastActivityMs = millis();
    if (mode != Mode::NOTIFICATION) {
        notifyReturnMode = mode;
        mode = Mode::NOTIFICATION;
        notifyAutoOpened = true;
        notifyIdleReads = 0;
        Notification::open();
    }
    drawNotifications();
}

// Botao do tipo "notifications": abre a lista de notificacoes sob demanda
// (vazia, com um aviso, se nenhuma chegou ainda).
void enterNotificationScreen() {
    notifyReturnMode = Mode::BUTTONS;
    mode = Mode::NOTIFICATION;
    notifyAutoOpened = false;
    notifyLastTouchMs = millis();
    notifyIdleReads = 0;
    lastActivityMs = millis();
    Notification::open();
    drawNotifications();
}

// So aceita toque como "acordar" depois de ver o dedo solto pelo menos uma
// vez. Sem isso, entrar na tela de descanso pelo botao de voltar soltava o
// dedo bem no instante da troca de modo — o touch capacitivo as vezes ainda
// reporta contato por mais um ou dois ciclos durante o levantar do dedo, e
// esse resquicio do mesmo toque era lido como um novo toque "acordando" a
// tela na hora, dando a impressao de duplo clique.
bool screensaverArmed = false;

void enterScreensaver() {
    if (!Screensaver::hasGif()) return; // sem gif configurado, continua no grid
    if (Screensaver::start()) {
        mode = Mode::SCREENSAVER;
        screensaverArmed = false;
    }
}

void exitScreensaver() {
    Screensaver::stop();
    mode = Mode::BUTTONS;
    drawAllButtons();
}

// Fecha a tela de notificacoes voltando pra que estava antes (grid, status,
// uso ou tela de descanso). Se essa tela nao puder mais abrir (ex.: o GIF foi
// apagado), cai no grid.
void exitNotificationScreen(bool touchedNow) {
    mode = Mode::BUTTONS;
    switch (notifyReturnMode) {
        case Mode::SCREENSAVER: enterScreensaver(); break;
        case Mode::STATUS: enterStatus(); break;
        case Mode::USAGE: enterUsage(); break;
        default: break;
    }
    if (mode == Mode::BUTTONS) drawAllButtons();
    lastActivityMs = millis();
    swallowTouch = touchedNow;
    swallowIdleReads = 0;
}

void setup() {
    // O buffer de recepcao padrao do USB CDC e de 256 bytes; o set_config do
    // app chega numa unica escrita de >1KB, e o loop() so drena a serial a
    // cada volta (desenho/touch) — sem folga os bytes se perdem e o JSON
    // chega truncado (invalid_json). Precisa vir antes de Serial.begin().
    Serial.setRxBufferSize(4096);
    // O driver reinicia em modo de gravacao (bootloader) quando a porta recebe
    // a sequencia DTR/RTS do esptool ou 1200 baud — o que acontecia sozinho
    // quando o configurador/script abria e fechava a porta, deixando a placa
    // parada em download mode. Aqui regravamos com o botao BOOT, entao desliga.
    Serial.enableReboot(false);
    Serial.begin(115200);

    if (!LittleFS.begin(true)) {
        Serial.println("Falha ao montar LittleFS.");
    }
    Config::load(deckConfig);
    Background::reload();
    Usage::reloadBackground();

    if (!display.begin()) {
        Serial.println("Falha ao iniciar o display (ver PSRAM/pinos).");
        while (true) delay(1000);
    }
    if (!touch.begin()) {
        Serial.println("Falha ao iniciar o touch (I2C).");
    }

    display.setRotation(ROTATION_90); // paisagem: 480x320
    display.setTouchRotation(&touch);

    Screensaver::begin(display.getCanvas());
    drawAllButtons();

    USB.productName("ESP32-S3 Stream Deck");
    USB.manufacturerName("DIY");
    Keyboard.begin();
    ConsumerControl.begin();
    USB.begin();

    SerialProtocol::begin(onConfigApplied, onAssetsChanged, onNotify);

    lastActivityMs = millis();
}

void loop() {
    SerialProtocol::poll();

    if (mode == Mode::NOTIFICATION) {
        TouchPoint np;
        bool touchedNow = touch.read(np);
        if (!touchedNow && notifyIdleReads < RELEASE_DEBOUNCE_READS) notifyIdleReads++;
        Notification::Action act = Notification::Action::NONE;
        if (notifyIdleReads >= RELEASE_DEBOUNCE_READS) { // ignora o resto do toque que abriu a tela
            act = Notification::handleTouch(touchedNow, np.x, np.y);
            if (touchedNow) notifyLastTouchMs = millis();
        }
        // lendo uma mensagem completa nao fecha na pressa, mesmo que a tela tenha
        // aberto sozinha
        unsigned long idleLimit =
            notifyAutoOpened && !Notification::inDetail() ? NOTIFY_AUTO_CLOSE_MS : NOTIFY_MANUAL_CLOSE_MS;
        if (act == Notification::Action::CLOSE || millis() - notifyLastTouchMs > idleLimit) {
            exitNotificationScreen(touchedNow);
        } else if (Notification::tick(millis())) {
            drawNotifications();
        }
        delay(10);
        return;
    }

    // Dedo que fechou as notificacoes ainda encostado: espera soltar antes de
    // a tela de volta (grid, status, uso ou descanso) interpretar como toque.
    if (swallowTouch) {
        TouchPoint sw;
        if (touch.read(sw)) swallowIdleReads = 0;
        else if (++swallowIdleReads >= RELEASE_DEBOUNCE_READS) swallowTouch = false;
        delay(10);
        return;
    }

    if (mode == Mode::SCREENSAVER) {
        TouchPoint p;
        bool touchedNow = touch.read(p);
        if (!touchedNow) {
            screensaverArmed = true;
        } else if (screensaverArmed) {
            exitScreensaver();
            lastActivityMs = millis();
            return;
        }
        if (!Screensaver::step()) {
            Screensaver::start(); // chegou ao fim do gif, repete
        }
        return;
    }

    if (mode == Mode::STATUS) {
        TouchPoint sp;
        bool touchedNow = touch.read(sp);
        if (!touchedNow && statusIdleReads < RELEASE_DEBOUNCE_READS) statusIdleReads++;
        if (statusIdleReads >= RELEASE_DEBOUNCE_READS) { // ignora o resto do toque que abriu a tela
            if (touchedNow) lastActivityMs = millis();
            if (Status::handleTouch(touchedNow, sp.x, sp.y) == Status::Action::CLOSE) {
                exitStatus(touchedNow);
                lastActivityMs = millis();
                return;
            }
        }
        Status::tick(millis());
        if (millis() - lastStatusRequestMs > STATUS_REFRESH_MS) {
            requestStatus();
            Status::markDirty(); // redesenha pra atualizar o aviso de "sem atualizar"
        }
        bool statusDirty = Status::consumeDirty();
        if (statusDirty || millis() - lastStatusDrawMs >= STATUS_FRAME_MS) {
            Status::draw(display.getCanvas());
            display.flush();
            lastStatusDrawMs = millis();
        }
        delay(10);
        return;
    }

    if (mode == Mode::USAGE) {
        TouchPoint up;
        if (touch.read(up)) {
            usageTouching = true;
            lastActivityMs = millis();
        } else if (usageTouching) { // soltou o dedo: volta pro grid
            exitUsage();
            lastActivityMs = millis();
            return;
        }
        if (millis() - lastUsageRequestMs > USAGE_REFRESH_MS) requestUsage();
        if (millis() - lastUsageDrawMs >= USAGE_FRAME_MS) drawUsage();
        delay(10);
        return;
    }

    TouchPoint p;
    bool touched = touch.read(p);

    // Anima os icones (Claude, Health, Avisos, Voltar) enquanto nao ha toque no grid
    if (!touched && pressedIndex == -1 && animCount > 0 && millis() - animDrawnAt >= ANIM_FRAME_MS) {
        for (int k = 0; k < animCount; k++) drawAnimFrame(animButtons[k]);
        display.flush();
        animDrawnAt = millis();
    }

    if (touched) {
        lastActivityMs = millis();
        releaseStreak = 0; // toque confirmado nesse ciclo, cancela soltura pendente
        int idx = hitTestButton(p.x, p.y);
        if (pressedIndex == -1 && idx >= 0) {
            pressedIndex = idx;
            drawButton(pressedIndex, true);
        }
    } else if (pressedIndex != -1 && ++releaseStreak >= RELEASE_DEBOUNCE_READS) {
        int idx = pressedIndex;
        pressedIndex = -1;
        releaseStreak = 0;
        drawButton(idx, false);
        triggerAction(idx);
    }

    if (millis() - lastActivityMs > deckConfig.idleTimeoutMs) {
        enterScreensaver();
    }

    delay(10);
}
