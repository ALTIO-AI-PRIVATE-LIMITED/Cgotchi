#!/usr/bin/env python3
"""Agent Buddy bridge: shows your Mac's Claude Code sessions on a CGotchi.

  agent_bridge.py install   add the Claude Code hooks and a login service
  agent_bridge.py run       keep the Gotchi's list up to date (the service)
  agent_bridge.py hook      called by Claude Code hooks (reads JSON on stdin)
  agent_bridge.py list      print the sessions the Gotchi would show
  agent_bridge.py name <id> <name>   show this name for a session

Each hook writes ~/.cgotchi/sessions/<session id>.json. `run` sends that list
to the Agent Buddy App over Wi-Fi (TCP 7333) and, when you pick a session on
the Gotchi, brings its Terminal or iTerm2 tab to the front so Wispr Flow
types into it. Standard library only.
"""

from __future__ import annotations

import json
import os
import re
import select
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path

PORT = 7333
HOME = Path.home()
STATE_DIR = HOME / ".cgotchi" / "sessions"
CONFIG = HOME / ".cgotchi" / "config.json"
CLAUDE_SETTINGS = HOME / ".claude" / "settings.json"
PLIST = HOME / "Library" / "LaunchAgents" / "com.cgotchi.agent-bridge.plist"
SCRIPT = Path(__file__).resolve()

CLOSED_KEEP_S = 10 * 60      # closed sessions stay on the list this long
FORGET_S = 24 * 60 * 60      # sessions untouched this long are dropped
SEND_EVERY_S = 2.0
HOOK_EVENTS = ["SessionStart", "UserPromptSubmit", "PreToolUse", "Notification", "Stop", "SessionEnd"]
ORDER = {"needs": 0, "working": 1, "done": 2, "ready": 3, "closed": 4}


# ---------------------------------------------------------------- hook side

def _ps(pid: int) -> tuple[int, str, str] | None:
    try:
        out = subprocess.run(["ps", "-o", "ppid=,tty=,comm=", "-p", str(pid)],
                             capture_output=True, text=True, timeout=2).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return None
    parts = out.split(None, 2)
    if len(parts) < 3:
        return None
    return int(parts[0]), parts[1], parts[2]


def _claude_process() -> tuple[int, str]:
    """The Claude Code process that ran this hook, and its terminal."""
    pid = os.getppid()
    for _ in range(4):
        info = _ps(pid)
        if info is None:
            break
        ppid, tty, comm = info
        name = os.path.basename(comm)
        if name not in ("sh", "bash", "zsh", "dash") or ppid <= 1:
            return pid, ("" if tty in ("??", "?") else "/dev/" + tty)
        pid = ppid
    return os.getppid(), ""


def _clean(text: str, limit: int) -> str:
    text = re.sub(r"\s+", " ", str(text or "")).strip()
    return text if len(text) <= limit else text[: limit - 1] + "~"


def hook() -> int:
    try:
        event = json.load(sys.stdin)
    except (ValueError, OSError):
        return 0
    sid = str(event.get("session_id") or "")
    name = event.get("hook_event_name") or ""
    if not sid or not name:
        return 0
    STATE_DIR.mkdir(parents=True, exist_ok=True)
    path = STATE_DIR / (re.sub(r"[^A-Za-z0-9_-]", "", sid) + ".json")
    try:
        record = json.loads(path.read_text())
    except (OSError, ValueError):
        record = {}
    if not record.get("pid"):
        record["pid"], record["tty"] = _claude_process()
    record["id"] = sid[:36]
    record["cwd"] = event.get("cwd") or record.get("cwd") or ""
    record["transcript"] = event.get("transcript_path") or record.get("transcript") or ""
    if name == "UserPromptSubmit" and not record.get("title"):
        record["title"] = _clean(event.get("prompt"), 40)
    state, info = record.get("state", "ready"), record.get("info", "")
    if name == "SessionStart":
        state, info = "ready", "New session"
    elif name == "UserPromptSubmit":
        state, info = "working", _clean(event.get("prompt"), 30)
    elif name == "PreToolUse":
        state, info = "working", _clean(event.get("tool_name"), 30)
    elif name == "Notification":
        message = str(event.get("message") or "")
        kind = str(event.get("notification_type") or "")
        if kind == "permission_prompt" or "permission" in message.lower():
            state, info = "needs", _clean(message, 30) or "Asks permission"
        elif state != "done":
            state, info = "needs", _clean(message, 30) or "Waiting for you"
    elif name == "Stop":
        state, info = "done", "Your turn"
    elif name == "SessionEnd":
        state, info = "closed", "Session ended"
    record["state"], record["info"], record["updated"] = state, info, time.time()
    tmp = path.with_suffix(".tmp")
    tmp.write_text(json.dumps(record))
    tmp.replace(path)
    return 0


# ---------------------------------------------------------------- list side

