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
MODEM_DEVICE=/dev/ttyS5
ALERT_CONTACT_1=+918550991121
ALERT_CONTACT_2=+919922434811
ALERT_CONTACT_3=+919922435710
CAMERA_ENABLED=1
CAMERA_RTSP_URL=rtsp://user:pass@camera-ip:554/stream1
CAMERA2_ENABLED=0
CAMERA2_RTSP_URL=
```

Notes:

- `CAMERA_ENABLED=1` plus a non-empty `CAMERA_RTSP_URL` enables the front camera.
- `CAMERA2_ENABLED=1` plus a non-empty `CAMERA2_RTSP_URL` enables the parking camera.
- Leave each camera's enabled key at `0` to keep that camera fully disabled.
- `SHUTTER_COUNT=1` means Shutter-2 GPIO pins are not exported or watched.
- `SMOKE_ENABLED=0` is the default so unwired smoke pins cannot false-trigger.
- `MODEM_DEVICE` defaults to `/dev/ttyS5` but is configurable until the final PCB UART mapping is locked.
- `ALERT_CONTACT_1/2/3` are used for call/SMS alert escalation and are also the only numbers allowed to control the system by SMS.

Future app/backend device configuration should map UI controls to these config keys:

```text
SHUTTER_COUNT=1|2
PANIC_ENABLED=0|1
SMOKE_ENABLED=0|1
CAMERA_ENABLED=0|1
CAMERA_RTSP_URL=rtsp://...
CAMERA2_ENABLED=0|1
CAMERA2_RTSP_URL=rtsp://...
ALERT_CONTACT_1=+91...
ALERT_CONTACT_2=+91...
ALERT_CONTACT_3=+91...
```

Current firmware support status:

- `SHUTTER_COUNT`, `PANIC_ENABLED`, `SMOKE_ENABLED`, `CAMERA_ENABLED`, `CAMERA_RTSP_URL`, `CAMERA2_ENABLED`, `CAMERA2_RTSP_URL`, and `ALERT_CONTACT_1/2/3` are implemented today.
- The app/backend should treat `CAMERA_ENABLED` and `CAMERA2_ENABLED` as the actual toggles. A URL alone is not enough to start a camera.
- SMS remote control is implemented today: an authorized contact can send `ARM` or `DISARM` to the hub SIM number. Unknown senders and unknown commands are ignored without a reply.

## Runtime Loop Build

The runtime now links pthreads because camera detection runs in a background thread while GPIO interrupt handling stays in the main thread.

Cross-compile inside the Docker container:

```sh
docker exec luckfox-dev bash -c "cd /workspace && mkdir -p build/luckfox && /workspace/luckfox-pico/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-gcc -Wall -Wextra -I /workspace/src /workspace/src/runtime_loop.c /workspace/src/camera_detection.c /workspace/src/config.c /workspace/src/schedule.c /workspace/src/alarm_logic.c /workspace/src/alert_dispatch.c /workspace/src/gpio_control.c /workspace/src/sensor_input.c /workspace/src/modem_hal.c /workspace/src/modem_state.c /workspace/src/sim_modem.c -pthread -o /workspace/build/luckfox/runtime_loop"
```

For local simulation tests, use the relevant simulation flags:

```text
-DSIMULATE_GPIO -DSIMULATE_MODEM -DSIMULATE_CAMERA
```

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

Person detection now uses single-frame confirmation for faster response. Before finalizing that change, the two no-person ground-truth images (`data_21.jpg` and `data_45.jpg`) were rerun on the real board through the production 480 letterbox + CLAHE path at threshold `>0.25`; both returned `NO_PERSON_DETECTED`, so false positives remained `0/2` on the empty-scene subset.

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
