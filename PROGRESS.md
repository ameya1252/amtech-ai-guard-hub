# AMTECH AI Guard Hub Progress

This project is building a small AI-powered security system for Indian shops, designed to run on a Luckfox Pico Ultra board.

## What Is Built

### Core Alarm Logic

The main alarm decision system is implemented and tested.

It supports:

- Armed and disarmed modes.
- Person detection alarm logic.
- A person must be detected for 3 frames in a row before the alarm triggers. This helps reduce false alarms.
- Shutter sensor alarm logic.
- If the shutter sensor is triggered while the system is armed, the alarm triggers immediately.
- If the shutter sensor is triggered while the system is disarmed, it is logged but does not trigger the alarm.

This part has been tested using simulated inputs.

### Arm/Disarm Schedule

The schedule system is implemented and tested.

It supports:

- Setting an armed time window, such as 11:00 PM to 6:00 AM.
- Correct handling of schedules that cross midnight.
- A 30-second exit delay before arming.
- A test-friendly clock where each `schedule_tick()` call represents 1 second, so we can test without waiting in real time.

This part has been tested, including overnight schedule cases and the full 30-second exit delay.

### GPIO Output And Sensor Input

The basic GPIO hardware layer is implemented and tested.

It supports:

- GPIO output for the alarm relay.
- GPIO input for a sensor, such as a shutter tamper sensor.
- A `SIMULATE_GPIO` mode so the logic can be tested on a laptop or in Docker before the physical board arrives.

In simulation mode, GPIO actions print to the console instead of touching real hardware.

### YOLOv5 Person Detection Integration

The Rockchip YOLOv5 demo has been inspected and patched.

We found where detection boxes, class IDs, and confidence scores become available. The demo now calls into AMTECH alarm logic from the detection loop.

The YOLOv5 example compiles successfully for the Luckfox Pico Ultra target inside the Docker build environment.

Full runtime testing of YOLOv5 inference must wait for the physical board because the model needs the real Rockchip NPU device at `/dev/rknpu`.

### Runtime Loop Skeleton

A standalone `runtime_loop.c` file has been added.

It shows how the final Guard Hub program will continuously run:

- Tick the schedule timer.
- Apply the arm/disarm schedule.
- Read the shutter sensor.
- Trigger the alarm immediately on shutter tamper when armed.
- Leave a clear placeholder for camera capture and YOLO inference.

For testing without hardware, it runs a fixed number of iterations in `SIMULATE_GPIO` mode.

## What Is Not Started Yet

These parts are not built yet:

- WhatsApp or notification integration.
- Cloud connectivity.
- Mobile app.
- Connecting the continuous runtime loop to real camera capture.
- Running the full AI detection pipeline on live camera frames.

Right now, real image processing only exists in Rockchip's single-image YOLOv5 demo. The continuous `runtime_loop.c` is ready for the camera pipeline to be plugged in later.

## Current Status

The core local security logic is in good shape:

- Alarm decisions are implemented.
- Scheduling is implemented.
- GPIO input and output are implemented.
- Simulation tests pass without physical hardware.
- The Rockchip YOLOv5 demo builds with AMTECH integration.

The next major milestone is testing on the actual Luckfox Pico Ultra board with real GPIO pins, the camera, and the NPU.
