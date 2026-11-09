/**
 * Tela de uso da assinatura do Claude (botao do tipo "usage"): janelas de 5
 * horas e 7 dias, em porcentagem, com quanto falta pra cada uma reiniciar.
 * Os dados vem do PC pela serial (comando "usage", ver SerialProtocol.h): a
 * ponte tools/bridge.ps1 le o snapshot do tools/claude_usage.py (que e quem
 * consulta a API) e empurra pra ca. Esta tela so guarda o ultimo snapshot e
 * desenha os cards; a contagem regressiva anda localmente entre atualizacoes.
 *
 * Tem um fundo proprio (opcional, enviado por tools/send-usage-bg.ps1) com os
 * cards translucidos por cima. Fica na flash comprimido (RLE), porque o
 * bitmap cru (300 KB) nao cabe na particao pequena junto com o GIF e o fundo
 * da grade; imagens quase chapadas ficam em poucos KB.
 */
#pragma once

#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>

#define USAGE_BG_PATH "/usage_bg.rle"
#define USAGE_BG_TMP_PATH "/usage_bg.tmp"
#define USAGE_BG_W 480
#define USAGE_BG_H 320
#define USAGE_BG_PIXELS ((size_t)USAGE_BG_W * USAGE_BG_H)
#define USAGE_BG_BYTES (USAGE_BG_PIXELS * 2)
#define USAGE_BG_MAX_BYTES ((size_t)150000) // maximo do arquivo comprimido

namespace Usage {
    // (Re)carrega o fundo da PSRAM a partir de /usage_bg.rle; sem arquivo
    // valido, a tela usa um fundo liso. Chamar no setup() e depois de
    // enviar/remover o fundo.
    void reloadBackground();

    // Atualiza o snapshot a partir do comando serial {"cmd":"usage",...}.
    void update(JsonDocument &doc);

    // Desenha a tela inteira.
    void draw(Arduino_Canvas *canvas);

    // Desenha um quadro. Chamar varias vezes por segundo: o mascote pula e a
    // contagem regressiva anda a partir do millis(). So redesenha a tela toda
    // quando algo mudou (dado novo, texto que anda sozinho); fora isso, so o
    // mascote, o que e bem mais rapido.
    void drawFrame(Arduino_Canvas *canvas);

    // Forca o proximo drawFrame() a redesenhar tudo (ao abrir a tela, pois
    // o canvas foi usado por outra).
    void invalidate();
}
