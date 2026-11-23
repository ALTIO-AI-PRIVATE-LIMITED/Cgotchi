# Errand Runner

Use the CGotchi as the remote for rides (Uber, Ola, Rapido) and Blinkit orders.
The Mac does the errand on your iPhone through iPhone Mirroring with
[phone-harness](https://github.com/phone-harness/phone-harness), and nothing
is booked until you press and hold a price on the Gotchi's screen for 2 seconds.

1. **Tap** an errand on the menu, or tap **Say it...**, hold the on-screen
   **Hold to talk** button and speak (Wispr Flow types into a box on the Mac),
   then press VOL- to send.
2. **Wait**: the Mac reads the fares or the cart total and stops before any
   Book or Pay button.
3. **Press and hold a price row for 2 s**. The row fills green; when it is
   full the Mac books that one. Letting go early or sliding your finger off the
   row books nothing. VOL- cancels.

BOOT is not used: holding it powers the Gotchi off.

Payment is Cash / Pay on delivery. The agent never types a UPI PIN, OTP or
password.

| Action | Touch | Keys |
| --- | --- | --- |
| Choose an errand | tap it | |
| Speak (Say it) | hold "Hold to talk" | VOL- sends |
| Book a quote | press and hold its row 2 s | |
| Cancel | tap the screen while it works | VOL- |

## Setup (once, on the Mac)

1. Gotchi firmware with the Network and Bluetooth keyboard services (the
   `esp32-agent` firmware, same as Agent Buddy). Join Wi-Fi and pair
   "CGotchi" in the Mac's Bluetooth settings for voice.
2. phone-harness on the Mac, iPhone Mirroring connected, Accessibility and
   Screen Recording on for the app that runs Claude Code.
3. Write the gate hook and the default menu:
   ```sh
   python3 guest/apps/errand-runner/mac/errand_bridge.py setup
   ```
   The menu lives in `~/.cgotchi/errand/config.json` (up to 6 rows of
   `id`, `label`, `task`).

## Run

```sh
python3 guest/apps/errand-runner/mac/errand_bridge.py run --demo            # fake quotes, no phone
python3 guest/apps/errand-runner/mac/errand_bridge.py run                   # real fares, confirm does not book
python3 guest/apps/errand-runner/mac/errand_bridge.py run --allow-orders    # confirm books
```

The bridge finds the Gotchi over Bonjour (`_cgotchi-errand._tcp`), or pass
`--host <ip>` once. `errand_bridge.py quote "ride from MG Road to Koramangala"`
tests one read-only quote from the terminal.

## Safety

- The quote step runs Claude Code with instructions to stop before booking.
- `mac/gate_hook.py` is a Claude Code PreToolUse hook. It blocks any
  phone-harness script that names a commit button (Book, Confirm, Pay, Place
  order, Request, Checkout, Buy now) or taps raw coordinates, unless the
  Gotchi has just confirmed. The bridge writes that confirm token only after
  the 2 s hold; it works once and expires after 60 s.
- Without `--allow-orders` a confirm never books.
- The hook reads script text, so it is a backstop, not a sandbox.

Tests: `python3 guest/apps/errand-runner/mac/test_errand.py`.
