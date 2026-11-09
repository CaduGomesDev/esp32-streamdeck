/**
 * Protocolo serial (USB CDC) usado pelo app de configuracao no PC.
 *
 * Comandos: uma linha JSON terminada em '\n'. Respostas: idem.
 *   {"cmd":"ping"}
 *   {"cmd":"get_config"}
 *   {"cmd":"set_config","buttons":[{"label":"OBS","key":"F13","color":"#FF0000"}, ...],"idle_timeout_ms":30000}
 *   {"cmd":"delete_gif"}
 *   {"cmd":"reboot"}
 *   {"cmd":"notify","app":"Discord","title":"...","body":"...","icon":"<base64 32x32, 1 byte alfa por pixel>"}
 *   {"cmd":"put_usage_bg","size":N} / {"cmd":"delete_usage_bg"}
 *       fundo da tela de uso do Claude; mesmo envio em duas fases do put_gif,
 *       com N bytes de RLE (ver Usage.h e tools/send-usage-bg.ps1)
 *   {"cmd":"usage","w":[{"l":"5 horas","p":17.0,"r":4380},{"l":"7 dias","p":12.0,"r":428000}],"a":12,"warn":"..."}
 *       (p = porcentagem, r = segundos ate reiniciar, a = idade do dado em s;
 *        ou {"cmd":"usage","error":"..."}). A placa pede com {"event":"usage_request"}.
 *
 * Upload do GIF e em duas fases (nao e so JSON, pois o conteudo e binario):
 *   1. host envia {"cmd":"put_gif","size":N}
 *   2. dispositivo responde {"ok":true} (pronto) ou {"ok":false,"error":...}
 *   3. se pronto, host envia exatamente N bytes crus (sem framing) do GIF
 *   4. dispositivo responde {"ok":true,"written":N} ao terminar
 */
#pragma once

#include "Config.h"

namespace SerialProtocol {
    using ApplyConfigFn = void (*)(const DeckConfig &cfg);
    // Chamado depois de enviar/remover um icone ou o fundo (recarregar e redesenhar).
    using AssetsChangedFn = void (*)();
    // Chamado ao receber {"cmd":"notify","app":...,"title":...,"body":...}
    // (ver tools/notify-bridge.ps1). Quem decide o que fazer com isso (entrar
    // no modo de notificacao, etc.) e o chamador.
    using NotifyFn = void (*)(const char *app, const char *title, const char *body);

    void begin(ApplyConfigFn onConfigApplied, AssetsChangedFn onAssetsChanged, NotifyFn onNotify);

    // Chamar a cada iteracao de loop(). Nao bloqueia, exceto durante uma
    // transferencia de GIF em andamento (put_gif), que e deliberadamente
    // sincrona.
    void poll();
}
