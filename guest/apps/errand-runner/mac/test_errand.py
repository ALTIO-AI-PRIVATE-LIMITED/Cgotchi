#!/usr/bin/env python3
"""Tests for the errand bridge and gate hook: python3 test_errand.py"""

from __future__ import annotations

import json
import os
import socket
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
_TMP = tempfile.mkdtemp()
os.environ["ERRAND_TOKEN"] = str(Path(_TMP) / "confirm.json")

import errand_bridge  # noqa: E402
import gate_hook  # noqa: E402

BOOK = 'phone-harness <<\'PY\'\n# task: ride\ntap_text("Book Uber Go")\nPY'
READ = 'phone-harness <<\'PY\'\n# task: ride\nopen_app("Uber")\nprint(screen())\nPY'


def write_token(created: float, used: bool = False) -> None:
    gate_hook.TOKEN.write_text(json.dumps({"quote_id": "q0", "created": created, "used": used}))


class GateTest(unittest.TestCase):
    def setUp(self) -> None:
        gate_hook.TOKEN.unlink(missing_ok=True)

    def test_read_only_script_is_allowed(self) -> None:
        self.assertIsNone(gate_hook.decide(READ))

    def test_other_commands_are_ignored(self) -> None:
        self.assertIsNone(gate_hook.decide("echo Book now"))

    def test_book_without_token_is_blocked(self) -> None:
        self.assertIn("Blocked", gate_hook.decide(BOOK))

    def test_each_commit_word_is_blocked(self) -> None:
        for word in ("Confirm", "Place order", "Pay", "Slide to pay", "Request", "Checkout", "Buy now"):
            self.assertIsNotNone(gate_hook.decide(f'phone-harness <<PY\ntap_text("{word}")\nPY'), word)

    def test_coordinate_tap_without_token_is_blocked(self) -> None:
        self.assertIsNotNone(gate_hook.decide("phone-harness <<PY\ntap(120, 640)\nPY"))

    def test_fresh_token_allows_once(self) -> None:
        write_token(time.time())
        self.assertIsNone(gate_hook.decide(BOOK))
        self.assertIsNotNone(gate_hook.decide(BOOK))

    def test_old_token_is_refused(self) -> None:
        write_token(time.time() - 61)
        self.assertIsNotNone(gate_hook.decide(BOOK))

    def test_used_token_is_refused(self) -> None:
        write_token(time.time(), used=True)
        self.assertIsNotNone(gate_hook.decide(BOOK))

    def test_secrets_are_never_typed(self) -> None:
        write_token(time.time())
        self.assertIsNotNone(gate_hook.decide('phone-harness <<PY\n# UPI PIN\ntype_text("1234")\nPY'))


class HelperTest(unittest.TestCase):
    def test_field_strips_tabs_and_newlines(self) -> None:
        self.assertEqual(errand_bridge.field("a\tb\nc"), "a b c")

    def test_last_json_takes_final_line(self) -> None:
        out = 'Looking at Uber...\n{"x": 1}\nmore\n{"summary": "A", "quotes": []}\n'
        self.assertEqual(errand_bridge.last_json(out)["summary"], "A")

    def test_last_json_none(self) -> None:
        self.assertIsNone(errand_bridge.last_json("no json here"))


class FakeGotchi:
    """Listens like the Gotchi app and records lines from the bridge."""

    def __init__(self) -> None:
        self.server = socket.socket()
        self.server.bind(("127.0.0.1", 0))
        self.server.listen(1)
        self.port = self.server.getsockname()[1]
        self.conn: socket.socket | None = None
        self.buffer = b""

    def accept(self) -> None:
        self.server.settimeout(5)
        self.conn, _ = self.server.accept()
        self.conn.settimeout(10)

    def send(self, line: str) -> None:
        self.conn.sendall((line + "\n").encode())

    def until(self, kind: str) -> list[list[str]]:
        lines: list[list[str]] = []
        while True:
            while b"\n" in self.buffer:
                raw, self.buffer = self.buffer.split(b"\n", 1)
                parts = raw.decode().split("\t")
                lines.append(parts)
                if parts[0] == kind:
                    return lines
            self.buffer += self.conn.recv(1024)

    def close(self) -> None:
        self.conn.close()
        self.server.close()


class DemoFlowTest(unittest.TestCase):
    def test_menu_quote_and_confirm_without_orders(self) -> None:
        gotchi = FakeGotchi()
        bridge = threading.Thread(target=errand_bridge.run, daemon=True,
                                  args=("127.0.0.1", gotchi.port, True, False, True))
        bridge.start()
        gotchi.accept()
        menu = gotchi.until("MEND")
        self.assertEqual([m[0] for m in menu[:-1]], ["M"] * len(menu[:-1]))
        gotchi.send(f"Q\t{menu[0][1]}")
        quotes = gotchi.until("QEND")
        offers = [q for q in quotes if q[0] == "O"]
        self.assertEqual(len(offers), 3)
        self.assertTrue(all(len(q) == 5 for q in offers))
        gotchi.send(f"C\t{offers[0][1]}")
        result = gotchi.until("R")[-1]
        self.assertEqual(result[1], "fail")  # orders are off: a confirm never books
        self.assertFalse(Path(errand_bridge.TOKEN).exists())
        gotchi.close()
        bridge.join(timeout=5)

    def test_cancel_drops_quotes(self) -> None:
        gotchi = FakeGotchi()
        threading.Thread(target=errand_bridge.run, daemon=True,
                         args=("127.0.0.1", gotchi.port, True, False, True)).start()
        gotchi.accept()
        menu = gotchi.until("MEND")
        gotchi.send(f"Q\t{menu[0][1]}")
        gotchi.send("X")
        gotchi.send("C\tq0")
        lines = gotchi.until("R")
        self.assertNotIn("QEND", [line[0] for line in lines])
        self.assertEqual(lines[-1][2], "That quote expired")
        gotchi.close()

    def test_quote_after_cancel_still_answers(self) -> None:
        gotchi = FakeGotchi()
        threading.Thread(target=errand_bridge.run, daemon=True,
                         args=("127.0.0.1", gotchi.port, True, False, True)).start()
        gotchi.accept()
        menu = gotchi.until("MEND")
        gotchi.send("X")  # an earlier errand was cancelled on the Gotchi
        gotchi.send(f"Q\t{menu[0][1]}")
        self.assertEqual(gotchi.until("QEND")[-1][0], "QEND")
        gotchi.close()


if __name__ == "__main__":
    unittest.main(verbosity=2)
