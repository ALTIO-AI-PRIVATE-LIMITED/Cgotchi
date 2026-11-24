#!/usr/bin/env python3
"""Errand Runner bridge: the CGotchi asks, your iPhone does the errand.

  errand_bridge.py setup                 write the gate hook settings and the default menu
  errand_bridge.py run [--host IP]       connect to the Gotchi app and serve errands
        --demo          fake quotes, never touches the phone
        --allow-orders  let a Gotchi confirm actually book (off by default)
  errand_bridge.py quote "ride from MG Road to Koramangala"   read-only test, prints JSON

Flow: the Gotchi sends Q (menu errand) or V (voice). The bridge runs Claude
Code headless with the phone-harness skill to read fares or a cart total and
stop before booking, then sends the quotes. When BOOT was held 2 s on the
Gotchi it sends C <quote id>; only then the bridge writes the one-use confirm
token that gate_hook.py checks and asks Claude to book that one quote.
Payment stays Cash / COD; nobody types a UPI PIN or OTP. Standard library only.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

PORT = 7334
HOME = Path.home()
STATE = HOME / ".cgotchi" / "errand"
CONFIG = STATE / "config.json"
TOKEN = STATE / "confirm.json"
HERE = Path(__file__).resolve().parent
WORKDIR = STATE / "work"  # Claude runs here, with the gate hook in .claude/settings.json
QUOTE_TIMEOUT_S = 6 * 60
BOOK_TIMEOUT_S = 4 * 60

DEFAULT_MENU = [
    {"id": "ride", "label": "Uber: MG Road to Koramangala",
     "task": "Uber only: ride from MG Road Metro Station, Bengaluru to Koramangala, Bengaluru. Open only the Uber app, read the fare options, and stop before any Choose/Book button. Do not open Ola or Rapido."},
    {"id": "auto", "label": "Auto: MG Road to Koramangala",
     "task": "Auto rickshaw from MG Road Metro Station, Bengaluru to Koramangala, Bengaluru on Uber, Ola and Rapido."},
    {"id": "milk", "label": "Blinkit: 1 milk",
     "task": "Blinkit: add one 500 ml toned milk to the cart and read the cart total with delivery time."},
]

RULES = """You control the user's real iPhone with phone-harness (iPhone Mirroring). Hard rules:
- Never type a UPI PIN, OTP, card number or password. Payment is Cash / Pay on delivery only.
- Never post, message or change settings.
- Keep every phone-harness script free of the words Book, Confirm, Pay, Request, Checkout and Buy now
  unless the task below says you may book; a hook blocks them, and coordinate taps, without the CGotchi's confirm.
- If an app needs a login or shows something unexpected, stop and report it.
"""

QUOTE_PROMPT = RULES + """
Task (READ ONLY, do not book or order): {task}
Open each app, enter the trip or items, read the price options, then go back without booking.
For Blinkit, empty the cart again at the end.
Finish with ONE line of JSON and nothing after it:
{{"summary": "<route or cart in under 30 chars>", "quotes": [{{"app": "Uber", "option": "Go", "price": "Rs 182", "eta": "4 min"}}]}}
Use at most 6 quotes, cheapest first. If nothing could be read: {{"summary": "...", "quotes": [], "error": "<why>"}}
"""

BOOK_PROMPT = RULES + """
The user held the button on the CGotchi to confirm THIS booking, so you may now press the final button once:
Task: {task}
Chosen quote: {quote}
Rules for this booking: pay with Cash (or Pay on delivery). If the price is now more than 10% higher than the quote,
or Cash is not offered, do not book: go back and report it. Do the selection and the final button press in ONE
phone-harness script (the confirm token works once).
Finish with ONE line of JSON: {{"ok": true, "text": "<booking id / driver / ETA in under 40 chars>"}}
or {{"ok": false, "text": "<why not>"}}
"""


# ---------------------------------------------------------------- helpers
def field(text: object, limit: int = 46) -> str:
    """One protocol field: no tabs or newlines, bounded."""
    return re.sub(r"[\t\r\n]+", " ", str(text)).strip()[:limit]


def last_json(text: str) -> dict | None:
    """The last {...} line in Claude's output that parses as JSON."""
    for line in reversed(text.strip().splitlines()):
        line = line.strip().strip("`")
        if line.startswith("{") and line.endswith("}"):
            try:
                return json.loads(line)
            except ValueError:
                continue
    match = re.findall(r"\{.*\}", text, re.DOTALL)
    if match:
        try:
            return json.loads(match[-1])
        except ValueError:
            return None
    return None


