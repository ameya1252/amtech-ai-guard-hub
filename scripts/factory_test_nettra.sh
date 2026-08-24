#!/bin/sh
#
# AMTECH NETTRA factory/field acceptance harness.
#
# Safe default:
#   - Does not toggle relays.
#   - Does not place calls.
#   - Does not send SMS.
#   - Does not modify /root/amtech_config.txt.
#
# Typical board run:
#   DEVICE_SERIAL=AMT-0000 /root/factory_test_nettra.sh
#
# Optional deeper checks:
#   FACTORY_TEST_CAMERA=1 DEVICE_SERIAL=AMT-0000 /root/factory_test_nettra.sh
#   FACTORY_TEST_MODEM_AT=1 DEVICE_SERIAL=AMT-0000 /root/factory_test_nettra.sh
#   FACTORY_TEST_RELAYS=1 DEVICE_SERIAL=AMT-0000 /root/factory_test_nettra.sh

set -u

DEVICE_SERIAL="${DEVICE_SERIAL:-unknown-device}"
CONFIG_PATH="${CONFIG_PATH:-/root/amtech_config.txt}"
RUNTIME_BIN="${RUNTIME_BIN:-/root/runtime_loop}"
INIT_SCRIPT="${INIT_SCRIPT:-/etc/init.d/S95runtime_loop}"
RUNTIME_LOG="${RUNTIME_LOG:-/root/runtime_loop.log}"
DEMO_BIN="${DEMO_BIN:-/root/rknn_yolov5_demo_export/rknn_yolov5_demo}"
MODEL_PATH="${MODEL_PATH:-/root/rknn_yolov5_demo_export/model/yolov5.rknn}"
CLAHE_BIN="${CLAHE_BIN:-/root/amtech_clahe_ppm}"
REPORT_DIR="${REPORT_DIR:-/root/factory_reports}"
BACKEND_HEALTH_URL="${BACKEND_HEALTH_URL:-https://amtech-ai-guard-hub-production.up.railway.app/health}"

FACTORY_TEST_CAMERA="${FACTORY_TEST_CAMERA:-0}"
FACTORY_TEST_MODEM_AT="${FACTORY_TEST_MODEM_AT:-0}"
FACTORY_TEST_RELAYS="${FACTORY_TEST_RELAYS:-0}"

AMTECH_ALARM_GPIO_PIN=49
AMTECH_STROBE_GPIO_PIN=48
AMTECH_PANIC_GPIO_PIN=32
AMTECH_SHUTTER1_NC_GPIO_PIN=33
AMTECH_SHUTTER1_NO_GPIO_PIN=40
AMTECH_SHUTTER2_NC_GPIO_PIN=41
AMTECH_SHUTTER2_NO_GPIO_PIN=72
AMTECH_SMOKE_GPIO_PIN=54
AMTECH_SIM_RESET_GPIO_PIN=57

PASS_COUNT=0
FAIL_COUNT=0
WARN_COUNT=0
REPORT_PATH=""

timestamp()
{
    date '+%Y-%m-%d %H:%M:%S'
}

safe_serial()
{
    printf '%s' "$DEVICE_SERIAL" | tr -c 'A-Za-z0-9_.-' '_'
}

log_line()
{
    level="$1"
    shift
    message="$*"
    printf '[%s] [%s] %s\n' "$(timestamp)" "$level" "$message" | tee -a "$REPORT_PATH"
}

pass()
{
    PASS_COUNT=$((PASS_COUNT + 1))
    log_line "PASS" "$*"
}

fail()
{
    FAIL_COUNT=$((FAIL_COUNT + 1))
    log_line "FAIL" "$*"
}

warn()
{
    WARN_COUNT=$((WARN_COUNT + 1))
    log_line "WARN" "$*"
}

info()
{
    log_line "INFO" "$*"
}

config_value()
{
    key="$1"

    if [ ! -f "$CONFIG_PATH" ]; then
        return 0
    fi

    sed -n "s/^${key}=//p" "$CONFIG_PATH" | tail -n 1
}

check_file()
{
    label="$1"
    path="$2"

    if [ -f "$path" ]; then
        pass "$label exists: $path"
    else
        fail "$label missing: $path"
    fi
}

