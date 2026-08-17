# AMTECH AI Guard Hub Progress

This project is building an AI-powered security system for Indian shops, designed to run on a Luckfox Pico Ultra board with a Rockchip RV1106G3 processor.

This document summarizes what exists in the repo right now and how the pieces fit together.

## Current System Shape

The system now has four main parts:

- Hub-side C code in `src/` for GPIO, sensors, alarm decisions, scheduling, camera detection, and modem call/SMS alerts.
- Test programs in `tests/` for the C modules and hardware bring-up diagnostics.
- A Python backend in `backend/` for alert storage, user/shop APIs, and media upload URLs.
- Patch files in `patches/` for wiring AMTECH logic into Rockchip's cloned `rknn_model_zoo` YOLOv5 example.

## Hub Runtime Logic

The main long-running hub program is `src/runtime_loop.c`.

It handles:

- GPIO interrupt watches for wired sensors.
- Schedule ticking and real-time arm/disarm decisions.
- Alarm ticking, including siren auto-stop and call escalation ticking.
- Camera detection results.
- Optional testing override with `--force-armed` or `AMTECH_FORCE_ARMED=1`.

The runtime uses sysfs GPIO edge detection and `poll()`. It does not continuously poll sensor values in a tight loop. GPIO inputs are configured for edge events, the value file is opened, and an initial read clears stale sysfs edge state before entering the poll loop.

The GPIO poll timeout is `100ms`, so `alarm_logic_tick()` runs frequently enough for siren timing and call escalation timing.

## Configuration

The hub reads a simple config file from:

```text
/root/amtech_config.txt
```

The path can be overridden with:

```sh
AMTECH_CONFIG_PATH=/path/to/config.txt
```

Supported keys:

```text
SHUTTER_COUNT=1
PANIC_ENABLED=1
SMOKE_ENABLED=0
SCHEDULE_ARM=23:00
SCHEDULE_DISARM=06:00
MODEM_DEVICE=/dev/ttyS5
ALERT_CONTACT_1=+918550991121
ALERT_CONTACT_2=+919922434811
ALERT_CONTACT_3=+919922435710
CAMERA_ENABLED=0
CAMERA_RTSP_URL=rtsp://user:pass@camera-ip:554/stream1
CAMERA2_ENABLED=0
CAMERA2_RTSP_URL=
BACKEND_BASE_URL=https://amtech-ai-guard-hub-production.up.railway.app
DEVICE_CONFIG_TOKEN=
```

Important defaults:

- `SHUTTER_COUNT=1`
- `PANIC_ENABLED=1`
- `SMOKE_ENABLED=0`
- `SCHEDULE_ARM=23:00`
- `SCHEDULE_DISARM=06:00`
- `MODEM_DEVICE=/dev/ttyS5`
- `CAMERA_ENABLED=0` and `CAMERA2_ENABLED=0`, so camera detection is disabled unless explicitly configured.
- `DEVICE_CONFIG_TOKEN` is empty, so backend config sync is disabled unless explicitly configured.

The shutter and smoke config defaults are deliberately conservative so unwired/floating pins are not watched accidentally.

Backend config sync now updates only `SCHEDULE_ARM`, `SCHEDULE_DISARM`, and `ALERT_CONTACT_1/2/3`. It preserves local hardware install keys such as shutter count, camera URLs, modem device, and sensor enable flags.

Status as of this checkpoint:

- Backend source and device simulation tests are complete.
- Live Railway endpoint verification passed for owner-authenticated `GET`/`PUT`, unauthenticated rejection, and invalid schedule rejection.
- Live Railway device-token verification is pending setting `DEVICE_CONFIG_SYNC_TOKEN` in Railway and matching `DEVICE_CONFIG_TOKEN` on the hub.
- Real hardware polling from the Luckfox board is deferred until SSH/board access is available again.

## GPIO And Sensor Hardware

GPIO output and input are implemented with the standard Linux sysfs GPIO interface in:

- `src/gpio_control.c`
- `src/sensor_input.c`

There is a `SIMULATE_GPIO` mode for laptop/Docker testing.

Current hardware pins:

