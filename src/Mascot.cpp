#include "Mascot.h"

#include <cmath>

namespace {

// '#' corpo, 'E' olho (celula do rosto; o traco do olho e desenhado por cima).
const char *const SPRITE[Mascot::CELLS_H] = {
    "..########..",
    "..#EE##EE#..",
    "############",
    "############",
    "..########..",
    "..########..",
    "..#.#..#.#..",
    "..#.#..#.#..",
};

constexpr unsigned long TURN_MS = 2400;    // ida e volta do giro
constexpr unsigned long STEP_MS = 350;     // troca das perninhas
constexpr unsigned long BLINK_PERIOD_MS = 2800, BLINK_MS = 270; // pisca a cada ~2,8 s
constexpr int LEG_ROW = Mascot::CELLS_H - 1; // ultima linha: perninha "levantada" some

// Perninhas (colunas) que ficam levantadas em cada fase do passo.
bool legLifted(int col, bool phase) {
    return phase ? (col == 2 || col == 9) : (col == 4 || col == 7);
}

} // namespace

namespace Mascot {

void draw(Arduino_Canvas *cv, int cx, int groundY, int scale, unsigned long now, const Colors &c) {
    // yaw: -1..1. Positivo = rosto virado pra direita, lateral escura na esquerda.
    const float yaw = sinf(2.0f * 3.14159265f * (now % TURN_MS) / TURN_MS);
    const int dir = yaw >= 0 ? 1 : -1;
    const int strip = (int)lroundf(fabsf(yaw) * scale * 1.4f); // espessura visivel
    const int shift = (int)lroundf(yaw * scale * 0.7f);        // o corpo acompanha o giro
    const bool stepPhase = (now / STEP_MS) % 2 == 0;
    const unsigned long blinkT = now % BLINK_PERIOD_MS; // < BLINK_MS: piscando

    const int x0 = cx - CELLS_W * scale / 2 + shift;
    const int top = groundY - CELLS_H * scale;

    cv->fillEllipse(cx + shift, groundY + 2, CELLS_W * scale / 2 - 3, 2, c.shadow);

    // Lateral: cada celula "empurrada" na horizontal pro lado oposto do giro.
    if (strip > 0) {
        for (int r = 0; r < CELLS_H; r++) {
            for (int col = 0; col < CELLS_W; col++) {
                if (SPRITE[r][col] == '.') continue;
                if (r == LEG_ROW && legLifted(col, stepPhase)) continue;
                int x = x0 + col * scale;
                int from = dir > 0 ? x - strip : x;
                cv->fillRect(from, top + r * scale, scale + strip, scale, c.side);
            }
        }
    }

    // Frente.
    for (int r = 0; r < CELLS_H; r++) {
        for (int col = 0; col < CELLS_W; col++) {
            if (SPRITE[r][col] == '.') continue;
            if (r == LEG_ROW && legLifted(col, stepPhase)) continue;
            cv->fillRect(x0 + col * scale, top + r * scale, scale, scale, c.face);
        }
    }

    // Olhos: retangulos escuros no meio de cada par de celulas 'E', deslizando
    // junto com o giro. Ao piscar fecham ate virar um traco e reabrem.
    const int eyeW = scale * 5 / 3 > 2 ? scale * 5 / 3 : 2;
    const int openH = scale * 4 / 3 > 2 ? scale * 4 / 3 : 2;
    const int eyeGap = scale * 2 / 3; // quanto cada olho se afasta do meio do rosto
    int eyeH = openH;
    if (blinkT < BLINK_MS) {
        eyeH = (blinkT * 3 / BLINK_MS) == 1 ? 1 : (openH / 2 > 1 ? openH / 2 : 1); // meio, fechado, meio
    }
    const int eyeShift = (int)lroundf(yaw * scale * 0.7f);
    const int eyeCy = top + scale * 4 / 3; // centro do olho aberto
    int pair = 0;
    for (int col = 0; col + 1 < CELLS_W; col++) {
        if (SPRITE[1][col] != 'E' || SPRITE[1][col + 1] != 'E') continue;
        int cx0 = x0 + (col + 1) * scale + eyeShift; // fronteira entre as duas celulas
        cx0 += pair++ == 0 ? -eyeGap : eyeGap;       // olho esquerdo pra esquerda, direito pra direita
        cv->fillRect(cx0 - eyeW / 2, eyeCy - eyeH / 2, eyeW, eyeH, c.eye);
        col++; // pula a segunda celula do par
    }
}

} // namespace Mascot