def _alive(pid: int) -> bool:
    try:
        os.kill(int(pid), 0)
        return True
    except (OSError, ValueError, TypeError):
        return False


def _transcript_title(path: str) -> str:
    """The session's /rename title, or Claude's own summary of it."""
    try:
        with open(path, "rb") as handle:
            handle.seek(0, os.SEEK_END)
            size = handle.tell()
            handle.seek(max(0, size - 512 * 1024))
            tail = handle.read().decode(errors="ignore")
    except (OSError, TypeError):
        return ""
    custom, summary = "", ""
    for line in tail.splitlines():
        if '"custom-title"' not in line and '"summary"' not in line:
            continue
        try:
            entry = json.loads(line)
        except ValueError:
            continue
        if entry.get("type") == "custom-title" and entry.get("customTitle"):
            custom = entry["customTitle"]
        elif entry.get("type") == "summary" and entry.get("summary"):
            summary = entry["summary"]
    return custom or summary


def _age(seconds: float) -> str:
    seconds = max(0, int(seconds))
    if seconds < 60:
        return f"{seconds}s"
    if seconds < 3600:
        return f"{seconds // 60}m"
    return f"{seconds // 3600}h"


def sessions() -> list[dict]:
    now = time.time()
    rows = []
    for path in STATE_DIR.glob("*.json"):
        try:
            record = json.loads(path.read_text())
        except (OSError, ValueError):
            continue
        updated = float(record.get("updated") or 0)
        if record.get("state") != "closed" and not _alive(record.get("pid")):
            record["state"], record["info"] = "closed", "Session ended"
        if now - updated > FORGET_S or (record["state"] == "closed" and now - updated > CLOSED_KEEP_S):
            path.unlink(missing_ok=True)
            continue
        record["name"] = (record.get("label") or _transcript_title(record.get("transcript", "")) or record.get("title")
                          or os.path.basename(record.get("cwd", "").rstrip("/")) or "session")
        rows.append(record)
    rows.sort(key=lambda r: (ORDER.get(r.get("state"), 9), -float(r.get("updated") or 0)))
    seen: dict[str, int] = {}
    for record in rows:
        seen[record["name"]] = seen.get(record["name"], 0) + 1
        if seen[record["name"]] > 1:
            record["name"] = f'{record["name"]} #{seen[record["name"]]}'
        record["line"] = f'{record.get("info", "")}'[:22] + " " + _age(now - float(record.get("updated") or now))
    return rows[:12]


def _field(text: str, limit: int) -> str:
    return _clean(text, limit).replace("\t", " ")


def snapshot() -> bytes:
    lines = [f'R\t{r["id"]}\t{r["state"]}\t{_field(r["name"], 18)}\t{_field(r["line"], 30)}' for r in sessions()]
    return ("\n".join(lines + ["END"]) + "\n").encode()


# ---------------------------------------------------------------- focus

def _running(app: str) -> bool:
    return subprocess.run(["pgrep", "-xq", app]).returncode == 0


def _osascript(script: str) -> None:
    subprocess.run(["osascript", "-e", script], capture_output=True, timeout=5)


def focus(session_id: str) -> None:
    path = STATE_DIR / (re.sub(r"[^A-Za-z0-9_-]", "", session_id) + ".json")
    try:
        tty = json.loads(path.read_text()).get("tty", "")
    except (OSError, ValueError):
        return
    if not re.fullmatch(r"/dev/ttys?\d+", tty or ""):
        return
    if _running("iTerm2"):
        _osascript(f'''tell application "iTerm2"
  repeat with w in windows
    repeat with t in tabs of w
      repeat with s in sessions of t
        if tty of s is "{tty}" then
          select w
          select t
          select s
          activate
          return
        end if
      end repeat
    end repeat
  end repeat
end tell''')
    if _running("Terminal"):
        _osascript(f'''tell application "Terminal"
  repeat with w in windows
    repeat with t in tabs of w
      if tty of t is "{tty}" then
        set selected of t to true
        set index of w to 1
        activate
        return
      end if
    end repeat
  end repeat
end tell''')


# ---------------------------------------------------------------- run

def _config() -> dict:
    try:
        return json.loads(CONFIG.read_text())
    except (OSError, ValueError):
        return {}