- Siren relay: GPIO49, active LOW. LOW means ON, HIGH means OFF.
- Strobe relay: GPIO48, active LOW. LOW means ON, HIGH means OFF.
- Panic switch: GPIO32, active HIGH, rising edge.
- Shutter 1 NC: GPIO33.
- Shutter 1 NO: GPIO40.
- Shutter 2 NC: GPIO41.
- Shutter 2 NO: GPIO72.
- Smoke detector: GPIO54, active LOW.

Relay outputs use glitch-safe initialization through sysfs `direction` writes of `high`, so the relay is set OFF before becoming an output.

## Shutter Logic

Shutters use dual-wire NC/NO sensing, implemented in `shutter_read_dual_state()`.

Truth table:

```text
NC LOW,  NO HIGH -> shutter closed
NC HIGH, NO LOW  -> shutter open
NC HIGH, NO HIGH -> tamper / wire open
NC LOW,  NO LOW  -> fault / short circuit
```

Alarm behavior:

- `SHUTTER_OPEN` triggers only while ARMED.
- `SHUTTER_TAMPER` triggers regardless of armed state.
- `SHUTTER_FAULT` logs a hardware fault and does not trigger the alarm.
- `SHUTTER_CLOSED` does nothing.

If `SHUTTER_COUNT=1`, Shutter 2 pins are not exported, watched, read, or passed into alarm logic.

## Panic And Smoke Logic

Panic:

- GPIO32.
- Active HIGH.
- Rising-edge watch.
- Triggers immediately regardless of ARMED/DISARMED state.

Smoke:

- GPIO54.
- Active LOW.
- Both-edge watch.
- Triggers regardless of ARMED/DISARMED state.
- Disabled by default with `SMOKE_ENABLED=0`.

All sensors now use the same `200ms` confirmation window. An edge wakes the runtime, the code waits 200ms, rereads the pin, and only triggers if the alarm state is still present.

## Core Alarm State Machine

Implemented in `src/alarm_logic.c`.

The alarm supports:

- ARMED/DISARMED state.
- Person detection confirmation.
- Shutter open/tamper/fault behavior.
- Panic button bypass.
- Smoke/fire bypass.
- Siren/strobe relay control.
- Backend alert-history logging support.
- SIM modem call/SMS dispatch.

Person detection:

- Person class is class ID `0` or class name `"person"`.
- Confidence must be greater than `0.25`, based on the 54-image ground-truth threshold sweep.
- Camera person detection requires 2 qualifying frames from the same camera source.
- After the system arms, camera detections have a 10-second camera-only grace period; they are logged but cannot trigger during that window. Shutter, panic, and smoke triggers are not delayed by this grace period.
- Person detection does not trigger while DISARMED.

Siren and strobe:

- Siren and strobe turn ON together when an alarm triggers.
- Siren automatically turns OFF after `5000ms`.
- Strobe stays ON after the siren stops.
- `alarm_logic_is_triggered()` stays true after siren auto-stop.
- `alarm_logic_reset()` turns both siren and strobe OFF and clears alarm state.

Repeated triggers:

- Any fresh trigger reactivates the siren and restarts the 5-second siren timer, even if the alarm was already active.
- Notification/call/SMS dispatch is rate-limited by a shared `30000ms` cooldown to avoid alert spam.
- The siren/strobe reactivation is independent from the call/SMS alert-dispatch cooldown.

## Camera Detection

Camera capture/detection has been integrated locally into the runtime through:

- `src/camera_detection.c`
- `src/camera_detection.h`

The implementation uses the subprocess approach proven by the standalone RTSP and dataset tests:

- Capture one frame with `ffmpeg`.
- Use RTSP over TCP.
- Use reduced probe settings: `-analyzeduration 1000000 -probesize 32768`.
- Scale to 480x480 with aspect-ratio-preserving letterbox: `-vf scale=480:480:force_original_aspect_ratio=decrease,pad=480:480:(ow-iw)/2:(oh-ih)/2`.
- Save the captured frame as a per-camera RGB PPM under `/tmp`.
- Run luminance-only CLAHE preprocessing with `/root/amtech_clahe_ppm`.
- Save the enhanced frame as a per-camera CLAHE PPM under `/tmp`.
- Encode the enhanced frame to a per-camera JPEG under `/tmp`.
- Run Rockchip's `rknn_yolov5_demo` against that frame.
- Parse `person @ ... confidence` lines from the demo output.

CLAHE is now the production path because it improved real dataset recall on the 54-image AMTECH ground-truth set:

