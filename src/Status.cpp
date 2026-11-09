#include "Status.h"

#include <cmath>

// Visual espelhado do painel web (health-server/vps/dashboard/public/index.html):
// mesmo tema terminal verde, cabecalho com logo + badge do broker e um card por
// PC (online ou offline) com medidores e barra de estabilidade. Os cards sao
// altos (em pe), 3 por tela; arrastar o dedo pro lado passa de 3 em 3.

namespace {

constexpr int SCREEN_W = 480;
constexpr int SCREEN_H = 320;
constexpr int HEADER_H = 40;
constexpr int PAGE_SIZE = 3;       // cards por tela
constexpr int MAX_PCS = 24;
constexpr int MAX_HIST = 24;
constexpr unsigned long STALE_MS = 6000; // sem dado novo do PC por esse tempo = aviso

struct Pc {
    char name[24];
    char host[24];
    char ip[20];
    char so[28];
    char since[16]; // "online desde", ja formatado pelo PC (dd/MM HH:mm)
    bool online;
    int8_t cpu, ram, disk; // -1 = desconhecido
    uint32_t uptimeSec;
    uint32_t silentSec;
    uint16_t events24;
    uint8_t streak, streakTotal;
    uint8_t histCount;
    uint8_t hist[MAX_HIST]; // 1 = online, 0 = falhou (mais antigo primeiro)
};

Pc gPcs[MAX_PCS];
int gCount = 0;
bool gHasData = false;
bool gBroker = true;
char gError[48] = "";
bool gDirty = true;
unsigned long gLastUpdateMs = 0;

float gScroll = 0, gTarget = 0, gVel = 0; // rolagem horizontal em px; gTarget = pagina de destino
bool gSettling = false;                    // animando ate a pagina
unsigned long gLastTick = 0;
bool gDown = false, gMoved = false, gPressBack = false;
int gStartX = 0, gStartY = 0, gLastX = 0, gStartPage = 0;
uint8_t gUpStreak = 0;

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Mesmas cores do painel (:root do index.html), ja misturadas onde o CSS usa alpha.
const uint16_t C_BG = rgb(6, 11, 14);
const uint16_t C_GRID = rgb(9, 22, 23);
const uint16_t C_PANEL = rgb(11, 20, 25);
const uint16_t C_BORDER = rgb(38, 61, 54);
const uint16_t C_ONLINE_BORDER = rgb(23, 77, 63);
const uint16_t C_OFFLINE_BORDER = rgb(95, 53, 63);
const uint16_t C_OFFLINE_PANEL = rgb(26, 20, 28);
const uint16_t C_TRACK = rgb(14, 22, 30);
const uint16_t C_TEXT = rgb(215, 224, 233);
const uint16_t C_BRIGHT = rgb(238, 244, 249);
const uint16_t C_MUTED = rgb(147, 164, 181);
const uint16_t C_SO = rgb(143, 179, 162);
const uint16_t C_ACCENT = rgb(110, 231, 183);
const uint16_t C_AMBER = rgb(252, 211, 77);
const uint16_t C_RED = rgb(251, 113, 133);

// Mistura `fg` sobre `bg` (RGB565) com opacidade `a` (0-255) do fg.
uint16_t mix(uint16_t bg, uint16_t fg, uint8_t a) {
    uint32_t r = (((fg >> 11) & 0x1F) * a + ((bg >> 11) & 0x1F) * (255 - a)) / 255;
    uint32_t g = (((fg >> 5) & 0x3F) * a + ((bg >> 5) & 0x3F) * (255 - a)) / 255;
    uint32_t b = ((fg & 0x1F) * a + (bg & 0x1F) * (255 - a)) / 255;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

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

void text(Arduino_Canvas *cv, int x, int y, int size, uint16_t color, const char *s) {
    cv->setTextSize(size);
    cv->setTextColor(color);
    cv->setCursor(x, y);
    cv->print(s);
}

void textRight(Arduino_Canvas *cv, int right, int y, int size, uint16_t color, const char *s) {
    text(cv, right - textW(s, size), y, size, color, s);
}

// Escreve `s` cortado pra caber em maxW pixels (fonte tamanho 1).
void textClip(Arduino_Canvas *cv, int x, int y, int maxW, uint16_t color, const char *s) {
    char buf[64];
    int maxChars = maxW / 6;
    if (maxChars > (int)sizeof(buf) - 1) maxChars = sizeof(buf) - 1;
    strncpy(buf, s, maxChars);
    buf[maxChars] = '\0';
    text(cv, x, y, 1, color, buf);
}

// Igual ao formatarTempo() do painel: "3d 4h", "2h 15min", "7min".
void fmtDuration(uint32_t sec, char *out, size_t n) {
    uint32_t d = sec / 86400, h = (sec % 86400) / 3600, m = (sec % 3600) / 60;
    if (d > 0) snprintf(out, n, "%ud %uh", (unsigned)d, (unsigned)h);
    else if (h > 0) snprintf(out, n, "%uh %umin", (unsigned)h, (unsigned)m);
    else snprintf(out, n, "%umin", (unsigned)m);
}

// Igual ao corNivel() do painel.
uint16_t levelColor(int pct) {
    if (pct >= 90) return C_RED;
    if (pct >= 70) return C_AMBER;
    return C_ACCENT;
}

void dashedHLine(Arduino_Canvas *cv, int x, int y, int w, uint16_t color) {
    for (int i = 0; i < w; i += 7) cv->drawFastHLine(x + i, y, (w - i < 4) ? (w - i) : 4, color);
}

// Linha de medidor: rotulo, trilho com preenchimento, valor (como .meter-row).
// bh = espessura da barra (6 normal, 8 quando o card tem folga).
void meterRow(Arduino_Canvas *cv, int x, int y, int w, const char *label, int pct, int bh = 6) {
    if (pct < 0) return;
    if (pct > 100) pct = 100;
    text(cv, x, y, 1, C_MUTED, label);
    int bx = x + 32, bw = w - 32 - 28;
    int by = y + 4 - bh / 2;
    cv->fillRoundRect(bx, by, bw, bh, bh / 2, C_TRACK);
    int fw = bw * pct / 100;
    if (fw < bh && pct > 0) fw = bh;
    if (fw > 0) cv->fillRoundRect(bx, by, fw, bh, bh / 2, levelColor(pct));
    char val[8];
    snprintf(val, sizeof(val), "%d%%", pct);
    textRight(cv, x + w, y, 1, rgb(216, 245, 228), val);
}

// Barra de segmentos do historico de heartbeat (.streak-bar).
void streakBar(Arduino_Canvas *cv, int x, int y, int w, const Pc &pc, int h = 4) {
    if (pc.histCount == 0) return;
    int n = pc.histCount;
    int seg = w / n;
    if (seg < 2) seg = 2;
    for (int i = 0; i < n; i++) {
        uint16_t c = pc.hist[i] ? C_ACCENT : C_RED;
        if (i == n - 1) { // o batimento mais recente pulsa
            float p = 0.5f + 0.5f * sinf(millis() * 0.005f);
            c = mix(c, 0xFFFF, (uint8_t)(110 * p));
        }
        cv->fillRect(x + i * seg, y, seg - 1, h, c);
    }
}

// LED de status do card: online = ponto verde com um anel que se expande e
// some; offline = ponto vermelho piscando.
void statusLed(Arduino_Canvas *cv, int x, int y, bool online, uint16_t panel) {
    unsigned long now = millis();
    if (online) {
        float ph = (now % 1800) / 1800.0f;
        cv->drawCircle(x, y, 3 + (int)(ph * 5), mix(panel, C_ACCENT, (uint8_t)((1.0f - ph) * 190)));
        cv->fillCircle(x, y, 2, C_ACCENT);
    } else {
        cv->fillCircle(x, y, 2, (now % 1000) < 500 ? C_RED : mix(panel, C_RED, 70));
    }
}

void badge(Arduino_Canvas *cv, int right, int y, const char *label, uint16_t color) {
    int bw = textW(label, 1) + 10;
    cv->drawRoundRect(right - bw, y, bw, 12, 3, color);
    text(cv, right - bw + 5, y + 2, 1, color, label);
}

// Icone "square-terminal" do Lucide (grade 24, o mesmo do site): quadrado
// arredondado, ">" pequeno e um traco. Aqui em 22 px, com 2 px de espessura.
void drawLogo(Arduino_Canvas *cv, int x, int y) {
    cv->drawRoundRect(x + 2, y + 2, 19, 19, 4, C_ACCENT);
    cv->drawRoundRect(x + 3, y + 3, 17, 17, 3, C_ACCENT);
    for (int d = 0; d < 2; d++) {
        cv->drawLine(x + 6 + d, y + 10, x + 8 + d, y + 8, C_ACCENT);
        cv->drawLine(x + 6 + d, y + 6, x + 8 + d, y + 8, C_ACCENT);
    }
    if ((millis() / 530) % 2 == 0) cv->fillRect(x + 10, y + 11, 5, 2, C_ACCENT); // cursor piscando
}

// Card alto de um PC (como no painel): nome + LED, host + badge, ip, SO, tempo
// online, medidores, desde/eventos e streak com a barra de heartbeat. A folga
// que sobra da altura do card e repartida entre as linhas.
void drawCard(Arduino_Canvas *cv, const Pc &pc, int cx, int cy, int cw, int ch) {
    cv->fillRoundRect(cx, cy, cw, ch, 4, pc.online ? C_PANEL : C_OFFLINE_PANEL);
    cv->drawRoundRect(cx, cy, cw, ch, 4, pc.online ? C_ONLINE_BORDER : C_OFFLINE_BORDER);
    if (!pc.online) cv->drawFastHLine(cx + 3, cy, cw - 6, C_RED); // faixa vermelha do topo (.card.offline::before)

    const int x = cx + 8, w = cw - 16;
    // 193 e a altura minima do conteudo; GAPS e quantas linhas recebem a folga.
    const int BASE_H = 193, GAPS = 12;
    int pad = (ch - BASE_H) / GAPS;
    if (pad < 0) pad = 0;
    if (pad > 8) pad = 8;
    const int bh = pad >= 4 ? 8 : 6;
    int y = cy + 8 + pad / 2;

    textClip(cv, x, y, w - 20, C_BRIGHT, pc.name);
    statusLed(cv, x + w - 8, y + 4, pc.online, pc.online ? C_PANEL : C_OFFLINE_PANEL);
    y += 12 + pad;

    char buf[32];
    snprintf(buf, sizeof(buf), "[%s]", pc.host);
    textClip(cv, x, y + 2, w - 34, C_MUTED, buf);
    badge(cv, x + w, y, pc.online ? "OK" : "OFFLINE", pc.online ? C_ACCENT : C_RED);
    y += 18 + pad;

    text(cv, x, y, 1, C_ACCENT, "$");
    textClip(cv, x + 12, y, w - 12, C_TEXT, pc.ip);
    y += 11 + pad;
    if (pc.so[0]) {
        textClip(cv, x, y, w, C_SO, pc.so);
        y += 11 + pad;
    }

    char dur[16];
    if (pc.online) {
        fmtDuration(pc.uptimeSec + pc.silentSec, dur, sizeof(dur));
        snprintf(buf, sizeof(buf), "online for %s", dur);
        textClip(cv, x, y, w, C_TEXT, buf);
    } else {
        fmtDuration(pc.silentSec, dur, sizeof(dur));
        snprintf(buf, sizeof(buf), "no signal for %s", dur);
        textClip(cv, x, y, w, C_RED, buf);
    }
    y += 16 + pad;

    if (pc.online) {
        meterRow(cv, x, y, w, "CPU", pc.cpu, bh);
        y += 15 + pad;
        meterRow(cv, x, y, w, "RAM", pc.ram, bh);
        y += 15 + pad;
        meterRow(cv, x, y, w, "DISCO", pc.disk, bh);
        y += 18 + pad;
    }

    dashedHLine(cv, x, y, w, C_BORDER);
    y += 7 + pad;

    if (pc.since[0]) {
        text(cv, x, y, 1, C_MUTED, "since");
        textRight(cv, x + w, y, 1, C_TEXT, pc.since);
        y += 12 + pad;
    }
    text(cv, x, y, 1, C_MUTED, "events 24h");
    snprintf(buf, sizeof(buf), "%u", (unsigned)pc.events24);
    textRight(cv, x + w, y, 1, C_TEXT, buf);
    y += 12 + pad;

    if (pc.streakTotal == 0) {
        text(cv, x, y, 1, C_MUTED, "STREAK --");
        textRight(cv, x + w, y, 1, C_MUTED, "WAITING");
    } else {
        snprintf(buf, sizeof(buf), "STREAK %u/%u", (unsigned)pc.streak, (unsigned)pc.streakTotal);
        text(cv, x, y, 1, C_MUTED, buf);
        bool stable = pc.streak >= pc.streakTotal;
        textRight(cv, x + w, y, 1, stable ? C_ACCENT : C_AMBER, stable ? "STABLE" : "UNSTABLE");
    }
    y += 13;
    const int sh = pad >= 4 ? 6 : 4;
    if (y + sh <= cy + ch - 4) streakBar(cv, x, y, w, pc, sh);
}

// Paginas de PAGE_SIZE cards; a rolagem e em pixels e encaixa de pagina em pagina.
int pageCount() { return gCount > 0 ? (gCount + PAGE_SIZE - 1) / PAGE_SIZE : 1; }

int maxScroll() { return (pageCount() - 1) * SCREEN_W; }

void clampScroll() {
    float m = (float)maxScroll();
    if (gScroll < 0) gScroll = 0;
    if (gScroll > m) gScroll = m;
    if (gTarget > m) gTarget = m;
}

// Bolinhas de pagina embaixo (a atual em destaque).
void drawPageDots(Arduino_Canvas *cv) {
    int n = pageCount();
    if (n <= 1) return;
    int cur = (int)lroundf(gScroll / SCREEN_W);
    int x0 = (SCREEN_W - (n - 1) * 14) / 2;
    for (int i = 0; i < n; i++) {
        if (i == cur) cv->fillCircle(x0 + i * 14, SCREEN_H - 7, 3, C_ACCENT);
        else cv->fillCircle(x0 + i * 14, SCREEN_H - 7, 2, C_BORDER);
    }
}

void line2(Arduino_Canvas *cv, int x0, int y0, int x1, int y1, uint16_t c) {
    cv->drawLine(x0, y0, x1, y1, c);
    cv->drawLine(x0 + 1, y0, x1 + 1, y1, c);
    cv->drawLine(x0, y0 + 1, x1, y1 + 1, c);
}

// Cabecalho fixo por cima da lista: voltar, logo, titulo e badge do broker.
void drawHeader(Arduino_Canvas *cv) {
    const int W = SCREEN_W;
    cv->fillRect(0, 0, W, HEADER_H, C_BG);
    cv->drawFastHLine(0, HEADER_H - 1, W, C_BORDER);

    uint16_t back = gPressBack ? C_MUTED : C_BRIGHT;
    line2(cv, 24, 12, 16, 20, back);
    line2(cv, 16, 20, 24, 28, back);

    drawLogo(cv, 30, 8);
    text(cv, 58, 13, 2, C_BRIGHT, "HEALTH SERVER");
    text(cv, 58 + textW("HEALTH SERVER ", 2), 13, 2, C_ACCENT, "MONITOR");

    const char *hdr;
    uint16_t hc;
    if (gError[0]) {
        hdr = gError;
        hc = C_RED;
    } else if (gHasData && millis() - gLastUpdateMs > STALE_MS) {
        hdr = "STALE DATA";
        hc = C_AMBER;
    } else if (gHasData && !gBroker) {
        hdr = "BROKER: OFFLINE";
        hc = C_RED;
    } else if (gHasData) {
        hdr = "BROKER: ONLINE";
        hc = C_ACCENT;
    } else {
        hdr = "";
        hc = C_MUTED;
    }
    if (!hdr[0]) return;
    int bw = textW(hdr, 1) + 24;
    if (bw > W - 330) bw = W - 330;
    const int bx = W - 10 - bw;
    cv->drawRoundRect(bx, 11, bw, 18, 3, hc);
    float ringPh = (millis() % 1800) / 1800.0f; // anel que se expande no ponto do badge
    cv->drawCircle(bx + 9, 20, 3 + (int)(ringPh * 3), mix(C_BG, hc, (uint8_t)((1.0f - ringPh) * 170)));
    cv->fillCircle(bx + 9, 20, 2, hc);
    char clipped[40];
    int maxChars = (bw - 24) / 6;
    if (maxChars > (int)sizeof(clipped) - 1) maxChars = sizeof(clipped) - 1;
    strncpy(clipped, hdr, maxChars);
    clipped[maxChars] = '\0';
    text(cv, bx + 16, 16, 1, hc, clipped);
}

} // namespace

namespace Status {

void update(JsonDocument &doc) {
    const char *err = doc["error"] | (const char *)nullptr;
    if (err) {
        copyAscii(gError, sizeof(gError), err);
        gDirty = true;
        return;
    }
    gError[0] = '\0';

    gBroker = doc["broker"] | true;
    gCount = 0;
    JsonArrayConst arr = doc["pcs"].as<JsonArrayConst>();
    for (JsonObjectConst o : arr) {
        if (gCount >= MAX_PCS) break;
        Pc &pc = gPcs[gCount++];
        copyAscii(pc.name, sizeof(pc.name), o["n"] | "?");
        copyAscii(pc.host, sizeof(pc.host), o["h"] | "");
        copyAscii(pc.ip, sizeof(pc.ip), o["ip"] | "");
        copyAscii(pc.so, sizeof(pc.so), o["so"] | "");
        copyAscii(pc.since, sizeof(pc.since), o["sn"] | "");
        pc.online = (o["on"] | 0) != 0;
        pc.cpu = (int8_t)(o["c"] | -1);
        pc.ram = (int8_t)(o["r"] | -1);
        pc.disk = (int8_t)(o["d"] | -1);
        pc.uptimeSec = o["u"] | 0;
        pc.silentSec = o["s"] | 0;
        pc.events24 = o["e"] | 0;
        pc.streak = o["st"] | 0;
        pc.streakTotal = o["stt"] | 0;
        pc.histCount = 0;
        const char *hi = o["hi"] | "";
        for (int i = 0; hi[i] && pc.histCount < MAX_HIST; i++) pc.hist[pc.histCount++] = hi[i] == '1';
    }
    gHasData = true;
    clampScroll();
    gLastUpdateMs = millis();
    gDirty = true;
}

void markDirty() { gDirty = true; }

bool consumeDirty() {
    bool d = gDirty;
    gDirty = false;
    return d;
}

void open() {
    gScroll = 0;
    gTarget = 0;
    gSettling = false;
    gVel = 0;
    gDown = false;
    gMoved = false;
    gPressBack = false;
    gUpStreak = 0;
    gLastTick = millis();
    gDirty = true;
}

Action handleTouch(bool touched, int x, int y) {
    Action act = Action::NONE;
    if (touched) {
        gUpStreak = 0;
        if (!gDown) {
            gDown = true;
            gMoved = false;
            gSettling = false;
            gStartX = x;
            gStartY = y;
            gLastX = x;
            gVel = 0;
            gStartPage = (int)lroundf(gScroll / SCREEN_W);
            gPressBack = y < HEADER_H && x < 70;
            gDirty = true;
        } else {
            if (!gMoved && (abs(x - gStartX) > 10 || abs(y - gStartY) > 10)) {
                gMoved = true;
                gPressBack = false;
                gDirty = true;
            }
            if (gMoved && gStartY >= HEADER_H) {
                float dx = (float)(gLastX - x);
                gScroll += dx;
                gVel = dx;
                clampScroll();
                gDirty = true;
            }
            gLastX = x;
        }
    } else if (gDown && ++gUpStreak >= 2) { // soltou (ignora 1 leitura falha do touch)
        gDown = false;
        if (!gMoved && gPressBack) act = Action::CLOSE;
        if (gMoved && gStartY >= HEADER_H) { // encaixa na pagina: arrasto longo ou rapido troca de pagina
            float off = gScroll - (float)gStartPage * SCREEN_W;
            int page = gStartPage;
            if (off > 80 || gVel > 8) page++;
            else if (off < -80 || gVel < -8) page--;
            if (page < 0) page = 0;
            if (page > pageCount() - 1) page = pageCount() - 1;
            gTarget = (float)page * SCREEN_W;
            gSettling = true;
        }
        gPressBack = false;
        gDirty = true;
    }
    return act;
}

void tick(unsigned long now) {
    unsigned long dt = now - gLastTick;
    gLastTick = now;
    if (dt > 100) dt = 100;
    if (!gDown && gSettling) {
        float diff = gTarget - gScroll;
        if (fabsf(diff) < 1.0f) {
            gScroll = gTarget;
            gSettling = false;
        } else {
            gScroll += diff * (1.0f - powf(0.75f, dt / 16.0f));
        }
        gDirty = true;
    }
}

void draw(Arduino_Canvas *cv) {
    const int W = SCREEN_W, H = SCREEN_H;
    cv->fillScreen(C_BG);
    int off = (int)lroundf(gScroll);
    for (int x = -(off % 32); x < W; x += 32) cv->drawFastVLine(x, 0, H, C_GRID); // fundo em grade do painel
    for (int y = 0; y < H; y += 32) cv->drawFastHLine(0, y, W, C_GRID);

    if (!gHasData || gCount == 0) {
        bool noneListed = gHasData && !gError[0]; // dados chegaram, mas a lista veio vazia
        const char *l1 = gError[0] ? "Nao foi possivel obter os dados"
                         : noneListed ? "Nenhum servidor cadastrado" : "Aguardando dados do PC...";
        int s1 = textW(l1, 2) <= W - 20 ? 2 : 1;
        text(cv, (W - textW(l1, s1)) / 2, H / 2 - 20, s1, C_TEXT, l1);
        if (!noneListed) {
            const char *l2 = "Rode tools/bridge.ps1 no PC";
            text(cv, (W - textW(l2, 1)) / 2, H / 2 + 10, 1, C_MUTED, l2);
        }
    } else {
        const int margin = 8, cgap = 6;
        const int cw = (W - 2 * margin - (PAGE_SIZE - 1) * cgap) / PAGE_SIZE;
        const int top = HEADER_H + 4, ch = H - 14 - top;
        for (int i = 0; i < gCount; i++) {
            int cx = (i / PAGE_SIZE) * W + margin + (i % PAGE_SIZE) * (cw + cgap) - off;
            if (cx + cw <= 0 || cx >= W) continue;
            drawCard(cv, gPcs[i], cx, top, cw, ch);
        }
        drawPageDots(cv);
    }
    drawHeader(cv); // por cima dos cards que passam por baixo dele
}

} // namespace Status