def config() -> dict:
    try:
        return json.loads(CONFIG.read_text())
    except (OSError, ValueError):
        return {}


def menu() -> list[dict]:
    return config().get("menu") or DEFAULT_MENU


def setup() -> int:
    STATE.mkdir(parents=True, exist_ok=True)
    (WORKDIR / ".claude").mkdir(parents=True, exist_ok=True)
    hook = f"{sys.executable} {HERE / 'gate_hook.py'}"
    settings = {"hooks": {"PreToolUse": [{"matcher": "Bash", "hooks": [{"type": "command", "command": hook}]}]}}
    (WORKDIR / ".claude" / "settings.json").write_text(json.dumps(settings, indent=2))
    if not CONFIG.exists():
        CONFIG.write_text(json.dumps({"menu": DEFAULT_MENU}, indent=2))
    print(f"gate hook: {WORKDIR / '.claude' / 'settings.json'}")
    print(f"menu (edit to taste): {CONFIG}")
    for tool in ("claude", "phone-harness"):
        print(f"{tool}: {shutil.which(tool) or 'NOT FOUND'}")
    return 0


# ---------------------------------------------------------------- the phone agent
class Agent:
    """Runs one Claude Code job at a time; cancel() stops it."""

    def __init__(self, demo: bool) -> None:
        self.demo = demo
        self.proc: subprocess.Popen | None = None
        self.cancelled = False

    def cancel(self) -> None:
        self.cancelled = True
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()

    def _claude(self, prompt: str, timeout: int) -> dict | None:
        self.cancelled = False
        if not (WORKDIR / ".claude" / "settings.json").exists():
            setup()
        command = ["claude", "-p", prompt, "--permission-mode", "acceptEdits",
                   "--allowedTools", "Bash(phone-harness:*)", "Read"]
        self.proc = subprocess.Popen(command, cwd=WORKDIR, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            out, _ = self.proc.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            return {"error": "timed out"}
        if self.cancelled:
            return None
        return last_json(out or "") or {"error": "no answer from the phone agent"}

    def phone_ready(self) -> str | None:
        """None when iPhone Mirroring is live, else a short reason for the Gotchi."""
        try:
            done = subprocess.run(["phone-harness"], input="print(connection_state())\n",
                                  capture_output=True, text=True, timeout=30)
        except (OSError, subprocess.TimeoutExpired):
            return "iPhone not reachable"
        state = (done.stdout.strip().splitlines() or [""])[-1]
        return None if state == "ready" else "iPhone not connected: open Mirroring, lock phone"

    def quote(self, task: str) -> dict | None:
        if not self.demo:
            problem = self.phone_ready()
            if problem:
                return {"error": problem}
        if self.demo:
            time.sleep(2)
            return None if self.cancelled else {
                "summary": "MG Road > Koramangala",
                "quotes": [{"app": "Rapido", "option": "Bike", "price": "Rs 64", "eta": "3 min"},
                           {"app": "Uber", "option": "Auto", "price": "Rs 118", "eta": "5 min"},
                           {"app": "Ola", "option": "Mini", "price": "Rs 189", "eta": "6 min"}]}
        return self._claude(QUOTE_PROMPT.format(task=task), QUOTE_TIMEOUT_S)

    def book(self, task: str, quote: dict) -> dict | None:
        problem = self.phone_ready()
        if problem:
            return {"error": problem}
        return self._claude(BOOK_PROMPT.format(task=task, quote=json.dumps(quote)), BOOK_TIMEOUT_S)


def ask_by_voice() -> str | None:
    """A focused Mac dialog that Wispr Flow types into; Enter (VOL-) submits."""
    script = ('display dialog "What should CGotchi do?" default answer "" '
              'with title "CGotchi errand" buttons {"Cancel", "OK"} default button "OK"\n'
              'text returned of result')
    try:
        done = subprocess.run(["osascript", "-e", 'tell application "System Events" to activate', "-e", script],
                              capture_output=True, text=True, timeout=120)
    except (OSError, subprocess.TimeoutExpired):
        return None
    text = done.stdout.strip()
    return text or None


# ---------------------------------------------------------------- the Gotchi link
class Session:
    def __init__(self, conn: socket.socket, agent: Agent, allow_orders: bool) -> None:
        self.conn = conn
        self.agent = agent
        self.allow_orders = allow_orders
        self.lock = threading.Lock()
        self.task = ""
        self.quotes: dict[str, dict] = {}
        self.worker: threading.Thread | None = None

    def send(self, *parts: object) -> None:
        line = "\t".join(field(part) for part in parts) + "\n"
        with self.lock:
            self.conn.sendall(line.encode())

    def send_menu(self) -> None:
        for item in menu()[:6]:
            self.send("M", item["id"], item["label"])
        self.send("MEND")

    def _start(self, target, *args) -> None:
        if self.worker and self.worker.is_alive():
            self.agent.cancel()
            self.worker.join(timeout=5)
        self.agent.cancelled = False  # an earlier cancel must not swallow this new errand
        self.worker = threading.Thread(target=target, args=args, daemon=True)
        self.worker.start()

    def handle(self, line: str) -> None:
        parts = line.split("\t")
        kind = parts[0]
        if kind == "Q" and len(parts) >= 2:
            item = next((m for m in menu() if m["id"] == parts[1]), None)
            if item:
                self._start(self._quote, item["task"])
        elif kind == "V":
            self._start(self._voice)
        elif kind == "C" and len(parts) >= 2:
            self._start(self._book, parts[1])
        elif kind == "X":
            self.agent.cancel()
            self.quotes = {}
            print("cancelled on the Gotchi", flush=True)

    def _voice(self) -> None:
        self.send("L", "Speak now, VOL- to send")
        text = ask_by_voice()
        if not text:
            self.send("R", "fail", "Nothing heard")
            return
        self._quote(text)

    def _quote(self, task: str) -> None:
        self.task, self.quotes = task, {}
        print(f"quote: {task}", flush=True)
        self.send("S", "Reading prices on the phone...")
        result = self.agent.quote(task)
        if result is None:
            return  # cancelled
        quotes = result.get("quotes") or []
        print(f"result: {json.dumps(result, ensure_ascii=False)}", flush=True)
        if not quotes:
            self.send("R", "fail", result.get("error") or "No prices found")
            return
        for index, quote in enumerate(quotes[:6]):
            quote_id = f"q{index}"
            self.quotes[quote_id] = quote
            app = f"{quote.get('app', '')} {quote.get('option', '')}".strip()
            self.send("O", quote_id, app, quote.get("price", ""), quote.get("eta", ""))
        self.send("QEND", result.get("summary") or "Quotes")

    def _book(self, quote_id: str) -> None:
        quote = self.quotes.get(quote_id)
        if not quote:
            self.send("R", "fail", "That quote expired")
            return
        if not self.allow_orders:
            print(f"confirm received for {quote} (orders are off)", flush=True)
            self.send("R", "fail", "Test mode: not booked")
            return
        STATE.mkdir(parents=True, exist_ok=True)
        TOKEN.write_text(json.dumps({"quote_id": quote_id, "task": self.task, "quote": quote,
                                     "created": time.time(), "used": False}))
        print(f"BOOKING {quote}", flush=True)
        self.send("S", f"Booking {quote.get('app', '')}...")
        try:
            result = self.agent.book(self.task, quote)
        finally:
            TOKEN.unlink(missing_ok=True)
            self.quotes = {}
        if result is None:
            self.send("R", "fail", "Cancelled")
        else:
            self.send("R", "ok" if result.get("ok") else "fail", result.get("text") or result.get("error") or "?")


def discover() -> str | None:
    """Find the Gotchi over Bonjour. dns-sd never exits, so read what it printed in 4 s."""
    if not shutil.which("dns-sd"):
        return None
    try:
        subprocess.run(["dns-sd", "-L", "CGotchi Errands", "_cgotchi-errand._tcp", "local"],
                       capture_output=True, text=True, timeout=4)
        out = ""
    except subprocess.TimeoutExpired as expired:
        out = expired.stdout.decode(errors="ignore") if isinstance(expired.stdout, bytes) else (expired.stdout or "")
    except OSError:
        return None
    match = re.search(r"can be reached at (\S+?)\.?:(\d+)", out)
    return match.group(1) if match else None


def serve(conn: socket.socket, agent: Agent, allow_orders: bool) -> None:
    session = Session(conn, agent, allow_orders)
    session.send_menu()
    buffer = b""
    while True:
        chunk = conn.recv(1024)
        if not chunk:
            raise OSError("Gotchi closed the connection")
        buffer += chunk
        while b"\n" in buffer:
            line, buffer = buffer.split(b"\n", 1)
            session.handle(line.decode(errors="ignore").strip())


def run(host: str | None, port: int, demo: bool, allow_orders: bool, once: bool = False) -> int:
    agent = Agent(demo)
    print(f"Errand bridge: {'DEMO (fake quotes)' if demo else 'phone'}; "
          f"orders {'ON' if allow_orders else 'OFF (confirm does not book)'}", flush=True)
    failures = 0
    while True:
        saved = host or os.environ.get("CGOTCHI_HOST") or config().get("host")
        # A saved IP that keeps refusing is stale (new Wi-Fi, new lease): look the Gotchi up again.
        target = (discover() or saved) if failures >= 3 or not saved else saved
        if not target:
            print("Gotchi not found; open Errand Runner on it, or pass --host <ip>", flush=True)
            time.sleep(5)
            continue
        conn = None
        try:
            conn = socket.create_connection((target, port), timeout=5)
            conn.settimeout(None)
            keepalive(conn)
            failures = 0
            print(f"connected to {target}:{port}", flush=True)
            serve(conn, agent, allow_orders)
        except OSError as error:
            failures += 1
            print(f"link: {error}", flush=True)
            agent.cancel()
            if conn:
                conn.close()
            if once:
                return 0
            time.sleep(3)


def keepalive(conn: socket.socket) -> None:
    """Notice a Gotchi that rebooted or left Wi-Fi within ~20 s instead of waiting forever."""
    conn.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
    idle = getattr(socket, "TCP_KEEPIDLE", getattr(socket, "TCP_KEEPALIVE", None))
    for option, value in ((idle, 10), (getattr(socket, "TCP_KEEPINTVL", None), 3),
                          (getattr(socket, "TCP_KEEPCNT", None), 3)):
        if option is not None:
            try:
                conn.setsockopt(socket.IPPROTO_TCP, option, value)
            except OSError:
                pass


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("setup")
    run_parser = sub.add_parser("run")
    run_parser.add_argument("--host")
    run_parser.add_argument("--port", type=int, default=PORT)
    run_parser.add_argument("--demo", action="store_true")
    run_parser.add_argument("--allow-orders", action="store_true")
    quote_parser = sub.add_parser("quote")
    quote_parser.add_argument("task")
    args = parser.parse_args(argv)
    if args.command == "setup":
        return setup()
    if args.command == "quote":
        print(json.dumps(Agent(demo=False).quote(args.task), indent=2))
        return 0
    if args.host:
        STATE.mkdir(parents=True, exist_ok=True)
        CONFIG.write_text(json.dumps({**config(), "host": args.host}, indent=2))
    return run(args.host, args.port, args.demo, args.allow_orders)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