```text
Baseline 480 letterbox, threshold >0.25:
  TP=27 FP=0 TN=2 FN=25
  recall=51.9%, precision=100.0%, accuracy=53.7%
  average total dataset frame time=848ms

480 letterbox + CLAHE, threshold >0.25:
  TP=32 FP=0 TN=2 FN=20
  recall=61.5%, precision=100.0%, accuracy=63.0%
  average total dataset frame time=1282ms
```

Net effect: +5 true positives with zero new false positives. CLAHE gained detections on `data_02`, `data_15`, `data_17`, `data_19`, `data_28`, `data_34`, and `data_41`, while losing two borderline detections on `data_43` and `data_52`.

Single-frame person confirmation was adopted after rerunning the two no-person ground-truth images (`data_21.jpg` and `data_45.jpg`) on the real board through the production 480 letterbox + CLAHE path at threshold `>0.25`. Both returned `NO_PERSON_DETECTED`, so the empty-scene false-positive count remained `0/2`.

The added dataset-frame latency was about 434ms on average:

```text
baseline avg: preprocess=305ms, inference=543ms, total=848ms
CLAHE avg:    preprocess=297ms, CLAHE=117ms, encode=294ms, inference=573ms, total=1282ms
```

A faster direct-JPEG CLAHE output path was tested using `stb_image_write`, but it changed the detector outputs and did not preserve the +5 TP improvement. Production therefore keeps the slightly slower PPM-to-ffmpeg-JPEG encode step because it is the accuracy-proven path.

Model selection is now closed for production:

- YOLO11n was downloaded, converted, and built as a real RV1106 test, but it failed during `rknn_init` on the board with an unsupported `MatMul` operation. Earlier YOLOv8/YOLOv10 tests failed in the same operator-support family, so newer attention/MatMul-based YOLO generations are not considered production candidates for this RV1106 runtime.
- YOLOv5m was tested because it stays in the RV1106-compatible YOLOv5 family. It initialized and ran on the board, but under the same 480 letterbox + CLAHE pipeline it reached only `57.7%` recall with `1492ms` average dataset-frame time.
- Current YOLOv5s + 480 letterbox + CLAHE remains better: `61.5%` recall with `1282ms` average dataset-frame time.

Final production model choice: YOLOv5s + 480x480 letterbox + CLAHE.

Real paths used by the camera module:

```text
/root/rknn_yolov5_demo_export/rknn_yolov5_demo
/root/rknn_yolov5_demo_export/model/yolov5.rknn
/root/amtech_clahe_ppm
/tmp/amtech_front_live_frame.ppm
/tmp/amtech_front_live_frame_clahe.ppm
/tmp/amtech_front_live_frame.jpg
/tmp/amtech_front_camera_detection_output.txt
/tmp/amtech_parking_live_frame.ppm
/tmp/amtech_parking_live_frame_clahe.ppm
/tmp/amtech_parking_live_frame.jpg
/tmp/amtech_parking_camera_detection_output.txt
```

The runtime supports two independently configured camera workers:

- Front camera: `CAMERA_ENABLED=1` and `CAMERA_RTSP_URL=...`
- Parking camera: `CAMERA2_ENABLED=1` and `CAMERA2_RTSP_URL=...`

Each enabled camera has its own pthread and its own temporary frame/output files. Capture and preprocessing can run in parallel, but the RKNN demo/NPU subprocess is protected by one shared inference mutex so only one camera uses the NPU at a time.

Camera threads never call `alarm_logic_*` directly. They publish source-tagged results (`intrusion-front` or `intrusion-parking`) into a mutex-protected FIFO queue. The main runtime thread consumes those results and calls the source-aware alarm detection handlers, keeping alarm state mutations single-threaded.

Timeouts:

- ffmpeg capture timeout: `15s`.
- CLAHE subprocess timeout: `5s`.
- CLAHE JPEG encode timeout: `5s`.
- RKNN demo subprocess timeout: `30s`.

`SIMULATE_CAMERA` mode returns canned detection results and does not start a real thread, run ffmpeg, or touch RTSP.

Standalone RTSP test scripts also exist:

- `tests/test_continuous_detection.sh`
- `tests/test_continuous_detection_persistent_rtsp.sh`

