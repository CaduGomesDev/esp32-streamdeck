/**
 * Tela de notificacoes (modo NOTIFICATION): lista rolavel com as ultimas
 * notificacoes recebidas do PC pela serial (comando "notify", ver
 * SerialProtocol.h), da mais recente pra mais antiga. Um script no PC
 * (tools/notify-bridge.ps1) le as notificacoes do Windows e manda pra ca.
 *
 * Aqui ficam os dados (so em RAM), o desenho e o toque da tela (rolar, tocar
 * numa notificacao pra abrir a mensagem completa e marcar como lida, "Marcar
 * todas como lidas" e voltar);
 * quem decide quando entrar/sair do modo e o main.cpp.
 */
#pragma once

#include <Arduino_GFX_Library.h>

namespace Notification {
    enum class Action { NONE, CLOSE };

    // Icone do app (mascara de transparencia 32x32, 1 byte por pixel, em
    // base64) da proxima notificacao; chamar antes do add(). nullptr/vazio:
    // reaproveita o icone ja conhecido desse app, ou cai num icone generico.
    void setIcon(const char *b64);

    // Guarda uma notificacao nova (nao lida) no topo da lista.
    void add(const char *app, const char *title, const char *body);

    // Abre a tela do comeco (rolagem no topo) e zera o estado do toque.
    void open();

    // Alimentar a cada volta do loop enquanto a tela estiver aberta, com o
    // estado atual do touch. Cuida de rolar, marcar como lida e dos botoes do
    // cabecalho; devolve CLOSE quando o usuario tocou em voltar.
    Action handleTouch(bool touched, int x, int y);

    // true quando algo mudou e a tela precisa ser redesenhada (toque, rolagem
    // com inercia, notificacao nova ou horarios relativos que envelheceram).
    bool tick(unsigned long now);

    void draw(Arduino_Canvas *canvas);

    // true enquanto a mensagem completa de uma notificacao esta aberta.
    bool inDetail();

    int count();
    int unreadCount();
}