check_executable()
{
    label="$1"
    path="$2"

    if [ -x "$path" ]; then
        pass "$label executable: $path"
    else
        fail "$label missing or not executable: $path"
    fi
}

check_command()
{
    command_name="$1"

    if command -v "$command_name" >/dev/null 2>&1; then
        pass "command available: $command_name"
    else
        fail "command missing: $command_name"
    fi
}

check_config_int()
{
    key="$1"
    expected_pattern="$2"
    value="$(config_value "$key")"

    if [ -z "$value" ]; then
        fail "config $key missing"
        return
    fi

    case "$value" in
        $expected_pattern)
            pass "config $key=$value"
            ;;
        *)
            fail "config $key=$value does not match expected $expected_pattern"
            ;;
    esac
}

check_config_present()
{
    key="$1"
    value="$(config_value "$key")"

    if [ -n "$value" ]; then
        pass "config $key present"
    else
        fail "config $key missing or empty"
    fi
}

check_gpio_export_possible()
{
    pin="$1"
    label="$2"

    if [ -d "/sys/class/gpio/gpio${pin}" ]; then
        pass "$label GPIO$pin already exported"
        return
    fi

    if [ -w "/sys/class/gpio/export" ]; then
        pass "$label GPIO$pin export interface available"
    else
        warn "$label GPIO$pin not exported and /sys/class/gpio/export is not writable in this shell"
    fi
}

check_health()
{
    if command -v curl >/dev/null 2>&1; then
        response="$(curl -fsS --max-time 10 "$BACKEND_HEALTH_URL" 2>/tmp/amtech_factory_curl.err || true)"
        if printf '%s' "$response" | grep -q '"status"[[:space:]]*:[[:space:]]*"ok"'; then
            pass "backend health OK: $BACKEND_HEALTH_URL"
        else
            err="$(cat /tmp/amtech_factory_curl.err 2>/dev/null)"
            warn "backend health not confirmed: ${err:-empty response}"
        fi
    else
        warn "curl missing; backend health check skipped"
    fi
}

runtime_status_check()
{
    if [ -x "$INIT_SCRIPT" ]; then
        if "$INIT_SCRIPT" status >/tmp/amtech_factory_runtime_status.txt 2>&1; then
            pass "runtime init status reports running"
        else
            warn "runtime init status did not report running: $(cat /tmp/amtech_factory_runtime_status.txt)"
        fi
    else
        warn "runtime init script not executable; status skipped"
    fi

    if command -v ps >/dev/null 2>&1 && ps | grep '[r]untime_loop' >/dev/null 2>&1; then
        pass "runtime_loop process visible"
    else
        warn "runtime_loop process not visible"
    fi
}

watchdog_check()
{
    watchdog_enabled="$(config_value WATCHDOG_ENABLED)"

    if [ -e /dev/watchdog0 ] || [ -e /dev/watchdog ]; then
        pass "watchdog device present"
    else
        if [ "$watchdog_enabled" = "1" ]; then
            fail "WATCHDOG_ENABLED=1 but no /dev/watchdog0 or /dev/watchdog present"
        else
            warn "watchdog device not present; WATCHDOG_ENABLED=${watchdog_enabled:-0}"
        fi
    fi
}

modem_check()
{
    modem_device="$(config_value MODEM_DEVICE)"
    [ -z "$modem_device" ] && modem_device="/dev/ttyS5"

    if [ -e "$modem_device" ]; then
        pass "modem device present: $modem_device"
    else
        fail "modem device missing: $modem_device"
        return
    fi

    if [ "$FACTORY_TEST_MODEM_AT" != "1" ]; then
        info "modem AT test skipped; set FACTORY_TEST_MODEM_AT=1 to send AT"
        return
    fi

    if [ -x /root/test_raw_at ]; then
        if /root/test_raw_at >/tmp/amtech_factory_raw_at.txt 2>&1; then
            if grep -q "OK" /tmp/amtech_factory_raw_at.txt; then
                pass "modem AT diagnostic returned OK"
            else
                fail "modem AT diagnostic ran but OK not found"
            fi
        else
            fail "modem AT diagnostic failed"
        fi
        sed 's/^/[raw_at] /' /tmp/amtech_factory_raw_at.txt >> "$REPORT_PATH"
    else
        warn "/root/test_raw_at not found; modem AT test skipped"
    fi
}

