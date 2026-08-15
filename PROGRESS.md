# AMTECH AI Guard Hub Progress

This project is building an AI-powered security system for Indian shops, designed to run on a Luckfox Pico Ultra board with a Rockchip RV1106G3 processor.

This document summarizes what exists in the repo right now and how the pieces fit together.

## Current System Shape

The system now has four main parts:

- Hub-side C code in `src/` for GPIO, sensors, alarm decisions, scheduling, camera detection, modem call/SMS alerts, and cloud notification calls.
- Test programs in `tests/` for the C modules and hardware bring-up diagnostics.
- A Python backend in `backend/` for alert storage, user/shop APIs, WhatsApp Cloud API integration, and media upload URLs.
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
MODEM_DEVICE=/dev/ttyS5
ALERT_CONTACT_1=+918550991121
ALERT_CONTACT_2=+919922434811
ALERT_CONTACT_3=+919922435710
CAMERA_RTSP_URL=rtsp://user:pass@camera-ip:554/stream1
```

Important defaults:

- `SHUTTER_COUNT=1`
- `PANIC_ENABLED=1`
- `SMOKE_ENABLED=0`
- `MODEM_DEVICE=/dev/ttyS5`
- `CAMERA_RTSP_URL` is empty, so camera detection is disabled unless configured.

The shutter and smoke config defaults are deliberately conservative so unwired/floating pins are not watched accidentally.

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
- Backend notification trigger.
- SIM modem call/SMS dispatch.

Person detection:

- Person class is class ID `0` or class name `"person"`.
- Confidence must be greater than `0.6`.
- A person must appear for `2` consecutive camera frames before the alarm triggers.
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
- The siren/strobe reactivation is independent from the notification cooldown.

## Camera Detection

Camera capture/detection has been integrated locally into the runtime through:

- `src/camera_detection.c`
- `src/camera_detection.h`

The implementation uses the subprocess approach proven by the standalone RTSP tests:

- Capture one frame with `ffmpeg`.
- Use RTSP over TCP.
- Use reduced probe settings: `-analyzeduration 1000000 -probesize 32768`.
- Scale before writing the JPEG: `-vf scale=640:640`.
- Save to `/tmp/amtech_live_frame.jpg`.
- Run Rockchip's `rknn_yolov5_demo` against that frame.
- Parse `person @ ... confidence` lines from the demo output.

Real paths used by the camera module:

```text
/root/rknn_yolov5_demo_export/rknn_yolov5_demo
/root/rknn_yolov5_demo_export/model/yolov5.rknn
/tmp/amtech_live_frame.jpg
/tmp/amtech_camera_detection_output.txt
```

The runtime starts camera detection in a separate pthread when `CAMERA_RTSP_URL` is configured. The camera thread never calls `alarm_logic_*` directly. It only publishes a detection result into a mutex-protected slot. The main runtime thread consumes that result and calls `alarm_logic_handle_detection()` and `alarm_logic_end_frame()`, keeping alarm state mutations single-threaded.

Timeouts:

- ffmpeg capture timeout: `15s`.
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

## Notifications To Backend

Implemented in `src/notify_client.c`.

The hub sends alerts to the backend endpoint:

```text
https://amtech-ai-guard-hub-production.up.railway.app/alert
```

It can be overridden with:

```sh
AMTECH_BACKEND_ALERT_URL=http://127.0.0.1:8000/alert
```

Event types currently used:

- `intrusion`
- `shutter`
- `shutter-1`
- `shutter-2`
- `panic`
- `smoke`
- `test`

`SIMULATE_NETWORK` mode prints the request instead of using libcurl.

The live Railway backend has been tested with real HTTPS requests from the C client. WhatsApp sending is still simulated on the backend unless Meta credentials and template settings are configured.

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
- Cloud/internet alerts use the normal network path through `notify_client.c`, not modem data.

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
- `GET /alerts/<shop_id>`

Security and infrastructure:

- JWT auth for shop/app endpoints.
- Password hashing with bcrypt.
- Basic rate limiting on auth routes.
- Database keepalive thread for Railway/Neon stability.
- R2 presigned upload URL support for future alert images/videos.

Database:

- `users`
- `shops`
- `devices`
- `alerts`

The backend reads `DATABASE_URL` from the environment. Neon Postgres is intended for Railway. SQLite can still be used for local testing.

WhatsApp:

- `SIMULATE_WHATSAPP` defaults on.
- Real Meta WhatsApp Cloud API code exists but needs Meta credentials, phone number ID, recipient, and approved utility template before real messages are sent.

## Tests

Current C test coverage includes:

- `tests/test_alarm_logic.c`
- `tests/test_sensor_input.c`
- `tests/test_schedule.c`
- `tests/test_notify_client.c`
- `tests/test_runtime_config.c`
- `tests/test_sim_modem.c`
- `tests/test_modem_state.c`
- `tests/test_modem_hal.c`
- `tests/test_alert_dispatch.c`
- `tests/test_camera_detection.c`

Diagnostic/hardware bring-up tests and scripts include:

- `tests/test_raw_at.c`
- `tests/test_raw_cops.c`
- `tests/test_raw_call_sms.c`
- `tests/test_continuous_detection.sh`
- `tests/test_continuous_detection_persistent_rtsp.sh`
- `run_batch_test.sh`

Simulation flags:

- `SIMULATE_GPIO`
- `SIMULATE_NETWORK`
- `SIMULATE_MODEM`
- `SIMULATE_CAMERA`

## What Is Still Not Finished

Still pending:

- Final production service packaging for `runtime_loop`.
- Real end-to-end runtime testing with GPIO sensors, camera RTSP, NPU inference, relays, and modem wired together for a long duration.
- Real Meta WhatsApp production sending.
- Mobile app.
- App-managed contact lists and device/shop configuration sync.
- Camera media upload from hub to backend/R2 during alerts.
- Recovery strategy for modem failures beyond the current terminal `FAILED` state.
- Replacing subprocess-based camera detection with a lower-level RKNN/RKMPI integration if later performance or reliability requires it.

## Current Confidence

The hub-side logic is now substantially built and tested under simulation. Several hardware-specific paths have also been validated through isolated real-board tests, especially GPIO behavior, RTSP frame capture, NPU demo inference, and basic SIM7672 AT/SMS/call behavior.

The full production runtime still needs a real long-running field test on the Luckfox board with the final wiring and config file.
