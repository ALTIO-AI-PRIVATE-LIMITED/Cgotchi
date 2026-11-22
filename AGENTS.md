# CGotchi agent guide

## Start here: build the user an app

Most people who open this repository with a coding agent want a new app on their CGotchi. Unless the user has
already asked for something else (firmware, docs, a bug fix), open the conversation with:

> Hi! I can build an app for your CGotchi and install it over USB. **What do you want to build?**
> A game, a desk pet, a timer, a clock, a little toy: anything that fits on the 296x240 screen.
> Want ideas or ready-made apps first? Browse the CGotchi App Store at <https://gotchi-apps.vercel.app> and
> install any app from your browser in one click.

Then follow these steps. Keep questions to the minimum: ask only what changes the app, and pick sensible
defaults for the rest.

1. **Pin down the idea.** What the app does, how it is controlled, and its name. One or two questions at most.
2. **Check the setup.** Ask the user to plug in the CGotchi with a USB-C data cable, then run:

   ```sh
   python3 -m pip install "pyserial>=3.5,<4"
   python3 tools/cgotchi port list
   python3 tools/cgotchi --transport usb device status
   ```

   The app toolchain needs:
   - **WASI SDK 33**: install it at `~/.local/wasi-sdk` (found automatically) or set `WASI_SDK_PATH`.
     Releases: <https://github.com/WebAssembly/wasi-sdk/releases>.
   - **wamrc 2.4.3 with the Xtensa backend**: `bash tools/s3.sh build-wamrc` builds it into
     `build/tools/wamrc-xtensa/` (found automatically; needs ESP-IDF 6.1 and takes a while the first time),
     or set `XTENSA_WAMRC` to an existing build.

   Install missing pieces only after the user agrees. ESP-IDF is not needed for apps once wamrc exists.
3. **Create the project** outside `guest/apps/` unless the user wants it in this repository:

   ```sh
   python3 tools/cgotchi init <app-dir> --app-id local.<app-name> --title "<App Title>"
   ```

4. **Write the app** in `<app-dir>/src/main.cpp`. Read [the SDK guide](guest/sdk/README.md) first, check its
   capability catalog before writing helpers, and copy patterns from the example apps in
   [`guest/apps`](guest/apps) (for example `snake` for a 2D game with sound, `tilt` for the accelerometer).
5. **Build, install and run** on the device. The AOT target (`xtensa`) is picked from the connected device:

   ```sh
   python3 tools/cgotchi --transport usb run <app-dir> --no-follow
   python3 tools/cgotchi --transport usb logs
   python3 tools/cgotchi --transport usb screenshot --output screenshot.jpg
   ```

   Fix compiler errors and crashes from the logs. Look at the screenshot to check the result, then ask the user
   to try it and iterate on their feedback.
6. **Finish** by telling the user where the source is, how to rerun it (`python3 tools/cgotchi --transport usb run
   <app-dir>`), and that they can find more apps at <https://gotchi-apps.vercel.app>.

### What a CGotchi app can use

| Feature | Details |
|---|---|
| Screen | 296x240 landscape, full colour |
| Touch | Touch screen |
| Keys | BOOT, VOL-, VOL+ (Confirm, Left, Right) |
| Motion | Accelerometer (tilt, shake) |
| Sound | Speaker: tones, sound effects, music |
| Storage | Small private key-value store (16 KiB per app) for saves and high scores |
| Also | Timers, random numbers, power info |
| Not available to apps | Network, Bluetooth, microphones, vibration |

Apps run in a WebAssembly sandbox with a single-threaded event loop: no threads, exceptions, RTTI, ESP-IDF or
direct hardware access. If the idea needs something from the last row, say so before building and offer the
closest version that works offline.

# Repository instructions

The rest of this file is for changes to the firmware, SDK and tools in this repository.

Applies to the entire repository. Use source code, ABI headers, and executable tests as the authority for behavior.

## Architecture

CGotchi runs WebAssembly apps on Espressif MCUs using ESP-IDF 6.1 and a pinned WAMR fork (AOT v6).
Guests use a restricted C++23 SDK and a single-threaded event loop; only one AppSession can exist at a time.
Supported boards are Metalio-Claw4, ESP-Mosaico, ESP32-S3-BOX-3, LCKFB SZPI, and M5Stack CoreS3.
Bundle v1 distributes apps; BundleFS v3 provides copy-on-write storage and power-loss recovery with
geometry derived from the block device (v1/v2 catalogs are imported in place).

