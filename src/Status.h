/**
 * Tela de status dos servidores/PCs (botao do tipo "status"). Os dados vem do
 * PC pela serial (comando "status", ver SerialProtocol.h): um script no PC
 * consulta a API do esp32-pc-monitor e empurra o resultado pra ca. Esta tela
 * so guarda o ultimo snapshot e desenha os cards.
 */
#pragma once

#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>

namespace Status {
    enum class Action { NONE, CLOSE };

    // Atualiza o snapshot a partir do comando serial {"cmd":"status",...}.
    // Todos os PCs entram na lista, online ou offline.
    void update(JsonDocument &doc);

    // Abre a tela do comeco (rolagem no topo) e zera o estado do toque.
    void open();

    // Alimentar a cada volta do loop com o estado atual do touch. Passa as paginas
    // (arrastando o dedo pro lado, de 3 cards em 3) e devolve CLOSE quando o usuario tocou
    // em voltar.
    Action handleTouch(bool touched, int x, int y);

    // Avanca a animacao de encaixe na pagina; chamar a cada volta do loop.
    void tick(unsigned long now);

    // Marca que a tela precisa ser redesenhada (ex: pra atualizar o aviso de
    // "sem atualizar" mesmo quando nao chegaram dados novos).
    void markDirty();
    // true (uma vez) se houve mudanca desde o ultimo desenho.
    bool consumeDirty();

    void draw(Arduino_Canvas *canvas);
}
