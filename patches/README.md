# AMTECH rknn_model_zoo Patches

These patches modify Rockchip's `rknn_model_zoo` YOLOv5 C++ example to wire in AMTECH's `alarm_logic.c`, `gpio_control.c`, and `notify_client.c`.

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
│   ├── gpio_control.c
│   └── gpio_control.h
└── rknn_model_zoo/
```