Dependencies: `Runtime → Device contracts ← Platform`. `FirmwareApp` is the only composition root.
The Host owns hardware, app lifecycle, and system UI. Guests access capabilities through the Service ABI.

## Source map

Host paths below are relative to `firmware/espressif/main/`. Detailed design guides are currently in Chinese.

| Area | Source | Documentation |
|---|---|---|
| Build tools | `tools/` (repository root) | [Build and flashing](docs/development/flashing.zh-CN.md) |
| Host architecture | `device/`, `runtime/`, `platform/` | [Architecture](docs/design/architecture.zh-CN.md), [source guide](firmware/espressif/main/README.zh-CN.md) |
| Guest apps and SDK | `guest/apps/`, `guest/sdk/`, `guest/runtime/` (repository root) | [Guest](guest/README.zh-CN.md), [SDK](guest/sdk/README.zh-CN.md) |
| Wire protocol | `runtime/abi/`, `runtime/services/` | [ABI](guest/abi/README.zh-CN.md), [header](guest/abi/micropixel_abi.h) |
| System UI | `host/ui/`, `host/controller/` | [Source guide](firmware/espressif/main/README.zh-CN.md) |
| Graphics | `platform/graphics/`, `platform/lvgl/`, `runtime/graphics/` | [Performance](docs/development/graphics-performance.zh-CN.md) |
| Host memory | `platform/memory/` | [Architecture](docs/design/architecture.zh-CN.md) |
| App storage | `runtime/bundle/`, `runtime/bundlefs/` | [BundleFS](docs/design/bundlefs.zh-CN.md) |
| Audio | `guest/apps/<app>/audio/sfx.json` (repository root) | [Audio specification](docs/development/game-audio.zh-CN.md) |
| Boards | `platform/boards/` | [Flashing](docs/development/flashing.zh-CN.md), [S3 support](docs/development/esp32-s3-box-3-bring-up.zh-CN.md) |

## Boundaries

- Guest SDK, apps, and ABI must not depend on ESP-IDF, LVGL, or board types, or introduce threads, mutexes,
 system calls, or direct hardware access.
