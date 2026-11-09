"""Mostra o uso da assinatura do Claude: janelas de 5 horas e 7 dias.

Este arquivo nunca vê o token; quem fala com a API é claude_oauth.py.

    python tools/claude_usage.py          # fica atualizando
    python tools/claude_usage.py --once   # uma consulta e sai
    python tools/claude_usage.py --raw    # imprime o JSON da resposta e sai
"""

import argparse
import json
import os
import sys
import time
from datetime import datetime, timezone

import claude_oauth
from claude_oauth import UsageError

INTERVAL_BASE = 300    # s entre consultas
INTERVAL_MAX = 1800    # teto do backoff
# Depois de uma falha que não é limite de taxa, tenta de novo logo: com o token
# vencido nem há requisição (só relê o arquivo), então o Claude Code renovando o
# token aparece em segundos em vez de esperar o próximo ciclo de 5 min.
RETRY_TOKEN = 20       # s; problema de token
RETRY_FAIL = 60        # s; rede/HTTP/resposta ruim
STALE_AFTER = 15 * 60  # só avisa que o dado está velho depois disso
REDRAW_EVERY = 30      # s; só atualiza a contagem regressiva, sem consultar

WINDOWS = (("five_hour", "5 horas"), ("seven_day", "7 dias"))


def parse_reset(value):
    """resets_at (ISO 8601 ou epoch) como datetime UTC; None se ilegível."""
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        return datetime.fromtimestamp(value / 1000 if value > 1e11 else value, timezone.utc)
    if isinstance(value, str):
        try:
            dt = datetime.fromisoformat(value.replace("Z", "+00:00"))
        except ValueError:
            return None
        return dt if dt.tzinfo else dt.replace(tzinfo=timezone.utc)
    return None


def extract_window(data, kind):
    """(percent, resets_at) de uma janela, ou None se a resposta não a traz."""
    item = next(
        (i for i in data.get("limits") or []
         if isinstance(i, dict) and i.get("kind") == kind and i.get("is_active", True)),
        None,
    )
    if item is None and isinstance(data.get(kind), dict):
        item = data[kind]
    if item is None:
        return None
    percent = item.get("percent", item.get("utilization"))
    if isinstance(percent, bool) or not isinstance(percent, (int, float)):
        return None
    return float(percent), parse_reset(item.get("resets_at"))


def extract_usage(data):
    usage = {kind: extract_window(data, kind) for kind, _ in WINDOWS}
    if not any(usage.values()):
        raise UsageError("bad_response", "resposta sem as janelas de 5h/7d")
    return usage


def fmt_duration(seconds):
    minutes = max(0, int(seconds // 60))
    days, rest = divmod(minutes, 1440)
    hours, mins = divmod(rest, 60)
    if days:
        return f"{days}d {hours}h"
    if hours:
        return f"{hours}h {mins:02d}min"
    return f"{mins}min"


class Monitor:
    def __init__(self):
        self.started = time.monotonic()
        self.good = None           # último número bom; falha nenhuma o apaga
        self.good_at = None
        self.interval = INTERVAL_BASE
        self.next_poll = 0.0
        self.problem = None        # UsageError da última consulta, se falhou

    def poll(self):
        now = time.monotonic()
        try:
            self.good = extract_usage(claude_oauth.fetch_usage())
            self.good_at = now
            self.problem = None
            self.interval = INTERVAL_BASE
        except UsageError as e:
            self.problem = e
            if e.kind == "rate_limited":
                self.interval = min(max(self.interval, INTERVAL_BASE) * 2, INTERVAL_MAX)
            elif e.is_token_problem:
                self.interval = RETRY_TOKEN
            else:
                self.interval = RETRY_FAIL
        self.next_poll = now + self.interval

    def age(self):
        return time.monotonic() - (self.good_at if self.good_at is not None else self.started)

    def snapshot(self):
        """Estado pronto para exibir (sem token): janelas, idade do dado e aviso."""
        now = datetime.now(timezone.utc)
        windows = []
        for kind, label in WINDOWS:
            win = self.good.get(kind) if self.good else None
            if win is None:
                windows.append({"label": label, "percent": None, "reset": "", "reset_sec": None})
                continue
            percent, resets = win
            reset_sec = None
            if resets is None:
                reset = ""
            elif resets <= now:
                reset, reset_sec = "janela reiniciada", 0
            else:
                reset_sec = int((resets - now).total_seconds())
                reset = f"reinicia em {fmt_duration(reset_sec)}"
            windows.append({"label": label, "percent": percent, "reset": reset, "reset_sec": reset_sec})

        age = f"atualizado há {fmt_duration(self.age())}" if self.good_at is not None else ""
        age_sec = int(self.age()) if self.good_at is not None else None
        warning = None
        if self.problem is not None:
            if self.problem.is_token_problem:
                warning = str(self.problem)
            elif self.age() > STALE_AFTER:
                what = "dado" if self.good else "sem dado"
                warning = (
                    f"{what} com {fmt_duration(self.age())} ({self.problem}); "
                    f"próxima tentativa em {fmt_duration(max(0, self.next_poll - time.monotonic()))}"
                )
        return {"windows": windows, "age": age, "age_sec": age_sec, "warning": warning}

    def render(self):
        snap = self.snapshot()
        lines = ["Claude - uso da assinatura"]
        for w in snap["windows"]:
            if w["percent"] is None:
                lines.append(f"{w['label']:<8} --")
                continue
            filled = round(min(max(w["percent"], 0), 100) / 5)
            bar = "█" * filled + "░" * (20 - filled)
            lines.append(f"{w['label']:<8} {bar} {w['percent']:5.1f}%   {w['reset']}".rstrip())
        if snap["age"]:
            lines.append(snap["age"])
        if snap["warning"]:
            lines.append(f"! {snap['warning']}")
        return "\n".join(lines)


def write_snapshot(path, snap):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(snap, f, ensure_ascii=False)
    os.replace(tmp, path)  # troca atômica: a janela nunca lê meio arquivo


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--once", action="store_true", help="consulta uma vez e sai")
    ap.add_argument("--raw", action="store_true", help="imprime o JSON da resposta e sai")
    ap.add_argument("--snapshot-file", metavar="ARQUIVO",
                    help="em vez de imprimir, grava o estado (sem token) neste JSON; usado pela janela")
    args = ap.parse_args()

    # pythonw (janela sem console) não tem stdout
    if sys.stdout and sys.stdout.encoding and sys.stdout.encoding.lower() != "utf-8":
        sys.stdout.reconfigure(encoding="utf-8")

    if args.raw:
        try:
            print(json.dumps(claude_oauth.fetch_usage(), indent=2, ensure_ascii=False))
        except UsageError as e:
            print(f"erro: {e}", file=sys.stderr)
            return 1
        return 0

    mon = Monitor()
    live = bool(sys.stdout) and sys.stdout.isatty() and not args.once and not args.snapshot_file
    if live:
        os.system("")  # habilita sequências ANSI no console do Windows
    try:
        while True:
            if time.monotonic() >= mon.next_poll:
                mon.poll()
            if args.snapshot_file:
                write_snapshot(args.snapshot_file, mon.snapshot())
            else:
                print(("\033[H\033[J" if live else "") + mon.render(), flush=True)
            if args.once:
                return 0 if mon.problem is None else 1
            time.sleep(max(1, min(REDRAW_EVERY, mon.next_poll - time.monotonic())))
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main())
