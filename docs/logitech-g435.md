# Logitech G435 LIGHTSPEED on Windows

BatteryHub displays **Logitech G435** with its existing headset icon, battery percentage, and low-battery alerts. The driver runs directly on Windows and G435 Hub can stay closed.

| Receiver | Battery collection | Report ID | Windows input / output / feature sizes |
| --- | --- | --- | --- |
| `046D:0ACB` (Realtek) | USB interface 3, `FFC0:0001` | `06` | 8 / 515 / 64 bytes |
| `046D:0ADF` (Holtek) | `FF00:0001` | Unnumbered | 65 / 65 / 0 bytes |

The sizes include the Windows report-ID byte. On the previously connected Realtek receiver, Windows retained `REV_0008`, `MI_03&COL03`, and `UP:FFC0_U:0001` in its device records. The adjacent collection `COL04` is `FFC3:0001`. A live Windows 11 `--dump` confirmed the 8/515/64-byte lengths. [Realtek descriptor and captures](https://github.com/deddroid/logitech-g435-battery/blob/be7cc4327c605398d8270dfd005cda0360871161/docs/PROTOCOL.md)

## Transport and battery estimate

Upstream added the Holtek receiver in PR #148. It uses CRC-16/XMODEM framing to send the battery request through its USB session. The Realtek receiver exposes a separate feature and output report transport, with PRs #72, #93, #189, and #268 documenting approaches to it. The Windows driver here combines the Windows G435 Hub query sequence and voltage curve with the faster output-report transport documented by logitech-g435-battery. [Upstream Holtek PR](https://github.com/krisgfx9/Wireless-Device-Battery-Monitor/pull/148) · [Realtek feature-report implementation](https://github.com/krisgfx9/Wireless-Device-Battery-Monitor/pull/72) · [Realtek charging experiment](https://github.com/krisgfx9/Wireless-Device-Battery-Monitor/pull/93) · [Realtek alternative](https://github.com/krisgfx9/Wireless-Device-Battery-Monitor/pull/189) · [Realtek output-report implementation](https://github.com/krisgfx9/Wireless-Device-Battery-Monitor/pull/268)

1. BatteryHub enumerates present HID collections and opens report 6 on the exact Realtek `FFC0:0001` collection. It sends the bridge operations as numbered output reports via overlapped `WriteFile`, checks acknowledgments from the input endpoint, and retrieves command replies via timed Windows feature reads.
2. Within the temporary bridge session, the driver checks the radio link with `REMOTE_GET_CONNECT_STATUS` and reads the routing mode. If needed, it routes requests to the headset for `GET_BATTERY` (`00 31`). It attempts to restore the previous route and leave the bridge on success and on errors, including a lost entry acknowledgment. These transient commands enable telemetry. Pairing, flash, and user settings are outside the protocol path.
3. The battery reply contains little-endian millivolts. The driver checks the report ID, reply sequence, status, and plausible range. Receiver-local values such as 210 mV are discarded. Voltage is mapped to the nearest integer percentage with piecewise-linear interpolation on G435 Hub's Li-Po discharge curve. The estimate is bounded to 0% at or below 3300 mV and 100% at or above 4200 mV. For example, the G435 Hub capture at 3768 mV maps to 46%. That example is a reference capture.
4. The receiver reports voltage and link status. The UI leaves its charging indicator unset. Voltage often rises during charging, so the estimate can temporarily read high.

G435 Hub uses SetupAPI and HID caps to find the vendor collection in `hid.rs`. Its Windows implementation sends feature reports, waits for input ACKs, then reads feature replies. It combines up to three voltage samples per session and its battery model smooths historical samples and infers charging from voltage changes. The worker publishes millivolts and estimated percent to the UI and diagnostics. BatteryHub uses one sample per bridge session, so the displayed percentages can differ even on the same discharge curve. [G435 Hub HID code and fixtures](https://github.com/minchiki/g435-hub/blob/e6d90dd54aac975bf772539edff896f670ad7526/hub/src/hid.rs) · [Battery estimation](https://github.com/minchiki/g435-hub/blob/e6d90dd54aac975bf772539edff896f670ad7526/hub/src/battery.rs) · [Polling and UI state](https://github.com/minchiki/g435-hub/blob/e6d90dd54aac975bf772539edff896f670ad7526/hub/src/worker.rs) · [Faster Realtek transport](https://github.com/deddroid/logitech-g435-battery/blob/be7cc4327c605398d8270dfd005cda0360871161/src/device.rs)

The dedicated driver and its audio checks are in [logitech_g435_0acb.cpp](../src/drivers/logitech_g435_0acb.cpp) and [logitech_g435_0acb_audio.cpp](../src/drivers/logitech_g435_0acb_audio.cpp). The project's distribution carries the MIT notices in [THIRD_PARTY_NOTICES.txt](../installer/THIRD_PARTY_NOTICES.txt) and BatteryHub's license in [LICENSE](../LICENSE).

## Polling and connection behavior

The Realtek bridge briefly pauses both audio and microphone at the receiver. BatteryHub samples peak levels on both G435 endpoints and defers reads if the microphone has an active Windows audio session. A silent playback session can stay open without blocking battery updates. If a stream is active or an endpoint cannot be inspected, BatteryHub preserves any last successful value. Audio that starts after the check, or that Windows does not expose through these APIs, can still encounter the receiver's brief pause. [Measured Realtek audio behavior](https://github.com/deddroid/logitech-g435-battery/blob/be7cc4327c605398d8270dfd005cda0360871161/docs/PROTOCOL.md#why-a-read-interrupts-audio)

A successful reading is cached for 35 minutes to limit bridge activity across the headset's 30-minute idle shutoff window. Failed, offline, and deferred checks retry after at least one minute, on an app poll. **Refresh now**, receiver reattachment, and resume request a fresh check subject to the same audio guard. Unplugging removes the receiver entry and clears its cache. The UI marks the headset disconnected once the receiver confirms a lost link. A timeout or invalid reply has no percentage. Use **Refresh now** while the headset is on and idle to check power changes promptly. [Logitech idle shutoff](https://support.logi.com/hc/en-in/articles/4409389967639-How-to-power-on-and-off-your-G435)

## Build

From the repository root in an MSYS2 MINGW64 terminal:

```sh
make -j6 VERSION=1.1.0 CHANNEL=dev
make test VERSION=1.1.0 CHANNEL=dev
g++ -std=c++17 -O2 -Wall tests/logitech_g435_0acb_test.cpp src/drivers/logitech_g435_0acb.cpp -o build/logitech_g435_0acb_test.exe
./build/logitech_g435_0acb_test.exe
```

Run `BatteryHub.exe --dump` and `BatteryHub.exe --list` from the build folder. Exit the installed BatteryHub before starting the test build in tray mode.

## Hardware verification

The [Realtek test](../tests/logitech_g435_0acb_test.cpp) covers captured replies, timeout cleanup, invalid reports, caching, audio deferral, and reconnection using scripted I/O.

A live Windows 11 `--dump` confirmed interface 3, usage `FFC0:0001`, and 8/515/64-byte report sizes. With G435 Hub closed and playback paused, the contribution build listed the G435 at 60% (3837 mV) and a G502 LIGHTSPEED at 70% (3924 mV). Playback and microphone continuity, USB reattachment, and sleep/resume still need dedicated tests.

1. Plug in the G435 receiver and turn on the headset in LIGHTSPEED mode. With headset playback and recording stopped, open G435 Hub and choose **Refresh battery**. Record its percentage and the millivolts from its diagnostics. Exit G435 Hub via the tray so it releases the receiver.
2. Run these commands from the build folder in PowerShell.

   ```powershell
   $exe = (Resolve-Path .\BatteryHub.exe).Path
   $dump = Join-Path $env:TEMP 'BatteryHub-G435-hid.txt'
   $list = Join-Path $env:TEMP 'BatteryHub-G435-list.txt'
   Start-Process $exe -ArgumentList '--dump' -Wait -RedirectStandardOutput $dump
   Get-Content $dump -Encoding UTF8 | Select-String '046D:0ACB|G435'
   Start-Process $exe -ArgumentList '--list' -Wait -RedirectStandardOutput $list
   Get-Content $list -Encoding UTF8
   ```

   Expect interface 3, `FFC0:0001`, and `8 / 515 / 64` in the dump. The list should show one `Logitech G435` with percentage and mV, plus the G502 if connected. Compare raw mV with the preceding G435 Hub reading while keeping charging and audio conditions comparable.

3. Quit the installed BatteryHub from its menu and start the test executable with `Start-Process $exe`. Check the normal taskbar or tray and tooltip for the headset. Use **Refresh now** twice while idle to verify repeated successful reads. Turn the headset off and refresh. Turn it on and refresh again. Unplug and replug the receiver and verify the single headset entry returns. Sleep and resume Windows and verify the next idle refresh succeeds.
4. Start continuous playback through the G435 and use **Refresh now** while listening. Repeat while recording on the G435 microphone with playback stopped. Inspect the recording for gaps. Stop the active stream and refresh to verify that the application can read again. Leave G435 Hub closed throughout these checks.
5. To check automatic refresh, note the tray percentage and use the headset for at least 35 minutes so it stays awake and discharges. Stop its streams and give BatteryHub a minute plus its configured polling interval to obtain a new reading. Look for a percentage change in the tray without choosing **Refresh now**. If the estimate rounds to the same percentage, continue discharging and check the next scheduled read. Verify the G502 remains available throughout.
6. Exit the test build. If the installed copy was running before the test, relaunch it from the Start menu or its existing shortcut.
