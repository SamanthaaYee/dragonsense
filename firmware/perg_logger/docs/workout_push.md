# Workout push (MVP) — spec for agents

Read this before changing anything under `web/`, `Workout.h`, or the BLE code in
`firmware/perg_freenove_force_display/`. It is the source of truth for the
feature's behavior and wire format. If you change behavior, update this file
in the same commit.

## Goal

A teammate opens a small web page on their phone, builds an interval workout
(a list of **pieces**, each followed by a **rest**), and pushes it to the Perg
**display** over Bluetooth. On the monitor, they tap **WORKOUT OFF** to arm the plan and then tap
**START**. The display guides them through each piece and rest, then stops
the session when the last piece finishes.

Audience is the project team only (a handful of people, all on iPhone using the
**Bluefy** browser, since iOS Safari has no Web Bluetooth). This is an MVP:
prefer simple and robust over featureful.

### Non-goals (do not build without being asked)

- Accounts, a backend, cloud storage, or a workout library.
- Wi-Fi on any board.
- Changes to either sensor logger (`perg_full_logger_ble`,
  `final_sensor_and_display_devboard`) or to `record_perg.py`.
- Piece markers in recorded CSVs.
- Support for the old CYD display (`perg_stat_display`). It uses Bluetooth
  Classic, which browsers cannot reach.
- Per-piece stats, splits history, or a results screen.

## System overview

```text
 phone (Bluefy)                      Freenove ESP32-S3 display              sensor ESP32
 web/index.html  --BLE write-->  workout characteristic                   (logger, BLE central)
                 <--BLE read---  status (same characteristic)
                                 Workout.h: parse, runner, status text
                                 telemetry characteristic  <--BLE write--  telemetry @10 Hz
                                 command characteristic    --notify---->   START / STOP
```

- The **display** (BLE peripheral/GATT server) is the only firmware that
  changes. The logger keeps connecting to it exactly as before.
- The **logger owns the session**: elapsed time, distance, and START/STOP state.
  The runner uses the session state, elapsed seconds, and distance from
  the existing `stats` message. The display sends the same START/STOP
  commands used by its touch button.
- The **workout runner lives on the display** and is driven purely by
  telemetry. It never keeps its own clock.

### Hardware constraints that shaped this

- The sensor ESP32 samples on ADC2 pins (GPIO 27, 14). ADC2 is unusable while
  Wi-Fi runs, so no Wi-Fi there. The S3 has no such restriction.
- Arduino-ESP32 core **3.3.12** builds the S3's BLE on **NimBLE** with
  `CONFIG_BT_NIMBLE_MAX_CONNECTIONS=3`. The logger plus one phone fit. Callbacks
  identify connections by `ble_gap_conn_desc::conn_handle`.
- NimBLE stops advertising when a central connects. The display must restart
  advertising after **every** connect (while below the connection limit) so a
  phone can join while the logger is connected.
- NimBLE runs `onWrite` synchronously **before** the write response is sent,
  so a status value set in `onWrite` is visible to a read issued after the
  write resolves. Keep `onWrite` fast and non-blocking.
- NimBLE `notify()` only reaches subscribed clients, so the phone never
  receives START/STOP.

## Separation rule (keeps the transport swappable)

| Layer | Where | Knows about |
| --- | --- | --- |
| Format, validation, runner, status text | `firmware/perg_freenove_force_display/Workout.h` | Nothing hardware-specific; plain C++ |
| Line framing | `BleLineFramer.h` (`BasicLineFramer<N>`) | Bytes and newlines only |
| BLE transport | `perg_freenove_force_display.ino` | Receives a line, calls `workout::parse`, sets status |
| Screen | `perg_freenove_force_display.ino` | Draws `workout::describe(...)` output |
| Web encoding/validation | `web/workout.js` | Nothing BLE-specific |
| Web transport | `web/index.html` | Web Bluetooth only |

To switch transport later (Wi-Fi hotspot, USB Web Serial, cloud fetch), replace
only the transport rows. The workout line format stays the same: a hotspot
HTTP handler or a Serial line reader would call the same `workout::parse`.
Never put BLE, Arduino, or TFT code into `Workout.h` or `workout.js`.

## Wire format v1

One ASCII line, terminated by `\n` on the wire (the terminator is not part of
the line for parsing or checksums).

```text
line   = "W1" *( ";" piece )
piece  = kind target "," rest
kind   = "T" / "D"          ; T = time piece (seconds), D = distance piece (metres)
target = number
rest   = number              ; rest after this piece, seconds
number = "0" / ( %x31-39 *3DIGIT )   ; canonical decimal, 1-5 digits, no leading zeros
```