camera_check_one()
{
    name="$1"
    enabled_key="$2"
    url_key="$3"
    mac_key="$4"
    enabled="$(config_value "$enabled_key")"
    url="$(config_value "$url_key")"
    mac="$(config_value "$mac_key")"

    if [ "$enabled" != "1" ]; then
        info "$name camera disabled by $enabled_key=${enabled:-0}; skipping RTSP check"
        return
    fi

    if [ -z "$url" ]; then
        fail "$name camera enabled but $url_key is empty"
        return
    fi

    pass "$name camera configured"
    if [ -n "$mac" ]; then
        pass "$name camera stable MAC configured: $mac"
    else
        info "$name camera MAC not configured; using RTSP URL IP only"
    fi

    if [ "$FACTORY_TEST_CAMERA" != "1" ]; then
        info "$name camera RTSP/NPU check skipped; set FACTORY_TEST_CAMERA=1 to run"
        return
    fi

    frame="/tmp/amtech_factory_${name}.jpg"
    output="/tmp/amtech_factory_${name}_detect.txt"
    rm -f "$frame" "$output"

    if ! command -v ffmpeg >/dev/null 2>&1; then
        fail "$name camera check needs ffmpeg"
        return
    fi

    if command -v timeout >/dev/null 2>&1; then
        timeout 20 ffmpeg -hide_banner -loglevel error -rtsp_transport tcp \
            -analyzeduration 1000000 -probesize 32768 -y -i "$url" \
            -frames:v 1 \
            -vf 'scale=480:480:force_original_aspect_ratio=decrease,pad=480:480:(ow-iw)/2:(oh-ih)/2' \
            -q:v 2 "$frame" >/tmp/amtech_factory_ffmpeg.txt 2>&1
    else
        ffmpeg -hide_banner -loglevel error -rtsp_transport tcp \
            -analyzeduration 1000000 -probesize 32768 -y -i "$url" \
            -frames:v 1 \
            -vf 'scale=480:480:force_original_aspect_ratio=decrease,pad=480:480:(ow-iw)/2:(oh-ih)/2' \
            -q:v 2 "$frame" >/tmp/amtech_factory_ffmpeg.txt 2>&1
    fi

    if [ ! -s "$frame" ]; then
        fail "$name camera did not capture a frame"
        sed 's/^/[ffmpeg] /' /tmp/amtech_factory_ffmpeg.txt >> "$REPORT_PATH"
        return
    fi

    pass "$name camera captured frame"

    if [ ! -x "$DEMO_BIN" ] || [ ! -f "$MODEL_PATH" ]; then
        fail "$name camera NPU test missing demo/model"
        return
    fi

    demo_dir="$(dirname "$DEMO_BIN")"
    demo_name="$(basename "$DEMO_BIN")"
    model_arg="$MODEL_PATH"
    case "$MODEL_PATH" in
        "$demo_dir"/*)
            model_arg="${MODEL_PATH#$demo_dir/}"
            ;;
    esac

    (
        cd "$demo_dir" || exit 1
        "./$demo_name" "$model_arg" "$frame"
    ) >"$output" 2>&1

    if grep -q "person @\\|load label\\|load lable\\|model" "$output"; then
        pass "$name camera RKNN demo ran"
    else
        warn "$name camera RKNN demo output did not include expected detector markers"
    fi
    sed 's/^/[detect] /' "$output" >> "$REPORT_PATH"
}

camera_checks()
{
    check_executable "RKNN YOLOv5 demo" "$DEMO_BIN"
    check_file "YOLOv5 RKNN model" "$MODEL_PATH"
    check_executable "CLAHE utility" "$CLAHE_BIN"
    check_command ffmpeg

    camera_check_one "front" CAMERA_ENABLED CAMERA_RTSP_URL CAMERA_MAC
    camera_check_one "parking" CAMERA2_ENABLED CAMERA2_RTSP_URL CAMERA2_MAC
}

relay_check()
{
    if [ "$FACTORY_TEST_RELAYS" != "1" ]; then
        info "relay toggle test skipped; set FACTORY_TEST_RELAYS=1 only with siren/strobe safely disconnected or expected"
        return
    fi

    for pin in "$AMTECH_ALARM_GPIO_PIN" "$AMTECH_STROBE_GPIO_PIN"; do
        if [ ! -d "/sys/class/gpio/gpio${pin}" ]; then
            echo "$pin" > /sys/class/gpio/export 2>/dev/null || true
            sleep 1
        fi

        if [ ! -w "/sys/class/gpio/gpio${pin}/value" ]; then
            fail "relay GPIO$pin value not writable"
            continue
        fi

        echo high > "/sys/class/gpio/gpio${pin}/direction" 2>/dev/null || \
            echo out > "/sys/class/gpio/gpio${pin}/direction" 2>/dev/null || true
        echo 1 > "/sys/class/gpio/gpio${pin}/value"
        sleep 1
        echo 0 > "/sys/class/gpio/gpio${pin}/value"
        sleep 1
        echo 1 > "/sys/class/gpio/gpio${pin}/value"
        pass "relay GPIO$pin toggled active-LOW ON then OFF"
    done
}

main()
{
    mkdir -p "$REPORT_DIR"
    REPORT_PATH="$REPORT_DIR/nettra_factory_$(safe_serial)_$(date '+%Y%m%d_%H%M%S').log"
    : > "$REPORT_PATH"

    info "AMTECH NETTRA factory test started"
    info "device_serial=$DEVICE_SERIAL"
    info "config=$CONFIG_PATH"
    info "report=$REPORT_PATH"

    check_file "config" "$CONFIG_PATH"
    check_executable "runtime binary" "$RUNTIME_BIN"
    check_executable "runtime init script" "$INIT_SCRIPT"

    check_config_int SHUTTER_COUNT "[12]"
    check_config_int PANIC_ENABLED "[01]"
    check_config_int SMOKE_ENABLED "[01]"
    check_config_present SCHEDULE_ARM
    check_config_present SCHEDULE_DISARM
    check_config_present MODEM_DEVICE
    check_config_present ALERT_CONTACT_1
    check_config_present BACKEND_BASE_URL

    check_gpio_export_possible "$AMTECH_ALARM_GPIO_PIN" "siren relay"
    check_gpio_export_possible "$AMTECH_STROBE_GPIO_PIN" "strobe relay"
    check_gpio_export_possible "$AMTECH_PANIC_GPIO_PIN" "panic"
    check_gpio_export_possible "$AMTECH_SHUTTER1_NC_GPIO_PIN" "shutter1 NC"
    check_gpio_export_possible "$AMTECH_SHUTTER1_NO_GPIO_PIN" "shutter1 NO"
    if [ "$(config_value SHUTTER_COUNT)" = "2" ]; then
        check_gpio_export_possible "$AMTECH_SHUTTER2_NC_GPIO_PIN" "shutter2 NC"
        check_gpio_export_possible "$AMTECH_SHUTTER2_NO_GPIO_PIN" "shutter2 NO"
    fi
    if [ "$(config_value SMOKE_ENABLED)" = "1" ]; then
        check_gpio_export_possible "$AMTECH_SMOKE_GPIO_PIN" "smoke"
    fi
    check_gpio_export_possible "$AMTECH_SIM_RESET_GPIO_PIN" "SIM7672 reset"

    check_health
    runtime_status_check
    watchdog_check
    modem_check
    camera_checks
    relay_check

    if [ -f "$RUNTIME_LOG" ]; then
        info "last runtime log lines:"
        tail -n 40 "$RUNTIME_LOG" 2>/dev/null | sed 's/^/[runtime] /' >> "$REPORT_PATH"
    else
        warn "runtime log not found: $RUNTIME_LOG"
    fi

    info "summary: PASS=$PASS_COUNT WARN=$WARN_COUNT FAIL=$FAIL_COUNT"
    printf '\nFactory report: %s\n' "$REPORT_PATH"
    printf 'PASS=%d WARN=%d FAIL=%d\n' "$PASS_COUNT" "$WARN_COUNT" "$FAIL_COUNT"

    if [ "$FAIL_COUNT" -gt 0 ]; then
        exit 1
    fi

    exit 0
}

main "$@"
