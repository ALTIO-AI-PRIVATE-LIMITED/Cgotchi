# Agent Buddy

See every Claude Code session running on your Mac on the CGotchi, and give
the next command by voice.

- **Working** (blue): Claude is busy.
- **Needs you** (orange, double beep): Claude asks for permission or input.
- **Done** (green, chime): finished, waiting for your next command.
- **Closed** (grey): the session ended (cleared after 10 minutes).

## Controls

| Action | Touch | Keys |
| --- | --- | --- |
| Pick a session (its terminal comes to the front on the Mac) | tap its row | VOL+ (next) |
| Speak the next command (Wispr Flow, Ctrl+Option) | hold **HOLD TO TALK** | hold BOOT |
| Send it (Enter) | tap **SEND** | VOL- |

## Setup (once)

1. Flash host firmware from the `esp32-agent` branch (it has the Wi-Fi
   Network service and the "CGotchi" Bluetooth keyboard), join Wi-Fi, and
   pair "CGotchi" on the Mac in Bluetooth settings.
2. In Wispr Flow, keep push-to-talk on **Ctrl+Option**.
3. On the Mac:
   ```sh
   python3 guest/apps/agent-buddy/mac/agent_bridge.py install
   ```
   This backs up and adds hooks to `~/.claude/settings.json`, and starts a
   login service that sends the session list to the Gotchi.
4. Restart open Claude Code sessions so they load the hooks.
5. Open Agent Buddy on the Gotchi. The bridge finds it over Bonjour. If not,
   run it once with the address shown in the top right corner:
   `python3 .../agent_bridge.py run --host 192.168.x.y` (it is remembered).
6. The first time you pick a session, macOS asks to let python3 control
   Terminal or iTerm2. Allow it.

`agent_bridge.py list` prints what the Gotchi should show.

Works with Claude Code sessions in Terminal and iTerm2. Sessions in other
apps are listed, but picking them does not switch windows.
