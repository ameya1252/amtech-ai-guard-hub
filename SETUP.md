# AMTECH AI Guard Hub Setup Notes

## Docker Build Container Dependencies

The Luckfox/Rockchip build should run inside the `luckfox-dev` Docker container, not directly on macOS.

No hub-side libcurl dependency is needed. Emergency delivery is handled by the SIM7672 modem using call + SMS, while the backend stores alert history for the app.

## Alert History Backend

The backend lives in `backend/`.

Run locally:

```sh
cd backend
PORT=8000 python3 app.py
```

Test alerts:

```sh
python3 test_alerts.py
```

Railway deployment is prepared with:

```text
backend/Procfile
backend/requirements.txt
```

The backend binds to `0.0.0.0` and reads Railway's dynamic port from `PORT`.

Live simulated Railway backend:

```text
https://amtech-ai-guard-hub-production.up.railway.app
```

Health check:

```sh
curl https://amtech-ai-guard-hub-production.up.railway.app/health
```

The backend no longer sends WhatsApp messages. It stores alerts in the database for the app's alert history feed.

## Backend Password Reset SMS

Forgot-password OTPs are sent by the backend, not by the physical hub's SIM7672 modem. This is deliberate: password reset must work even if a customer's hub is offline.

Safe default for local/dev/Railway until MSG91 is configured:

```text
SIMULATE_SMS=1
```

With simulation enabled, `POST /auth/forgot-password` logs the OTP that would be sent and stores the hashed OTP in the database for verification.

Production MSG91 variables:

```text
SIMULATE_SMS=0
SMS_PROVIDER=msg91
MSG91_AUTH_KEY=...
MSG91_SENDER_ID=...
MSG91_PASSWORD_RESET_MESSAGE=Your AMTECH password reset OTP is ##OTP##. It expires in 10 minutes.
PASSWORD_RESET_OTP_SECRET=...
PASSWORD_RESET_OTP_EXPIRY_MINUTES=10
```

Before setting `SIMULATE_SMS=0`, complete India DLT registration and get the OTP sender/template approved inside MSG91. The backend expects app users' phone numbers in `+91XXXXXXXXXX` format.

## Backend Database

The backend reads its database connection from:

```sh
DATABASE_URL
```

On Railway, this should point to the Neon Postgres connection string.

The backend creates these tables automatically at startup:

- `users`
- `shops`
- `devices`
- `alerts`
- `push_tokens`
- `camera_inventory`
- `cameras`
- `shop_device_schedules`
- `shop_emergency_contacts`
- `password_reset_otps`

## Device Config Sync Backend

The backend exposes:

```text
GET /shop/<shop_id>/device-config
PUT /shop/<shop_id>/device-config
GET /shop/<shop_id>/pending-command
POST /shop/<shop_id>/pending-command/ack
```

`GET` returns the hub's baseline schedule and three emergency contacts. The mobile app can call it with the owner's normal JWT. The physical hub can call it with:

```text
X-AMTECH-DEVICE-CONFIG-TOKEN: <shared token>
```

Railway must set the matching environment variable:

```text
DEVICE_CONFIG_SYNC_TOKEN=<same shared token>
```

`PUT` is owner-authenticated and updates the schedule/contact rows. This is the endpoint the Settings UI uses for schedule and emergency contact edits.

The pending-command endpoints are separate from the 5-minute schedule/contact sync. They are used for fast app Arm/Disarm control: app Arm/Disarm sets a pending command in the backend, and the hub polls that lightweight command path every few seconds using the same device token. Applied commands are acknowledged so they are not repeated.

For local testing without Neon:

```sh
DATABASE_URL=sqlite:////tmp/amtech_alerts.db PORT=8000 python3 backend/app.py
```

## Hub Config File

The hub runtime reads:

```text
/root/amtech_config.txt
```

Override for tests:

```sh
export AMTECH_CONFIG_PATH=/tmp/amtech_config.txt
```

