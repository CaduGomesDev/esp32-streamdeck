/**
 * Tela de descanso: toca em loop um GIF animado gravado em /screensaver.gif
 * na particao LittleFS (enviado pelo app PC via protocolo serial).
 */
#pragma once

#include <Arduino_GFX_Library.h>

#define GIF_PATH "/screensaver.gif"
#define GIF_TMP_PATH "/screensaver.tmp"

// Deixa margem na particao "spiffs" (0x180000 = 1.5MB) para o config.json,
// os icones dos botoes (ate 8 x 16KB) e metadados do LittleFS.
#define MAX_GIF_SIZE (1000UL * 1024UL)

namespace Screensaver {
    void begin(Arduino_Canvas *canvas);

    bool hasGif();

    // Abre (ou reabre) o GIF do inicio. Retorna false se nao houver arquivo
    // valido.
    bool start();

    void stop();

    // Decodifica e desenha um frame (bloqueia pelo delay proprio do frame).
    // Retorna false quando o GIF terminou (chamador deve chamar start() de
    // novo para repetir) ou se nao houver GIF.
    bool step();
}