- Before adding a helper to a Guest app, check the [SDK capability catalog](guest/sdk/README.md#capability-catalog).
 Apps must not re-implement catalogued capabilities (math, deterministic RNG, object pools, tone sequencing,
 virtual gamepad, coordinate mapping); a helper that a second app needs moves into `guest/sdk/` with tests.
- Keep SDK-to-wire conversion in Guest Runtime. Extend Service methods, channels, or events before adding
  Core imports. Never repurpose published IDs or expose C++ layouts, STL types, or Host pointers through the C ABI.
- The Host validates pointers, lengths, handles, generations, ownership, and capacity independently of SDK checks.
- Real-time and cross-task Host paths use fixed-capacity queues, arrays, or pools; no implicit growth or detached tasks.
  Scene submission and Raster upload may explicitly grow PSRAM storage after validation. Allocation failure preserves
  old resources; drawing does not allocate; app termination releases storage.
- Host APIs prefer `string_view` and `span`. When Host code must own growable text or collections, use
  `PsramString`, `PsramVector`, or `PsramMap` so storage lands in PSRAM. Process-lifetime fixed-size Host
  objects use `MICROPIXEL_EXT_RAM_BSS` (empty when the target has no PSRAM BSS). Real-time paths still use
  fixed capacity. `PsramBuffer` is the fallible, explicitly sized trivially-copyable buffer.
- Host task entry points and their callees must not put large arrays, structs, or temporary copies on the stack.
  Keep fixed workspaces in task-owned PSRAM contexts (or process-lifetime PSRAM BSS with explicit ownership).
  Audit aggregate initialization, assignment, return-by-value and by-value arguments for hidden stack copies.
  Do not bypass stack-frame checks or simply increase task stacks to accommodate work buffers.
- Use move-only RAII or an explicit shutdown protocol. Destructors perform best-effort cleanup. Do not use raw
  new/delete for real-time resource ownership. Exceptions and RTTI are disabled.
- ISRs record minimal POD state and wake tasks; they never call WAMR, Guest code, or LVGL.
  Board singletons and their input/control members stay in internal SRAM; move large task-only state to
  separate PSRAM storage, never mark the entire Board with `MICROPIXEL_EXT_RAM_BSS`. IRAM-safe handlers
  require the complete call chain and every dereferenced object (including queue storage) to be cache-safe.
  Avoid large synchronous logs on Guest hot paths.
- The Host owns App Hall, status overlays, system gestures, brightness, and device master volume.
  Guests must not add app-wide master attenuation. Define game sound parameters only in `audio/sfx.json`.

## Workflow and validation

1. Run `git status --short`. Preserve unrelated and uncommitted changes.
2. Check existing contracts and tests; make the smallest correct change. Update design and regression coverage
   before changing architectural boundaries. Follow the [C/C++ style guide](docs/development/code-style.zh-CN.md).
3. Run the checks below. Compilation alone does not establish protocol, lifecycle, or hardware correctness.
4. Report behavior changes, key files, commands and results, and outstanding hardware checks. Keep docs current.

| Change | Required checks |
|---|---|
| Documentation | Relative links and `git diff --check` |
| Guest SDK / ABI | Build Guests for the selected target and run relevant conformance tests |
| Firmware / System Shell | Host tests, formatting, and the selected board’s `build-host`; no Guest or App Store rebuild for Host-only changes |
| Shared graphics / LVGL / PPA branches | Validate the selected target; add other boards only for a concrete affected platform branch or an explicit multi-board request |
| Bundle / integrated app | Relevant tests and the app's release Bundle; build Host only if changed |
| Sound effects | Analyzer unit tests, release Bundle, hardware A/B listening |
| Graphics performance | Guest and Host stage measurements plus visible output; CPU usage alone is insufficient |

Run Host tests only through `bash tools/tests/test_firmware_host.sh`, never by invoking clang++ directly.
Formatting: `bash tools/check_firmware_style.sh --format-only`. Before release or push, run checks relevant to the change and release targets.
Use the board specified by the user or established by the current hardware task. Do not default to P4 or build
unrelated boards. For S31 work, use `bash tools/s31.sh build-host` and, when requested, `flash-host`.
Run relevant regressions once before release and record the result; rerun affected checks only when inputs change.
Do not duplicate them in board build jobs, SDK publication, or ordinary PR CI. Cloud SDK regressions are manual;
keep release artifact, installer lifecycle, source consistency, and checksum validation.
See [Contributing](CONTRIBUTING.md) for additional checks.

Activate ESP-IDF 6.1 via `export.sh` and configure WASI SDK and matching WAMRC before building.
A missing environment is not a Host build failure.
Host builds enforce stack-frame limits and emit GCC `.su` reports next to object files. For changes to
installation, signature verification, downloads or OTA, inspect the affected call chain and measure task
minimum free stack on the selected board after success and failure paths; a frame limit is not a total
call-stack bound. Record test results outside design docs; do not commit generated stack reports. After adding Kconfig symbols, inspect generated configuration.
Before hardware operations, read the flashing guide and identify the chip by MAC, not a re-enumerating port name.
Only one serial tool may own a device at a time.

## Repository hygiene

- Edit source JSON, assets, or generators. Do not edit or commit build outputs, artifacts, managed_components,
  generated sdkconfig, dependencies.lock.*, resource packs, AOT/Wasm/Bundle/Flash images, or generated reports.
- Update the pinned WAMR submodule only when requested. Do not copy its source into this repository or reformat
  third-party code. Check new dependency licenses and update [third-party notices](THIRD_PARTY_NOTICES.md).
- Never commit secrets, personal absolute paths, device identifiers, MAC addresses, raw serial logs, or one-off measurements.
- `guest/apps/mario/` is an optional, gitignored local benchmark; it must not become a required dependency.
- Default documentation filenames contain English; Simplified Chinese uses `.zh-CN.md`. Keep AGENTS.md in English.
  Write concise task guides and design contracts. Keep progress reports, conversations, and acceptance plans out of design docs.