Examples:

| Line | Meaning |
| --- | --- |
| `W1;T240,60;T240,60;D500,90` | 4:00 on / 1:00 rest, 4:00 on / 1:00 rest, 500 m then 1:30 rest |
| `W1;D1000,0` | One 1000 m piece |
| `W1` | Clear the stored workout |

Limits (enforced on **both** the page and the display):

| Field | Min | Max |
| --- | --- | --- |
| Pieces | 1 (0 means clear) | 20 |
| Time target `T` | 10 s | 3600 s |
| Distance target `D` | 50 m | 10000 m |
| Rest | 0 s | 1800 s |
| Line length | — | 242 characters (`"W1"` + 20 × `";D10000,1800"`) |

No whitespace, no trailing separator, no other characters. Anything else is
rejected; the display never partially accepts a workout.

The rest after the **last** piece is stored but ignored: the workout ends
when the last piece's target is reached.

## BLE GATT

Service `be7a1000-7c2d-4e58-a608-dbea31094d61` (unchanged), advertised by
name `perg-force-display`.

| Characteristic | UUID | Properties | Used by |
| --- | --- | --- | --- |
| Force telemetry | `be7a1003-7c2d-4e58-a608-dbea31094d61` | write | logger → display (`stats` and `drive`) |
| Command | `be7a1002-7c2d-4e58-a608-dbea31094d61` | notify | display → logger (unchanged) |
| **Workout** | `be7a1004-7c2d-4e58-a608-dbea31094d61` | read, write | phone ↔ display (new) |

### Push sequence (page)

1. `requestDevice({ filters: [{ services: [SERVICE] }] })`, connect, get the
   workout characteristic.
2. Write `line + "\n"` in chunks of **at most 20 bytes**, in order, each an
   acknowledged write (`writeValueWithResponse`, falling back to
   `writeValue`). 20 bytes fits the default ATT MTU, so no MTU assumptions.
3. Read the characteristic once and interpret the status below.
4. Disconnect, freeing the connection slot.

### Status value (display)

After every write the display sets the characteristic value to one of:

| Value | Meaning |
| --- | --- |
| `RECV` | Chunk buffered; no newline yet |
| `OK,<pieces>,<fnv>` | Validated and queued. `<fnv>` is the FNV-1a 32-bit hash of the received line, 8 lowercase hex digits |
| `ERR,<code>` | Rejected; the previous workout is kept |

Error codes: `length`, `version`, `syntax`, `range`, `count`, `busy`.
`busy` means a session is running, or a workout is in progress, on the display.
Tap STOP first. This stops a new plan from starting mid-row.

The display frames lines with a 256-byte buffer. Lines of 243–255 characters
report `ERR,length`. Longer lines are dropped silently, and the status stays
`RECV`. The page never sends either kind.

The page must check that `<fnv>` equals its own hash of the line it sent. A
stale `OK` from an earlier push will not match. The initial value before any
push is `READY`.

FNV-1a 32: `h = 0x811c9dc5; for each byte b: h = (h ^ b) * 0x01000193 mod 2^32`.

## Runner semantics (display)

The display feeds the runner the running state, elapsed time, and
distance from each `stats` update. The runner is a pure function
`advance(plan, previous_state, sample) -> new_state`.

Phases: `Idle → Work → (Rest → Work)* → Finishing → Finished`.

- **Idle**: a stored plan starts piece 1 only after it is armed on the
  LCD and the logger reports that the session started. The baseline is
  the current elapsed time and distance.
- **Work** (piece *i*): progress is `elapsed − start` for `T` pieces or
  `distance − start_distance` for `D` pieces. When progress reaches the
  target:
  - last piece → **Finishing**. While Finishing and the session still runs,
    the display requests STOP on every telemetry update unless a command is
    already pending. The existing 1 s retry handles delivery, and a link drop
    clearing the pending command cannot lose the STOP.
  - rest > 0 → **Rest**, starting at the piece's end.
  - rest = 0 → the next piece starts immediately.
  - Time pieces end exactly at `start + target` (no drift from the 10 Hz
    updates). Distance pieces with no rest carry overshoot into the next piece;
    after a rest the next piece's distance baseline is the distance at the end
    of the rest (distance paddled during rest never counts).
- **Rest**: after `rest` seconds, the next piece starts at `rest_start + rest`.
- **Finishing**: waits for `running` to become false, then becomes **Finished**.
- **Finished**: shows DONE and disarms the workout. Tap DONE to arm it again.
- Stopping during Work or Rest aborts and disarms the workout.
- `elapsed_s` dropping by more than 1 s while in progress means the logger
  started a new session that the display missed; restart from piece 1.