The one-shot `probe_scale` profile was the best measured path on board, around 2-3 seconds total per frame in the successful test run. The persistent RTSP version was kept as a diagnostic script but was slower because ffmpeg kept decoding in the background.

## YOLOv5 Demo Integration

Rockchip's YOLOv5 example in `rknn_model_zoo` was patched to call AMTECH alarm logic from the detection loop.

The integration patch is stored under:

```text
patches/
```

This keeps changes to the third-party `rknn_model_zoo` clone reproducible if that folder is deleted or recloned.

## Schedule Logic

Implemented in `src/schedule.c`.

It supports:

- An armed time window, such as 23:00 to 06:00.
- Overnight windows that wrap past midnight.
- A 30-second exit delay.
- Test-friendly ticking where each `schedule_tick()` call represents one simulated second.

The runtime uses real system time for the current arm/disarm decision.

## SIM7672 Modem

The modem transport and HAL are implemented in:

- `src/sim_modem.c`
- `src/modem_state.c`
- `src/modem_hal.c`
- `src/alert_dispatch.c`

The modem UART path is configurable with `MODEM_DEVICE`, defaulting to `/dev/ttyS5`.

Current modem architecture:

- The data/PDP internet path was deliberately removed.
- The modem is now registration-only for SMS and voice call reliability.
- The state machine stops at `REGISTERED`.
- Emergency alerts use modem SMS and voice calls, not modem data or WhatsApp.
- SMS receiving is used as a backup remote control channel for `ARM` and `DISARM` commands.

Registration state machine:

```text
POWER_OFF -> BOOTING -> MODEM_READY -> SIM_READY -> REGISTERING -> REGISTERED
```

Failure behavior:

- Per-state retries exist.
- If a state cannot advance after retry limit, it enters `FAILED`.
- `FAILED` is terminal for now; full recovery escalation is deferred.

Proven real hardware basics:

- `AT`
- SIM ready checks with `AT+CPIN?`
- Network registration with `AT+CEREG?`
- Raw voice call and SMS diagnostics were created for board testing.

## SMS And Voice Alert Dispatch

Implemented in `src/alert_dispatch.c` and `src/modem_hal.c`.

SMS:

- Sent once to all configured contacts at the start of an incident.
- Uses text mode: `AT+CMGF=1`.
- Uses interactive `AT+CMGS="<number>"`, waits for `>` prompt, sends message text and Ctrl+Z.

Voice calls:

- Calls contacts in priority order.
- Three contacts are supported through `ALERT_CONTACT_1`, `ALERT_CONTACT_2`, and `ALERT_CONTACT_3`.
- Each contact gets up to 2 attempts.
- Each attempt times out after about 30 seconds in the escalation layer.
- The modem HAL has a 45-second safety max duration.
- Call status is polled with `AT+CLCC`.
- `CLCC` state `0` is treated as connected only if it stays active for at least 3 seconds. This reduces the chance that voicemail/IVR briefly stops escalation incorrectly.

Escalation stops if a call is confirmed answered. If nobody answers after all attempts, escalation ends cleanly.

Alert SMS messages are event-specific:

- Panic: `AMTECH ALERT: Panic button pressed at your shop. Immediate attention needed.`
- Shutter 1: `AMTECH ALERT: Shutter 1 intrusion detected at your shop.`
- Shutter 2: `AMTECH ALERT: Shutter 2 intrusion detected at your shop.`
- Intrusion: `AMTECH ALERT: Person detected inside your shop while armed.`
- Smoke: `AMTECH ALERT: Smoke detected at your shop. Possible fire emergency.`

`SIMULATE_MODEM` mode prints and records simulated SMS/call/hangup behavior for tests.

## SMS Remote Arm/Disarm/Stop

Implemented through `src/modem_hal.c` and `src/runtime_loop.c`.

Behavior:

