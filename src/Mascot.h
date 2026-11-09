/**
 * Mascote do Claude Code em pixel art, igual ao da tela de carregamento do app
 * do Claude: bracos abertos, olhos em traco e quatro perninhas. Ele "dança"
 * girando de um lado pro outro como um bloco 3D: aparece uma faixa lateral mais
 * escura (a espessura do corpo), ora de um lado, ora do outro, os olhos
 * acompanham o giro e as perninhas se alternam.
 *
 * Usado pela tela de uso (Usage.cpp) e pelo botao do Claude na grade (main.cpp).
 */
#pragma once

#include <Arduino_GFX_Library.h>

namespace Mascot {
    struct Colors {
        uint16_t face;   // frente do corpo
        uint16_t side;   // lateral (mais escura)
        uint16_t eye;
        uint16_t shadow; // sombra no chao
    };

    // Tamanho do desenho em celulas (sem giro). Com o giro, ele sai ate 2 celulas
    // pra cada lado do centro (deslocamento + faixa lateral).
    constexpr int CELLS_W = 12, CELLS_H = 8;

    // Desenha com o centro em `cx` e os pes em `groundY`, `scale` pixels por
    // celula; a fase da danca sai do millis() passado em `now`.
    void draw(Arduino_Canvas *cv, int cx, int groundY, int scale, unsigned long now, const Colors &c);
}
