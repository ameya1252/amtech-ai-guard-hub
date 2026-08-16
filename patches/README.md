# AMTECH rknn_model_zoo Patches

These patches modify Rockchip's `rknn_model_zoo` YOLOv5 C++ example to wire in AMTECH's alarm logic, GPIO control, config, alert dispatch, and modem support files.

Apply after cloning `rknn_model_zoo` fresh:

```sh
cd rknn_model_zoo
git apply ../patches/rknn_model_zoo_yolov5_alarm_integration.patch
```

The patch expects this workspace layout:

```text
luckfox-project/
├── src/
│   ├── alarm_logic.c
│   ├── alarm_logic.h
│   ├── alert_dispatch.c
│   ├── alert_dispatch.h
│   ├── config.c
│   ├── config.h
│   ├── gpio_control.c
│   ├── gpio_control.h
│   ├── modem_hal.c
│   ├── modem_hal.h
│   ├── modem_state.c
│   ├── modem_state.h
│   ├── sim_modem.c
│   └── sim_modem.h
└── rknn_model_zoo/
```
