/**
 * Imagem de fundo da grade de botoes: bitmap cru RGB565 480x320 (paisagem),
 * gravado em /background.rgb565 na LittleFS (enviado pelo app de
 * configuracao, que converte e recorta a imagem). Fica carregado na PSRAM
 * pra redesenhar os botoes (translucidos) sem reler a flash.
 */
#pragma once

#include <Arduino.h>

#define BG_PATH "/background.rgb565"
#define BG_TMP_PATH "/background.tmp"
#define BG_WIDTH 480
#define BG_HEIGHT 320
#define BG_BYTES ((size_t)BG_WIDTH * BG_HEIGHT * 2)

namespace Background {
    // (Re)carrega o arquivo na PSRAM; libera a memoria se nao houver arquivo
    // valido. Chamar no setup() e depois de enviar/remover o fundo.
    void reload();

    // nullptr se nao houver fundo. Linhas de BG_WIDTH pixels, RGB565 nativo.
    const uint16_t *pixels();
}
