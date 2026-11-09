/**
 * Configuracao dos botoes do deck, persistida em /config.json na particao
 * LittleFS. Permite que o app PC leia/grave sem recompilar o firmware.
 */
#pragma once

#include <Arduino.h>

#define MAX_BUTTONS 8
#define CONFIG_PATH "/config.json"
#define MACRO_TEXT_MAX 64

// Tipos de acao de um botao:
//  "key"         - pressiona uma unica tecla (campo `key`, ex: "F13")
//  "macro"       - digita texto (campo `macroText`), opcionalmente abrindo
//                  antes com Win+R (macroWinR) e finalizando com Enter
//                  (macroEnter). Ex: abrir um site no navegador padrao.
//  "volume_up"   - tecla de midia: aumentar volume do sistema
//  "volume_down" - tecla de midia: diminuir volume do sistema
//  "volume_mute" - tecla de midia: mudo/desmudo
//  "status"      - abre a tela de status dos servidores (ver Status.h); os
//                  dados chegam do PC pela serial
//  "screensaver"   - volta na hora para a tela de descanso (ver Screensaver.h)
//  "notifications" - abre a lista de notificacoes (ver Notification.h)
//                    recebidas do PC
//  "usage"         - abre a tela de uso da assinatura do Claude (ver Usage.h);
//                    os dados chegam do PC pela serial
struct ButtonConfig {
    char label[16];
    char type[16]; // precisa caber "notifications" (13 letras + terminador)
    char key[8];                   // usado quando type == "key"
    char macroText[MACRO_TEXT_MAX]; // usado quando type == "macro"
    bool macroWinR;                 // usado quando type == "macro"
    bool macroEnter;                // usado quando type == "macro"
    char colorHex[8];                // "#RRGGBB"
};

struct DeckConfig {
    uint8_t buttonCount;
    ButtonConfig buttons[MAX_BUTTONS];
    uint32_t idleTimeoutMs;
    // Opacidade das caixas dos botoes sobre a imagem de fundo (0-100%). So
    // vale quando ha /background.rgb565; sem fundo as caixas sao opacas.
    uint8_t boxOpacity;
};

#define DEFAULT_BOX_OPACITY 70

namespace Config {
    void loadDefaults(DeckConfig &cfg);

    // Le /config.json; se nao existir ou estiver corrompido, aplica os
    // defaults e tenta gravar.
    bool load(DeckConfig &cfg);

    bool save(const DeckConfig &cfg);

    // Serializa cfg como JSON (usado pelo comando get_config do protocolo
    // serial). Retorna o tamanho escrito em out, ou 0 em caso de erro.
    size_t toJson(const DeckConfig &cfg, char *out, size_t outSize);

    // Preenche cfg a partir de um JSON (comando set_config). Retorna false
    // se o JSON for invalido ou os campos obrigatorios estiverem ausentes.
    bool fromJson(const char *json, DeckConfig &cfg);

    uint8_t keyNameToCode(const char *name);
    const char *keyCodeToName(uint8_t code);
    uint16_t colorHexToRGB565(const char *hex);

    // Caminho do icone (LittleFS) de um botao pelo indice, ex: "/icon0.rgba".
    String iconPath(uint8_t index);
}