def discover() -> str | None:
    """Finds the Gotchi's advert (_cgotchi-agents._tcp) with dns-sd."""
    if not shutil.which("dns-sd"):
        return None
    try:
        proc = subprocess.Popen(["dns-sd", "-L", "CGotchi Agents", "_cgotchi-agents._tcp", "local"],
                                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    except OSError:
        return None
    host = None
    deadline = time.time() + 4
    try:
        while time.time() < deadline and host is None:
            ready, _, _ = select.select([proc.stdout], [], [], 0.5)
            if ready:
                match = re.search(r"can be reached at (\S+?)\.?:(\d+)", proc.stdout.readline())
                if match:
                    host = match.group(1)
    finally:
        proc.kill()
    return host


def run(host: str | None) -> int:
    STATE_DIR.mkdir(parents=True, exist_ok=True)
    while True:
        target = host or os.environ.get("CGOTCHI_HOST") or _config().get("host") or discover()
        if not target:
            print("Gotchi not found; open Agent Buddy on it, or set its address with: run --host <ip>", flush=True)
            time.sleep(5)
            continue
        try:
            conn = socket.create_connection((target, PORT), timeout=5)
        except OSError as error:
            print(f"cannot reach {target}:{PORT}: {error}", flush=True)
            time.sleep(3)
            continue
        print(f"connected to {target}", flush=True)
        conn.settimeout(None)
        last, last_sent, buffer = b"", 0.0, b""
        try:
            while True:
                data = snapshot()
                if data != last or time.time() - last_sent > 15:
                    conn.sendall(data)
                    last, last_sent = data, time.time()
                ready, _, _ = select.select([conn], [], [], SEND_EVERY_S)
                if ready:
                    chunk = conn.recv(1024)
                    if not chunk:
                        raise OSError("Gotchi closed the connection")
                    buffer += chunk
                    while b"\n" in buffer:
                        line, buffer = buffer.split(b"\n", 1)
                        parts = line.decode(errors="ignore").strip().split("\t")
                        if len(parts) == 2 and parts[0] == "F":
                            focus(parts[1])
        except OSError as error:
            print(f"disconnected: {error}", flush=True)
            conn.close()
            time.sleep(2)


# ---------------------------------------------------------------- install

def install() -> int:
    command = f'/usr/bin/python3 "{SCRIPT}" hook'
    settings = {}
    if CLAUDE_SETTINGS.exists():
        backup = CLAUDE_SETTINGS.with_name(f"settings.json.bak-{int(time.time())}")
        shutil.copy2(CLAUDE_SETTINGS, backup)
        print(f"backed up {CLAUDE_SETTINGS} to {backup}")
        settings = json.loads(CLAUDE_SETTINGS.read_text() or "{}")
    hooks = settings.setdefault("hooks", {})
    for name in HOOK_EVENTS:
        entries = hooks.setdefault(name, [])
        if any(h.get("command") == command for e in entries for h in e.get("hooks", [])):
            continue
        entry = {"hooks": [{"type": "command", "command": command, "timeout": 5}]}
        if name == "PreToolUse":
            entry["matcher"] = "*"
        entries.append(entry)
    CLAUDE_SETTINGS.parent.mkdir(parents=True, exist_ok=True)
    CLAUDE_SETTINGS.write_text(json.dumps(settings, indent=2) + "\n")
    print(f"added Agent Buddy hooks to {CLAUDE_SETTINGS}")

    log = HOME / ".cgotchi" / "agent-bridge.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    PLIST.parent.mkdir(parents=True, exist_ok=True)
    PLIST.write_text(f'''<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>com.cgotchi.agent-bridge</string>
  <key>ProgramArguments</key><array>
    <string>/usr/bin/python3</string><string>{SCRIPT}</string><string>run</string>
  </array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>StandardOutPath</key><string>{log}</string>
  <key>StandardErrorPath</key><string>{log}</string>
</dict></plist>
''')
    subprocess.run(["launchctl", "unload", str(PLIST)], capture_output=True)
    subprocess.run(["launchctl", "load", str(PLIST)], check=False)
    print(f"started the bridge service (log: {log})")
    print("Restart open Claude Code sessions so they pick up the hooks.")
    return 0


def main(argv: list[str]) -> int:
    command = argv[1] if len(argv) > 1 else ""
    if command == "hook":
        try:
            return hook()
        except Exception:  # a hook must never break Claude Code
            return 0
    if command == "run":
        host = argv[argv.index("--host") + 1] if "--host" in argv[:-1] else None
        if host:
            CONFIG.parent.mkdir(parents=True, exist_ok=True)
            CONFIG.write_text(json.dumps({**_config(), "host": host}))
        return run(host)
    if command == "install":
        return install()
    if command == "name" and len(argv) >= 4:
        # agent_bridge.py name <session id or prefix> <name>: show this name on the Gotchi
        matches = [p for p in STATE_DIR.glob("*.json") if p.stem.startswith(argv[2])]
        if len(matches) != 1:
            print(f"{len(matches)} sessions match {argv[2]!r}; give more of the id")
            return 1
        record = json.loads(matches[0].read_text())
        record["label"] = _clean(" ".join(argv[3:]), 40)
        matches[0].write_text(json.dumps(record))
        print(f'{matches[0].stem} is now "{record["label"]}"')
        return 0
    if command == "list":
        for record in sessions():
            print(f'{record["state"]:8} {record["name"]:20} {record["line"]}')
        return 0
    print(__doc__)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
