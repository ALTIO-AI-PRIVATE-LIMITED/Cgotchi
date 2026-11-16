# CGotchi touch test station

Finds out why touch fails on a batch of CGotchi units. One firmware, one web page.

## Use (per device)
1. Open the site in Chrome or Edge (Web Serial). Optionally press "Save logs to a folder" once.
2. Plug in the CGotchi, press **Flash**, pick the `usbmodem` port.
3. Press **Track**. Screen shows `CGOTCHI TOUCH DIAG`. (If nothing arrives, unplug/replug once and press Track.)
4. Touch and swipe the whole screen ~15 s; tap "Touch works" / "Touch not working".
5. **Unplug.** After 4 s the log is saved with an automatic diagnosis and the page moves to the next device number.

Results stay in the browser (localStorage); export as CSV or all logs, or auto-write `device-NN_<mac>_<RESULT>.log` + `summary.csv` to a chosen folder.

## Diagnosis codes
| Code | Meaning |
|---|---|
| TOUCH_OK | Fingers reported; check zone coverage |
| TOUCH_PARTIAL | Fingers reported but < 6 of 9 zones reached |
| NO_SENSE | Touch chip healthy on I2C, never reports a finger: panel / flex fault |
| IRQ_NO_POINTS | Chip pulses IRQ but reports no coordinates: touch-chip firmware/config |
| IRQ_STUCK | IRQ line always low, no fingers |
| I2C_UNSTABLE | > 2 % of touch reads fail: loose flex / solder |
| CHIP_MISSING | CST810 (0x15) absent from I2C scan |
| CHIP_NO_ID | Chip on bus but ID registers 0xA7..0xA9 unreadable |
| NO_DATA | No DIAG output at all |

## Firmware (`firmware/`)
Based on cheeko-gotchi `firmware/hardware_test`, changed to:
- never block on touch calibration (hold BOOT at power-up to calibrate);
- print `DIAG ...` lines: `info` (MAC, reset reason) and I2C scan every 5 s, CST810 chip/project/fw ID (regs 0xA7..0xA9),
  a 1 s `stat` line (read ok/err, finger frames, taps, IRQ falling edges, IRQ level, last register bytes, raw ranges),
  `down`/`move`/`up` events and `btn` presses;
- 8 MB flash header (units boot as 8 MB).

Build (ESP-IDF 5.5): `cd firmware && idf.py set-target esp32s3 && idf.py build && idf.py merge-bin -o cgotchi-touchdiag.bin`,
then copy `build/cgotchi-touchdiag.bin` to `site/`. The merged image is written at 0x0 and replaces the device firmware.
