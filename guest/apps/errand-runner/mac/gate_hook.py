#!/usr/bin/env python3
"""Claude Code PreToolUse hook: no booking or paying on the phone without the Gotchi.

A phone-harness script may only touch a commit button (Book, Confirm, Pay,
Place order, ...) or tap raw coordinates when the Gotchi has just confirmed:
errand_bridge.py writes ~/.cgotchi/errand/confirm.json after BOOT was held
for 2 seconds. That token is used up by the first script it lets through and
expires after 60 seconds. Exit code 2 blocks the tool call.

This is a backstop for the agent's instructions, not a sandbox: it reads the
script text, so it cannot see what is under a coordinate (hence coordinates
need the token too).
"""

from __future__ import annotations

import json
import os
import re
import sys
import time
from pathlib import Path

TOKEN = Path(os.environ.get("ERRAND_TOKEN", Path.home() / ".cgotchi" / "errand" / "confirm.json"))
MAX_AGE_S = 60
RISKY = re.compile(
    r"\b(book|confirm|place\s*order|pay|slide\s*to\s*pay|request|checkout|check\s*out|buy\s*now|proceed\s*to\s*pay)\b",
    re.IGNORECASE,
)
COORDINATE_TAP = re.compile(r"\b(tap|long_press|tap_image_point)\s*\(\s*-?\d")
# Never type secrets, token or not.
SECRET = re.compile(r"\b(upi\s*pin|otp|password|passcode)\b", re.IGNORECASE)


def _take_token(now: float) -> bool:
    """Returns True and marks the token used if a fresh unused token exists."""
    try:
        token = json.loads(TOKEN.read_text())
    except (OSError, ValueError):
        return False
    if token.get("used") or now - float(token.get("created", 0)) > MAX_AGE_S:
        return False
    token["used"] = True
    TOKEN.write_text(json.dumps(token))
    return True


def decide(command: str, now: float | None = None) -> str | None:
    """Returns a reason to block, or None to allow."""
    if "phone-harness" not in command and "phone_harness" not in command:
        return None
    if SECRET.search(command) and "type_text" in command:
        return "Never type a PIN, OTP or password on the phone."
    risky = RISKY.search(command) or COORDINATE_TAP.search(command)
    if not risky:
        return None
    if _take_token(time.time() if now is None else now):
        return None
    return (f"Blocked: '{risky.group(0)}' needs a fresh confirm from the CGotchi (hold BOOT 2 s). "
            "Stop before any Book/Confirm/Pay step and report the quote instead.")


def main() -> int:
    try:
        event = json.load(sys.stdin)
    except ValueError:
        return 0
    if event.get("tool_name") != "Bash":
        return 0
    reason = decide(str(event.get("tool_input", {}).get("command", "")))
    if reason:
        print(reason, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