Example config:

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
CAMERA_ENABLED=1
CAMERA_RTSP_URL=rtsp://user:pass@camera-ip:554/stream1
CAMERA_MAC=
CAMERA2_ENABLED=0
CAMERA2_RTSP_URL=
CAMERA2_MAC=
BACKEND_BASE_URL=https://amtech-ai-guard-hub-production.up.railway.app
DEVICE_CONFIG_TOKEN=
WATCHDOG_ENABLED=0
```

Notes:

- `CAMERA_ENABLED=1` plus a non-empty `CAMERA_RTSP_URL` enables the front camera.
- `CAMERA2_ENABLED=1` plus a non-empty `CAMERA2_RTSP_URL` enables the parking camera.
- Leave each camera's enabled key at `0` to keep that camera fully disabled.
- `CAMERA_MAC` and `CAMERA2_MAC` are optional stable camera identities. The runtime still connects to RTSP by IP, but when a MAC is configured it checks `/proc/net/arp` before each capture and rewrites the RTSP URL host to the current IP if that MAC is found. This is a software fallback for router/DHCP IP changes on the same LAN/subnet. If the MAC is not found in ARP, the configured RTSP URL is used unchanged.
- Production installs should still use router DHCP reservation/static lease for each camera MAC whenever possible. That is the primary fix for changing camera IPs; hub-side MAC lookup is the backup.
- `SHUTTER_COUNT=1` means Shutter-2 GPIO pins are not exported or watched.
- `SMOKE_ENABLED=0` is the default so unwired smoke pins cannot false-trigger.
- `MODEM_DEVICE` defaults to `/dev/ttyS5` but is configurable until the final PCB UART mapping is locked.
- `SCHEDULE_ARM` and `SCHEDULE_DISARM` define the baseline automatic arm/disarm window. Overnight windows such as `23:00` to `06:00` are supported.
- `ALERT_CONTACT_1/2/3` are used for call/SMS alert escalation and are also the only numbers allowed to control the system by SMS.
- `BACKEND_BASE_URL` defaults to the live Railway backend.
- `DEVICE_CONFIG_TOKEN` enables backend device-config polling. Leave it empty to disable sync cleanly.
- `WATCHDOG_ENABLED=0` is the safe default. Set it to `1` only after confirming the board exposes `/dev/watchdog0` or `/dev/watchdog` and the runtime logs the effective watchdog timeout.

Future app/backend device configuration should map UI controls to these config keys:

```text
SHUTTER_COUNT=1|2
PANIC_ENABLED=0|1
SMOKE_ENABLED=0|1
CAMERA_ENABLED=0|1
CAMERA_RTSP_URL=rtsp://...
CAMERA_MAC=aa:bb:cc:dd:ee:ff
CAMERA2_ENABLED=0|1
CAMERA2_RTSP_URL=rtsp://...
CAMERA2_MAC=aa:bb:cc:dd:ee:ff
SCHEDULE_ARM=HH:MM
SCHEDULE_DISARM=HH:MM
ALERT_CONTACT_1=+91...
ALERT_CONTACT_2=+91...
ALERT_CONTACT_3=+91...
WATCHDOG_ENABLED=0|1
```

Current firmware support status:

- `SHUTTER_COUNT`, `PANIC_ENABLED`, `SMOKE_ENABLED`, `CAMERA_ENABLED`, `CAMERA_RTSP_URL`, `CAMERA_MAC`, `CAMERA2_ENABLED`, `CAMERA2_RTSP_URL`, `CAMERA2_MAC`, and `ALERT_CONTACT_1/2/3` are implemented today.
- Backend-to-device config sync is implemented for `SCHEDULE_ARM`, `SCHEDULE_DISARM`, and `ALERT_CONTACT_1/2/3`. The hub polls `GET /shop/<shop_id>/device-config` every 5 minutes when `DEVICE_CONFIG_TOKEN` is configured, then atomically updates only those synced keys in `/root/amtech_config.txt`.
- Config sync deliberately preserves local install-specific keys such as `SHUTTER_COUNT`, `PANIC_ENABLED`, `SMOKE_ENABLED`, `MODEM_DEVICE`, camera RTSP/MAC settings, and `WATCHDOG_ENABLED`.
- The app/backend should treat `CAMERA_ENABLED` and `CAMERA2_ENABLED` as the actual toggles. A URL alone is not enough to start a camera.
- SMS remote control is implemented today: an authorized contact can send `ARM`, `ARM FORCE`, `DISARM`, `STOP`, `STATUS`, or `HELP` to the hub SIM number. `STOP` clears an active alarm and disarms the system. Unknown senders and unknown commands are ignored without a reply.
- SMS `ARM` and `DISARM` are manual overrides. Once an owner sends `ARM`, the real-time schedule is not allowed to immediately disarm the system just because the current time is outside the scheduled armed window. Once an owner sends `DISARM` or `STOP`, the schedule is not allowed to immediately re-arm it. The manual override clears at the next natural schedule boundary, returning the hub to normal schedule-driven behavior.
- Normal SMS/schedule/reboot arming now runs a wired-sensor preflight before entering ARMED. Configured shutters must read closed, panic must be idle, and smoke must be normal. If a configured wired sensor is open, tampered, faulted, active, or unreadable, SMS/schedule/restore ARM is blocked and the hub stays DISARMED. SMS `ARM` replies with `AMTECH NETTRA: ARM BLOCKED` and the failing sensor state. App ARM will get the same visible block behavior after the backend pending-command result contract is extended to return failure reasons to the app.
- `ARM FORCE` is an explicit authorized SMS override for installation/testing or an owner-approved emergency bypass. It skips the wired-sensor preflight and arms anyway; use it only when the owner intentionally accepts the listed sensor fault.
- SMS `ARM` sends an immediate `AMTECH NETTRA: ARMING` reply. Shutter, panic, and smoke protection are active immediately after `ARM`; only camera person detection waits for the 60-second static-scene calibration.
- When camera static-scene calibration completes after an arm cycle, the hub sends `AMTECH NETTRA: ARMED` / `All monitoring active.` If the system is disarmed before calibration finishes, this second message is not sent.
- When an alarm event fires, the runtime temporarily disarms while the siren/call/SMS sequence is active. When the siren naturally auto-stops, the runtime automatically re-arms through the normal arm path, which restarts camera grace/static-scene calibration. Explicit owner `STOP`, `DISARM`, or app disarm cancels this pending automatic re-arm.
- Alert SMS and backend push notification text for camera/person intrusion also mention that the system is temporarily disarmed and will re-arm after the siren stops.
- SMS TX failure is non-fatal. The modem HAL confirms `+CMGS`/`OK`, retries once on send failure, sets `SMS_TX_FAIL` if the retry also fails, and clears that fault automatically after a later successful SMS. `STATUS` shows `SMS TX FAULT` while the fault is active.
- Voice call escalation is independent of SMS. Even if every SMS send fails, the call sequence must continue through the configured attempts: Contact 1 twice, then Contact 2 twice, then Contact 3 twice.
- SIM7672 reset control is implemented on GPIO57. The line idles LOW/normal; hard reset pulses HIGH for 2.5 seconds, releases LOW, and waits for repeated `AT`/`OK`. Repeated high-level modem failures can trigger a bounded reset, but reset is rate-limited and deferred while a voice call is active.
- Internal hardware watchdog support is implemented with the Linux watchdog API. When `WATCHDOG_ENABLED=1`, the runtime opens `/dev/watchdog0` or `/dev/watchdog`, requests a 60-second timeout, feeds it every 10 seconds, and deliberately stops feeding if the main runtime heartbeat is stale for 15 seconds. Keep it disabled until the real board has been checked for watchdog device availability and the logged effective timeout.

## Runtime Loop Build

The runtime now links pthreads because camera detection runs in a background thread while GPIO interrupt handling stays in the main thread.

Cross-compile inside the Docker container:

```sh
docker exec luckfox-dev bash -c "cd /workspace && mkdir -p build/luckfox && /workspace/luckfox-pico/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-gcc -Wall -Wextra -I /workspace/src /workspace/src/runtime_loop.c /workspace/src/camera_detection.c /workspace/src/config.c /workspace/src/config_sync.c /workspace/src/schedule.c /workspace/src/alarm_logic.c /workspace/src/alert_dispatch.c /workspace/src/gpio_control.c /workspace/src/sensor_input.c /workspace/src/modem_hal.c /workspace/src/modem_state.c /workspace/src/sim_modem.c /workspace/src/device_command_sync.c /workspace/src/watchdog_manager.c -pthread -o /workspace/build/luckfox/runtime_loop"
```

For local simulation tests, use the relevant simulation flags:

```text
-DSIMULATE_GPIO -DSIMULATE_MODEM -DSIMULATE_CAMERA -DSIMULATE_WATCHDOG
```

## Runtime Autostart On Board Boot

The Luckfox Buildroot image uses BusyBox init. `/etc/inittab` runs `/etc/init.d/rcS`, and `rcS` starts every `/etc/init.d/S??*` script in numeric order.

Install the AMTECH runtime startup script:

```sh
scp scripts/S95runtime_loop root@<board-ip>:/etc/init.d/S95runtime_loop
ssh root@<board-ip> "chmod +x /etc/init.d/S95runtime_loop"
```

On the deployed board, the runtime binary is expected at:

```text
/root/runtime_loop
```

The startup script logs to:

```text
/root/runtime_loop.log
```

This board's `/var/log` is a symlink to `/tmp`, so `/var/log` is not persistent across reboot. `/root/runtime_loop.log` is used instead.

Production autostart does not use `--force-armed`. The system should arm through schedule or authorized SMS commands, not the testing-only force-armed override.

Useful commands on the board:

```sh
/etc/init.d/S95runtime_loop start
/etc/init.d/S95runtime_loop stop
/etc/init.d/S95runtime_loop restart
/etc/init.d/S95runtime_loop status
tail -200 /root/runtime_loop.log
```

The script includes a small restart loop: if `runtime_loop` crashes, it logs the exit and restarts after 5 seconds. Running `stop` creates a stop marker before killing the process, so intentional stops do not immediately restart.

At boot, the script waits up to 90 seconds for the configured `MODEM_DEVICE` path, defaulting to `/dev/ttyS5`, before starting `runtime_loop`. On the tested board, `/dev/ttyS5` can appear later than the network/SSH services. If the modem node still does not exist after that wait, it logs a warning and starts anyway so GPIO alarm protection can still run.

## Factory Test Harness

New hubs can run the board-side factory/field acceptance harness:

```text
scripts/factory_test_nettra.sh
```

Deploy it to the board:

```sh
scp scripts/factory_test_nettra.sh root@<board-ip>:/root/factory_test_nettra.sh
ssh root@<board-ip> "chmod +x /root/factory_test_nettra.sh"
```

Run the safe default test:

```sh
DEVICE_SERIAL=AMT-0000 /root/factory_test_nettra.sh
```

The safe default checks config, runtime binary, init script, GPIO availability, backend health, runtime process status, watchdog device availability, modem device presence, camera configuration/dependencies, and recent runtime logs. It does not toggle siren/strobe relays, does not send SMS, does not place calls, and does not edit `/root/amtech_config.txt`.

Reports are written to:

```text
/root/factory_reports/
```

Optional deeper checks:

```sh
FACTORY_TEST_CAMERA=1 DEVICE_SERIAL=AMT-0000 /root/factory_test_nettra.sh
FACTORY_TEST_MODEM_AT=1 DEVICE_SERIAL=AMT-0000 /root/factory_test_nettra.sh
FACTORY_TEST_RELAYS=1 DEVICE_SERIAL=AMT-0000 /root/factory_test_nettra.sh
```

Notes:

- `FACTORY_TEST_CAMERA=1` captures one RTSP frame per enabled camera using the production 480x480 letterbox profile and runs the RKNN YOLOv5 demo. It is a production-pipeline sanity check, not a full accuracy test.
- `FACTORY_TEST_MODEM_AT=1` runs `/root/test_raw_at` if present. It checks modem AT responsiveness without sending SMS or placing calls.
- `FACTORY_TEST_RELAYS=1` toggles GPIO49 and GPIO48 active-LOW ON then OFF. Use it only when siren/strobe activation is safe and expected.

## Camera Detection

Runtime camera detection is enabled per camera only when both its enabled key and RTSP URL are set.

Camera identity is now split into stable identity and current connection detail:

- `CAMERA_MAC` / `CAMERA2_MAC`: stable camera identity, useful when DHCP/router changes move a camera to a new IP.
- `CAMERA_RTSP_URL` / `CAMERA2_RTSP_URL`: cached/current RTSP connection string. This is still required because RTSP connects by IP/hostname, not directly by MAC.

If a camera MAC is configured, the runtime searches `/proc/net/arp` for that MAC before each capture. When found, it replaces only the host/IP part of the RTSP URL and keeps the same username/password/port/path. This works only when the camera is on the same LAN/subnet as the hub; MAC discovery generally does not work across routers/VLANs.

### Camera DHCP Reservation

For production installs, reserve each camera's IP in the shop router using the camera MAC address:

1. Connect to the same LAN as the cameras and open the router admin page, commonly `192.168.0.1` or `192.168.1.1`.
2. Find the router section named **LAN**, **DHCP**, **Address Reservation**, **Static Lease**, or similar.
3. Add each camera MAC with the intended fixed IP, for example:

```text
CAM-0001 / Cam1 -> aa:bb:cc:dd:ee:01 -> 192.168.0.4
CAM-0002 / Cam2 -> aa:bb:cc:dd:ee:02 -> 192.168.0.7
```

4. Save/apply the router settings, then reboot or reconnect the cameras so they request DHCP again.
5. Confirm the cameras keep the reserved IPs after reconnect/reboot, then set the hub RTSP URLs to those IPs.

The mobile app can guide the installer through these steps, but it generally cannot create the DHCP reservation automatically because consumer routers do not expose a standard API for third-party apps. Automatic setup would only be possible for specific router brands/models with supported management APIs.

The current implementation uses the proven subprocess pipeline:

```text
ffmpeg RTSP capture -> RGB PPM -> CLAHE -> enhanced PPM -> JPEG -> rknn_yolov5_demo -> parse person detections
```

Front and parking cameras run in separate capture threads. Each thread uses its own `/tmp` frame/output paths, so the two cameras cannot overwrite each other's intermediate files. The RKNN demo/NPU step is serialized with a shared pthread mutex because the RV1106 has one shared NPU.

The ffmpeg profile is:

```text
-rtsp_transport tcp -analyzeduration 1000000 -probesize 32768 -vf "scale=480:480:force_original_aspect_ratio=decrease,pad=480:480:(ow-iw)/2:(oh-ih)/2"
```

The CLAHE utility is built from:

```text
tools/amtech_clahe_ppm.c
third_party/graphics_gems/clahe.c
third_party/stb/stb_image_write.h
```

Cross-compile it inside Docker:

```sh
docker exec luckfox-dev bash -c "cd /workspace && mkdir -p build/luckfox && /workspace/luckfox-pico/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-gcc -Wall -Wextra -DBYTE_IMAGE /workspace/tools/amtech_clahe_ppm.c /workspace/third_party/graphics_gems/clahe.c -o /workspace/build/luckfox/amtech_clahe_ppm"
```

Deploy it to the board at:

```text
/root/amtech_clahe_ppm
```

Expected board paths:

```text
/root/rknn_yolov5_demo_export/rknn_yolov5_demo
/root/rknn_yolov5_demo_export/model/yolov5.rknn
/root/amtech_clahe_ppm
```

The camera worker has process timeouts:

- ffmpeg capture: 15 seconds
- CLAHE: 5 seconds
- JPEG encode: 5 seconds
- RKNN demo subprocess: 30 seconds

GPIO interrupt handling stays in the main thread and should continue even if camera capture is slow or fails.

Validation on the 54-image AMTECH ground-truth set at threshold `>0.25`:

```text
Baseline 480 letterbox: TP=27 FP=0 TN=2 FN=25, recall=51.9%, avg total=848ms
480 letterbox + CLAHE:  TP=32 FP=0 TN=2 FN=20, recall=61.5%, avg total=1282ms
```

CLAHE is enabled by default because it gained five net true positives with no new false positives. A direct-JPEG optimization was tested, but it changed detector behavior and did not keep the recall gain, so production keeps the PPM + ffmpeg JPEG encode path.

Person detection now uses 2-frame confirmation per camera source. A 1-frame confirmation test was tried for faster response, but real parking-camera testing produced low-confidence phantom person detections immediately after arming. The production path therefore uses 2 qualifying frames plus a 10-second camera-only grace period after arming. Shutter, panic, and smoke triggers remain immediate and are not delayed by this camera grace period.

Production model decision:

- Use YOLOv5s with `model/yolov5.rknn`.
- YOLO11n was tested on real RV1106 hardware and failed to initialize with an unsupported `MatMul` op, so it is not compatible with the current board/runtime.
- YOLOv5m was compatible, but slower and less accurate on the AMTECH dataset: `57.7%` recall and `1492ms` average dataset-frame time with CLAHE, versus YOLOv5s at `61.5%` recall and `1282ms`.
- Keep YOLOv5s + 480x480 letterbox + CLAHE as the production detector unless a future model beats these real-board numbers.

Testing-only force armed mode:

```sh
./runtime_loop --force-armed
```

or:

```sh
AMTECH_FORCE_ARMED=1 ./runtime_loop
```

This bypasses the schedule and is not for production use.