- A telemetry gap may cross several boundaries; the runner settles all of them
  in one update.
- Uploading a plan or rebooting the display leaves the stored plan disarmed.
  A regular recording can start while a plan is stored.
- A new workout push is rejected with `busy` while a session runs or a
  START/STOP command is pending.
- `OK` is returned once the plan is validated and queued. The NVS save happens
  afterwards in `loop()`; a failed save is only logged to Serial.
- Losing the logger link does not reset the runner; it continues from the
  next telemetry.

## Display UI

The top-left strip (left of the START/STOP button, where the `PERG` title
was) shows the workout status with a thin progress bar. It is drawn in the
same frame buffer as the stroke curve, foot balance, and workout metrics.

| State | Text | Color |
| --- | --- | --- |
| No workout stored | `PERG` | white |
| Stored and disarmed | `WORKOUT OFF` | dim |
| Armed | `READY 5 PCS` (`READY 1 PC` for one) | cyan |
| Work | `P2/5 1:23` (time left) or `P2/5 245m` (distance left) | cyan |
| Rest | `REST 0:45` | cyan |
| Finishing / Finished | `DONE` | cyan |

Remaining time and distance round **up**, so `0:00` / `0m` is never shown
mid-piece. The main **TIME** metric shows elapsed session time.

The accepted workout is saved to NVS (`Preferences` namespace `perg`, key
`workout`) as its canonical line and reloaded at boot. An invalid stored line
is ignored.

## Web page

`web/index.html` + `web/workout.js`. These are static files with no build step and no
external dependencies. Web Bluetooth needs HTTPS (or `localhost`), so host
them on GitHub Pages or any HTTPS static host, and open the URL in **Bluefy**
on iPhone or Chrome on desktop/Android.

- Rows of pieces: type (Time / Distance), target (`m:ss` or seconds for time,
  metres for distance), rest (`m:ss` or seconds). Add, duplicate, remove.
- Live preview of the encoded line and of any validation error; Send is
  disabled while invalid.
- **Send to Perg** and **Clear Perg workout** buttons, and a status message for
  each step: connecting, sending, confirmed or error.
- The last workout is remembered in `localStorage` (convenience only; wrap
  access in try/catch).

`web/workout.js` must work both in the browser (global `PergWorkout`) and in
Node (`module.exports`) so tests can run it.

## Tests

- `tests/test_workout_firmware.py`: compiles `Workout.h` natively with `c++`.
  It covers parse acceptance and rejection, limits, canonical round-trip, the
  checksum, every runner transition listed above, and the status text.
- `tests/test_workout_web.py`: runs `web/workout.js` under `node` (skipped
  if absent). It checks encoding, validation, time parsing, the checksum, and
  that JS and C++ agree on the same vectors.
- `tests/test_ble_framing_firmware.py` must keep passing (`BleLineFramer`
  remains a 112-byte framer).

Run `python3 -m unittest discover -s tests -v`.

## Must be checked on hardware (cannot be verified in CI)

1. The display sketch compiles and uploads (ESP32S3 Dev Module, core 3.3.12).
2. The logger connects and telemetry flows as before.
3. With the logger connected, a phone in Bluefy finds `perg-force-display`,
   pushes a workout, gets `OK`, and disconnects. The logger link and
   telemetry are not disturbed, before, during, or after.
4. The phone disconnecting never shows the "Waiting for BLE logger" screen.
5. The workout runs through time and distance pieces with rests and sends
   STOP at the end. Stopping mid-workout returns to WORKOUT OFF.
6. Start a regular session after finishing a workout and confirm it continues
   until manually stopped.
7. Power-cycle the display and confirm that the plan is stored and disarmed.

## Status

- [x] Spec
- [x] `Workout.h` + native tests (with ASan/UBSan)
- [x] Display BLE transport, connection tracking, NVS, UI. Compiles for ESP32S3 Dev
  Module on core 3.3.12. The only warning is the existing `BLE2902`
  deprecation, which is harmless on NimBLE.
- [x] Web page + Node tests (JS and C++ parsers agree on every test vector)
- [ ] Hardware checks above
- [ ] Host `web/` on HTTPS (for example GitHub Pages) and open it in Bluefy

Known MVP limits, accepted for now: if the logger drops mid-workout, pushes stay
`busy` until it reconnects. The first device to write telemetry is treated as
the logger. Neither BLE link is authenticated.
