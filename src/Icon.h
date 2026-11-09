/**
 * Icones dos botoes: bitmaps RGBA8888 64x64 fixos, gravados em
 * /icon<indice>.rgba na particao LittleFS (enviados pelo app de
 * configuracao). Desenhados com alpha-blend sobre a cor de fundo do botao.
 */
#pragma once

#include <Arduino_GFX_Library.h>

namespace Icon {
    constexpr int SIZE = 64;
    constexpr size_t BYTES = (size_t)SIZE * SIZE * 4; // RGBA8888

    bool exists(uint8_t index);

    // Desenha o icone do botao `index` com canto superior-esquerdo em (x,y),
    // misturando (alpha-blend) com `bgColor565` (a cor de fundo ja desenhada
    // no botao). Nao faz nada (retorna false) se o arquivo nao existir ou
    // tiver tamanho invalido.
    bool draw(Arduino_Canvas *canvas, uint8_t index, int16_t x, int16_t y, uint16_t bgColor565);

    // Mistura o icone do botao `index` direto num buffer RGB565 (bufW x bufH)
    // com canto superior-esquerdo em (x,y) dentro dele, usando os pixels que ja
    // estao no buffer como fundo (ex: caixa translucida sobre a foto de fundo).
    // Retorna false (sem alterar nada) se nao houver icone valido.
    bool blendInto(uint16_t *buf, int bufW, int bufH, uint8_t index, int x, int y);
}