- The modem is initialized for text-mode SMS receive with `AT+CMGF=1`, SIM storage via `AT+CPMS="SM","SM","SM"`, and stored-message notifications via `AT+CNMI=2,1,0,0,0`.
- The runtime polls unread SMS periodically using `AT+CMGL="REC UNREAD"`.
- Only `ALERT_CONTACT_1`, `ALERT_CONTACT_2`, and `ALERT_CONTACT_3` are authorized senders.
- Valid commands are case-insensitive `ARM`, `DISARM`, and `STOP`.
- `ARM` calls the same `alarm_logic_set_armed(1)` path used elsewhere and replies with `System ARMING...`, or `System already ARMED` if no state change is needed. After camera calibration completes, the hub sends `System ARMED`.
- `DISARM` calls the same `alarm_logic_set_armed(0)` path used elsewhere and replies with `System DISARMED`, or `System already DISARMED` if no state change is needed.
- `STOP` calls `alarm_logic_reset()`, disarms the system, and replies with `Alarm stopped, system DISARMED`; if no alarm is active it replies with `No active alarm`.
- Unknown senders and unrecognized message text are ignored without a reply.
- Every read SMS is deleted with `AT+CMGD=<index>`, including rejected and malformed messages, so SIM storage does not fill.
- SMS command polling is serialized through the modem HAL mutex, so a `STOP` SMS can still be processed during an active alert call.

## Backend

The backend lives in `backend/` and uses Flask.

It provides:

- `GET /health`
- `POST /alert`
- `POST /auth/signup`
- `POST /auth/login`
- `POST /shop`
- `GET /shop/<shop_id>`
- `GET /me/shops`
- `POST /shop/<shop_id>/arm`
- `POST /shop/<shop_id>/disarm`
- `GET /shop/<shop_id>/status`
- `POST /shop/<shop_id>/media/upload-url`
- `GET /shop/<shop_id>/device-config`
- `PUT /shop/<shop_id>/device-config`
- `GET /alerts/<shop_id>`

Security and infrastructure:

- JWT auth for shop/app endpoints.
- Password hashing with bcrypt.
- Basic rate limiting on auth routes.
- Database keepalive thread for Railway/Neon stability.
- R2 presigned upload URL support for future alert images/videos.
- Device config sync endpoint for schedule and emergency contact settings. The app uses owner JWT auth; the physical hub can use the `X-AMTECH-DEVICE-CONFIG-TOKEN` shared-secret header. Live Railway owner-auth behavior is verified. Device-token live verification is pending Railway `DEVICE_CONFIG_SYNC_TOKEN` setup. Device-side polling is simulation-tested only until the board is reachable again.

Database:

- `users`
- `shops`
- `devices`
- `alerts`
- `shop_device_schedules`
- `shop_emergency_contacts`

The backend reads `DATABASE_URL` from the environment. Neon Postgres is intended for Railway. SQLite can still be used for local testing.

The backend no longer sends WhatsApp messages. Its alert role is database logging for the app's alert history feed.

## Tests

Current C test coverage includes:

- `tests/test_alarm_logic.c`
- `tests/test_sensor_input.c`
- `tests/test_schedule.c`
- `tests/test_runtime_config.c`
- `tests/test_sim_modem.c`
- `tests/test_modem_state.c`
- `tests/test_modem_hal.c`
- `tests/test_alert_dispatch.c`
- `tests/test_camera_detection.c`
- `tests/test_camera_queue.c`

Diagnostic/hardware bring-up tests and scripts include:

- `tests/test_raw_at.c`
- `tests/test_raw_cops.c`
- `tests/test_raw_call_sms.c`
- `tests/test_continuous_detection.sh`
- `tests/test_continuous_detection_persistent_rtsp.sh`
- `run_batch_test.sh`

Simulation flags:

- `SIMULATE_GPIO`
- `SIMULATE_MODEM`
- `SIMULATE_CAMERA`
- `SIMULATE_NETWORK`

## What Is Still Not Finished

Still pending:

- Final production service packaging for `runtime_loop`.
- Real end-to-end runtime testing with GPIO sensors, camera RTSP, NPU inference, relays, and modem wired together for a long duration.
- Mobile app.
- App UI for editing contact lists and schedules. The backend/device sync foundation now exists, but the settings screens are not built yet.
- Camera media upload from hub to backend/R2 during alerts.
- Recovery strategy for modem failures beyond the current terminal `FAILED` state.
- Replacing subprocess-based camera detection with a lower-level RKNN/RKMPI integration if later performance or reliability requires it.

## Current Confidence

The hub-side logic is now substantially built and tested under simulation. Several hardware-specific paths have also been validated through isolated real-board tests, especially GPIO behavior, RTSP frame capture, NPU demo inference, and basic SIM7672 AT/SMS/call behavior.

The full production runtime still needs a real long-running field test on the Luckfox board with the final wiring and config file.
