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

## Device Config Sync Backend

The backend exposes:

```text
GET /shop/<shop_id>/device-config
PUT /shop/<shop_id>/device-config
```

`GET` returns the hub's baseline schedule and three emergency contacts. The mobile app can call it with the owner's normal JWT. The physical hub can call it with:

```text
X-AMTECH-DEVICE-CONFIG-TOKEN: <shared token>
```

Railway must set the matching environment variable:

```text
DEVICE_CONFIG_SYNC_TOKEN=<same shared token>
```

`PUT` is owner-authenticated and updates the schedule/contact rows. This is the endpoint the future Settings UI will use.

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
CAMERA2_ENABLED=0
CAMERA2_RTSP_URL=
BACKEND_BASE_URL=https://amtech-ai-guard-hub-production.up.railway.app
DEVICE_CONFIG_TOKEN=
```

Notes:

- `CAMERA_ENABLED=1` plus a non-empty `CAMERA_RTSP_URL` enables the front camera.
- `CAMERA2_ENABLED=1` plus a non-empty `CAMERA2_RTSP_URL` enables the parking camera.
- Leave each camera's enabled key at `0` to keep that camera fully disabled.
- `SHUTTER_COUNT=1` means Shutter-2 GPIO pins are not exported or watched.
- `SMOKE_ENABLED=0` is the default so unwired smoke pins cannot false-trigger.
- `MODEM_DEVICE` defaults to `/dev/ttyS5` but is configurable until the final PCB UART mapping is locked.
- `SCHEDULE_ARM` and `SCHEDULE_DISARM` define the baseline automatic arm/disarm window. Overnight windows such as `23:00` to `06:00` are supported.
- `ALERT_CONTACT_1/2/3` are used for call/SMS alert escalation and are also the only numbers allowed to control the system by SMS.
- `BACKEND_BASE_URL` defaults to the live Railway backend.
- `DEVICE_CONFIG_TOKEN` enables backend device-config polling. Leave it empty to disable sync cleanly.

Future app/backend device configuration should map UI controls to these config keys:

```text
SHUTTER_COUNT=1|2
PANIC_ENABLED=0|1
SMOKE_ENABLED=0|1
CAMERA_ENABLED=0|1
CAMERA_RTSP_URL=rtsp://...
CAMERA2_ENABLED=0|1
CAMERA2_RTSP_URL=rtsp://...
SCHEDULE_ARM=HH:MM
SCHEDULE_DISARM=HH:MM
ALERT_CONTACT_1=+91...
ALERT_CONTACT_2=+91...
ALERT_CONTACT_3=+91...
```

Current firmware support status:

- `SHUTTER_COUNT`, `PANIC_ENABLED`, `SMOKE_ENABLED`, `CAMERA_ENABLED`, `CAMERA_RTSP_URL`, `CAMERA2_ENABLED`, `CAMERA2_RTSP_URL`, and `ALERT_CONTACT_1/2/3` are implemented today.
- Backend-to-device config sync is implemented for `SCHEDULE_ARM`, `SCHEDULE_DISARM`, and `ALERT_CONTACT_1/2/3`. The hub polls `GET /shop/<shop_id>/device-config` every 5 minutes when `DEVICE_CONFIG_TOKEN` is configured, then atomically updates only those synced keys in `/root/amtech_config.txt`.
- Config sync deliberately preserves local install-specific keys such as `SHUTTER_COUNT`, `PANIC_ENABLED`, `SMOKE_ENABLED`, `MODEM_DEVICE`, and camera RTSP settings.
- The app/backend should treat `CAMERA_ENABLED` and `CAMERA2_ENABLED` as the actual toggles. A URL alone is not enough to start a camera.
- SMS remote control is implemented today: an authorized contact can send `ARM`, `DISARM`, `STOP`, `STATUS`, or `HELP` to the hub SIM number. `STOP` clears an active alarm and disarms the system. Unknown senders and unknown commands are ignored without a reply.
- SMS `ARM` and `DISARM` are manual overrides. Once an owner sends `ARM`, the real-time schedule is not allowed to immediately disarm the system just because the current time is outside the scheduled armed window. Once an owner sends `DISARM` or `STOP`, the schedule is not allowed to immediately re-arm it. The manual override clears at the next natural schedule boundary, returning the hub to normal schedule-driven behavior.
- SMS `ARM` sends an immediate `System ARMING...` reply. Shutter, panic, and smoke protection are active immediately after `ARM`; only camera person detection waits for the 60-second static-scene calibration.
- When camera static-scene calibration completes after an arm cycle, the hub sends `System ARMED` to all three configured alert contacts. If the system is disarmed before calibration finishes, this second message is not sent.

## Runtime Loop Build

The runtime now links pthreads because camera detection runs in a background thread while GPIO interrupt handling stays in the main thread.

Cross-compile inside the Docker container:

```sh
docker exec luckfox-dev bash -c "cd /workspace && mkdir -p build/luckfox && /workspace/luckfox-pico/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-gcc -Wall -Wextra -I /workspace/src /workspace/src/runtime_loop.c /workspace/src/camera_detection.c /workspace/src/config.c /workspace/src/config_sync.c /workspace/src/schedule.c /workspace/src/alarm_logic.c /workspace/src/alert_dispatch.c /workspace/src/gpio_control.c /workspace/src/sensor_input.c /workspace/src/modem_hal.c /workspace/src/modem_state.c /workspace/src/sim_modem.c -pthread -o /workspace/build/luckfox/runtime_loop"
```

For local simulation tests, use the relevant simulation flags:

```text
-DSIMULATE_GPIO -DSIMULATE_MODEM -DSIMULATE_CAMERA
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

## Camera Detection

Runtime camera detection is enabled per camera only when both its enabled key and RTSP URL are set.

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
