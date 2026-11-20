# Agent Buddy

See every Claude Code session running on your Mac on the CGotchi, and give
the next command by voice.

- **Working** (blue): Claude is busy.
- **Needs you** (orange, double beep): Claude asks for permission or input.
- **Done** (green, chime): finished, waiting for your next command.
- **Closed** (grey): the session ended (cleared after 10 minutes).

## Screens

- **Grid** (home): one character per session, 4 per row. Yellow = working,
  green = needs you (red badge blinks), blue = done, grey = closed. Top right
  shows the counts with the same coloured dots; the selected session's name
  is the title. Tap a character to pick it (its terminal comes to the front
  on the Mac). Drag up or down to scroll when there are more than 8.
- **Controls** (swipe up): the selected session with Hold to Talk and Send.
  Swipe down or tap above the sheet to close.
- **Overview** (swipe right): counts per state, Mac, Wi-Fi and Bluetooth
  status. Swipe left to go back.

| Action | Touch | Keys |
| --- | --- | --- |
| Pick the next session | tap a character | VOL+ |
| Speak the next command (Wispr Flow, Ctrl+Option) | hold **Hold to Talk** | hold BOOT |
| Send it (Enter) | tap **Send** | VOL- |

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

## What is listed

- Local sessions: from the hooks, plus `claude agents --json` for sessions
  started before the hooks were installed.
- Cloud and Remote Control sessions of your Claude account (marked
  "Cloud:"). Tapping one opens it in the Claude app or browser. This uses the
  Claude Code sign-in (`claude auth login`) and the private endpoint the CLI
  uses for its own session list, so it can stop working after a Claude
  update; the local list keeps working either way.
- Not listed: claude.ai project threads and chats. They are not in any API
  the Mac can reach.

Works with Claude Code sessions in Terminal and iTerm2. Sessions in other
apps are listed, but picking them does not switch windows.
