"""Único módulo do programa que toca no token OAuth do Claude Code.

Para auditar: se um arquivo não importa este módulo, ele não vê o token.
O resto do programa só recebe o JSON de uso (fetch_usage) ou um UsageError.

As três regras, e onde cada uma é aplicada:

1. Credencial só para leitura: _read_credentials() abre o arquivo com
   mode="r" e nada mais. Não há nenhum outro open/write/rename neste módulo.
   Quem renova o token é o Claude Code.
2. Token nunca impresso, logado ou gravado: ele vive só em variáveis locais
   de fetch_usage()/_request(), nunca em global, atributo, exceção ou
   mensagem de erro. O único destino é USAGE_URL (api.anthropic.com), e
   redirecionamentos são recusados para o cabeçalho Authorization não ser
   reenviado a outro host.
3. Isolamento: é este arquivo inteiro.
"""

import json
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime
from pathlib import Path

CREDENTIALS_PATH = Path.home() / ".claude" / ".credentials.json"
USAGE_URL = "https://api.anthropic.com/api/oauth/usage"
BETA_HEADER = "oauth-2025-04-20"
# Só nestes status o servidor é tentado de novo, sem o cabeçalho beta.
RETRY_WITHOUT_BETA = (400, 404, 422)


class UsageError(Exception):
    """Falha ao obter o uso. A mensagem nunca contém o token.

    kind: no_token | token_expired | token_rejected  (problemas de token)
          rate_limited | network | http | bad_response (problemas transitórios)
    """

    TOKEN_KINDS = ("no_token", "token_expired", "token_rejected")

    def __init__(self, kind, message):
        super().__init__(message)
        self.kind = kind

    @property
    def is_token_problem(self):
        return self.kind in self.TOKEN_KINDS


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


_opener = urllib.request.build_opener(_NoRedirect)


def _find_key(obj, key):
    """Primeiro valor de `key` em qualquer profundidade; (achou, valor)."""
    if isinstance(obj, dict):
        if key in obj:
            return True, obj[key]
        children = obj.values()
    elif isinstance(obj, list):
        children = obj
    else:
        return False, None
    for child in children:
        found, value = _find_key(child, key)
        if found:
            return True, value
    return False, None


def _expiry_epoch(value):
    """expiresAt como epoch em segundos (aceita ms, s ou ISO 8601); None se ilegível."""
    if isinstance(value, bool):
        return None
    if isinstance(value, (int, float)):
        return value / 1000 if value > 1e11 else float(value)
    if isinstance(value, str):
        try:
            return datetime.fromisoformat(value.replace("Z", "+00:00")).timestamp()
        except ValueError:
            return None
    return None


def _read_credentials():
    """(accessToken, expiresAt em epoch ou None). Abre o arquivo SOMENTE para leitura."""
    try:
        with open(CREDENTIALS_PATH, "r", encoding="utf-8") as f:
            data = json.load(f)
    except FileNotFoundError:
        raise UsageError("no_token", "arquivo de credenciais não encontrado") from None
    except (OSError, ValueError):
        raise UsageError("no_token", "arquivo de credenciais ilegível") from None

    found, token = _find_key(data, "accessToken")
    if not found or not isinstance(token, str) or not token.strip():
        raise UsageError("no_token", "accessToken ausente nas credenciais")
    _, expires = _find_key(data, "expiresAt")
    return token.strip(), _expiry_epoch(expires)


def _request(token, with_beta):
    headers = {"Authorization": f"Bearer {token}", "Accept": "application/json"}
    if with_beta:
        headers["anthropic-beta"] = BETA_HEADER
    req = urllib.request.Request(USAGE_URL, headers=headers, method="GET")
    # Último portão da regra 2: o token só sai para api.anthropic.com por https.
    parts = urllib.parse.urlsplit(req.full_url)
    if parts.scheme != "https" or parts.hostname != "api.anthropic.com":
        raise UsageError("http", "destino não permitido")
    with _opener.open(req, timeout=20) as resp:
        return json.load(resp)


def fetch_usage():
    """Resposta JSON de /api/oauth/usage, ou UsageError."""
    token, expires = _read_credentials()
    if expires is not None and time.time() >= expires:
        raise UsageError("token_expired", "token vencido (abra o Claude Code para renovar)")

    try:
        try:
            data = _request(token, with_beta=True)
        except urllib.error.HTTPError as e:
            if e.code not in RETRY_WITHOUT_BETA:
                raise
            data = _request(token, with_beta=False)
    except urllib.error.HTTPError as e:
        if e.code in (401, 403):
            raise UsageError("token_rejected", f"token recusado pelo servidor (HTTP {e.code})") from None
        if e.code == 429:
            raise UsageError("rate_limited", "limite de taxa (HTTP 429)") from None
        raise UsageError("http", f"HTTP {e.code}") from None
    except (urllib.error.URLError, TimeoutError, OSError):
        raise UsageError("network", "sem conexão com api.anthropic.com") from None
    except ValueError:
        raise UsageError("bad_response", "resposta não é JSON") from None
    finally:
        del token

    if not isinstance(data, dict):
        raise UsageError("bad_response", "resposta inesperada")
    return data
