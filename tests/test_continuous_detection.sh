#!/bin/sh

# Standalone RTSP + RKNN YOLOv5 reliability test for the Luckfox board.
# Usage:
#   RTSP_URL='rtsp://user:pass@camera-ip:554/stream1' ./tests/test_continuous_detection.sh
#
# The RTSP URL is intentionally not hardcoded so camera credentials do not live
# in the repository.

set -u

DEMO_BIN="${DEMO_BIN:-/root/rknn_yolov5_demo_export/rknn_yolov5_demo}"
MODEL_PATH="${MODEL_PATH:-/root/rknn_yolov5_demo_export/model/yolov5.rknn}"
FRAME_PATH="${FRAME_PATH:-/tmp/amtech_rtsp_frame.jpg}"
DEMO_OUTPUT_PATH="${DEMO_OUTPUT_PATH:-/tmp/amtech_detection_output.txt}"
CSV_PATH="${CSV_PATH:-/tmp/amtech_continuous_detection.csv}"
TEST_DURATION_SECONDS="${TEST_DURATION_SECONDS:-180}"
FRAME_INTERVAL_SECONDS="${FRAME_INTERVAL_SECONDS:-2}"
RTSP_CAPTURE_TIMEOUT_SECONDS="${RTSP_CAPTURE_TIMEOUT_SECONDS:-15}"
FFMPEG_CAPTURE_PROFILE="${FFMPEG_CAPTURE_PROFILE:-probe_scale}"
FFMPEG_SCALE_FILTER="${FFMPEG_SCALE_FILTER:-scale=640:640}"
BENCHMARK_CAPTURE_PROFILES="${BENCHMARK_CAPTURE_PROFILES:-0}"
BENCHMARK_CAPTURE_RUNS="${BENCHMARK_CAPTURE_RUNS:-3}"

if [ -z "${RTSP_URL:-}" ]; then
    echo "ERROR: RTSP_URL is required."
    echo "Example:"
    echo "  RTSP_URL='rtsp://user:pass@camera-ip:554/stream1' $0"
    exit 1
fi

if [ ! -x "$DEMO_BIN" ]; then
    echo "ERROR: demo binary not executable: $DEMO_BIN"
    exit 1
fi

if [ ! -f "$MODEL_PATH" ]; then
    echo "ERROR: model file not found: $MODEL_PATH"
    exit 1
fi

if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "ERROR: ffmpeg not found on PATH"
    exit 1
fi

now_ms()
{
    value="$(date +%s%3N 2>/dev/null || true)"
    case "$value" in
        *N*|"")
            value="$(date +%s)"
            echo $((value * 1000))
            ;;
        *)
            echo "$value"
            ;;
    esac
}

rss_kb()
{
    awk '/VmRSS:/ { print $2; found=1 } END { if (!found) print 0 }' /proc/$$/status 2>/dev/null
}

frame_size_bytes()
{
    path="$1"

    wc -c < "$path" 2>/dev/null | awk '{ print $1 }'
}

frame_dimensions()
{
    path="$1"

    if command -v ffprobe >/dev/null 2>&1; then
        ffprobe -v error \
            -select_streams v:0 \
            -show_entries stream=width,height \
            -of csv=s=x:p=0 \
            "$path" 2>/dev/null
        return
    fi

    echo "unknown"
}

run_ffmpeg_capture_profile()
{
    profile="$1"

    case "$profile" in
        baseline)
            ffmpeg \
                -hide_banner \
                -loglevel error \
                -rtsp_transport tcp \
                -y \
                -i "$RTSP_URL" \
                -frames:v 1 \
                -q:v 2 \
                "$FRAME_PATH"
            ;;
        skipkey)
            ffmpeg \
                -hide_banner \
                -loglevel error \
                -rtsp_transport tcp \
                -skip_frame nokey \
                -y \
                -i "$RTSP_URL" \
                -frames:v 1 \
                -q:v 2 \
                "$FRAME_PATH"
            ;;
        scale)
            ffmpeg \
                -hide_banner \
                -loglevel error \
                -rtsp_transport tcp \
                -y \
                -i "$RTSP_URL" \
                -frames:v 1 \
                -vf "$FFMPEG_SCALE_FILTER" \
                -q:v 2 \
                "$FRAME_PATH"
            ;;
        probe)
            ffmpeg \
                -hide_banner \
                -loglevel error \
                -rtsp_transport tcp \
                -analyzeduration 1000000 \
                -probesize 32768 \
                -y \
                -i "$RTSP_URL" \
                -frames:v 1 \
                -q:v 2 \
                "$FRAME_PATH"
            ;;
        probe_scale)
            ffmpeg \
                -hide_banner \
                -loglevel error \
                -rtsp_transport tcp \
                -analyzeduration 1000000 \
                -probesize 32768 \
                -y \
                -i "$RTSP_URL" \
                -frames:v 1 \
                -vf "$FFMPEG_SCALE_FILTER" \
                -q:v 2 \
                "$FRAME_PATH"
            ;;
        combined)
            ffmpeg \
                -hide_banner \
                -loglevel error \
                -rtsp_transport tcp \
                -analyzeduration 1000000 \
                -probesize 32768 \
                -skip_frame nokey \
                -y \
                -i "$RTSP_URL" \
                -frames:v 1 \
                -vf "$FFMPEG_SCALE_FILTER" \
                -q:v 2 \
                "$FRAME_PATH"
            ;;
        *)
            echo "ERROR: unknown FFMPEG_CAPTURE_PROFILE=$profile" >&2
            return 2
            ;;
    esac
}

run_ffmpeg_capture()
{
    if command -v timeout >/dev/null 2>&1; then
        timeout "$RTSP_CAPTURE_TIMEOUT_SECONDS" "$0" --capture-once "$FFMPEG_CAPTURE_PROFILE"
        return $?
    fi

    run_ffmpeg_capture_profile "$FFMPEG_CAPTURE_PROFILE"
}

run_capture_profile_once()
{
    profile="$1"
    start_ms="$(now_ms)"
    rm -f "$FRAME_PATH"
    if run_ffmpeg_capture_profile "$profile" && [ -s "$FRAME_PATH" ]; then
        end_ms="$(now_ms)"
        echo $((end_ms - start_ms))
        return 0
    fi

    return 1
}

benchmark_capture_profiles()
{
    profiles="baseline skipkey scale probe probe_scale combined"

    echo "Capture profile benchmark"
    echo "  runs per profile: $BENCHMARK_CAPTURE_RUNS"
    echo "  scale filter: $FFMPEG_SCALE_FILTER"
    echo

    for profile in $profiles; do
        run=1
        successes=0
        failures=0
        total_ms=0
        best_ms=0

        echo "Profile: $profile"
        while [ "$run" -le "$BENCHMARK_CAPTURE_RUNS" ]; do
            capture_ms="$(run_capture_profile_once "$profile" 2>/dev/null || true)"
            if [ -n "$capture_ms" ]; then
                successes=$((successes + 1))
                total_ms=$((total_ms + capture_ms))
                if [ "$best_ms" -eq 0 ] || [ "$capture_ms" -lt "$best_ms" ]; then
                    best_ms="$capture_ms"
                fi
                echo "  run $run: ${capture_ms}ms"
            else
                failures=$((failures + 1))
                echo "  run $run: failed"
            fi
            run=$((run + 1))
        done

        if [ "$successes" -gt 0 ]; then
            average_ms=$((total_ms / successes))
        else
            average_ms=0
        fi

        echo "  summary: successes=$successes failures=$failures average_ms=$average_ms best_ms=$best_ms"
        echo
    done
}

parse_person_result()
{
    awk '
        $1 == "person" && $2 == "@" {
            confidence = $NF + 0
            if (!found || confidence > max_confidence) {
                max_confidence = confidence
            }
            found = 1
        }
        END {
            if (found) {
                printf "yes,%.3f", max_confidence
            } else {
                printf "no,"
            }
        }
    ' "$DEMO_OUTPUT_PATH"
}

if [ "${1:-}" = "--capture-once" ]; then
    run_ffmpeg_capture_profile "${2:-$FFMPEG_CAPTURE_PROFILE}"
    exit $?
fi

if [ "$BENCHMARK_CAPTURE_PROFILES" = "1" ]; then
    benchmark_capture_profiles
    exit 0
fi

start_ms="$(now_ms)"
end_ms=$((start_ms + TEST_DURATION_SECONDS * 1000))
frame_count=0
capture_failures=0
inference_failures=0
person_frames=0
total_frame_ms=0
total_capture_ms=0
total_inference_ms=0
max_rss_kb="$(rss_kb)"

echo "timestamp,person_detected,max_confidence,capture_ms,inference_ms,total_frame_ms,frame_bytes,frame_dimensions,capture_status,inference_status" > "$CSV_PATH"

echo "Starting continuous detection test"
echo "  demo: $DEMO_BIN"
echo "  model: $MODEL_PATH"
echo "  frame interval: ${FRAME_INTERVAL_SECONDS}s"
echo "  duration: ${TEST_DURATION_SECONDS}s"
echo "  ffmpeg capture profile: $FFMPEG_CAPTURE_PROFILE"
echo "  ffmpeg scale filter: $FFMPEG_SCALE_FILTER"
echo "  csv: $CSV_PATH"
echo

while :; do
    loop_start_ms="$(now_ms)"
    if [ "$loop_start_ms" -ge "$end_ms" ]; then
        break
    fi

    frame_count=$((frame_count + 1))
    timestamp="$(date '+%Y-%m-%dT%H:%M:%S%z')"
    capture_status="ok"
    inference_status="ok"
    person_detected="no"
    max_confidence=""
    frame_bytes=0
    frame_dims="unknown"

    echo "Processing frame $frame_count at $timestamp..."

    rm -f "$FRAME_PATH" "$DEMO_OUTPUT_PATH"
    capture_start_ms="$(now_ms)"
    if ! run_ffmpeg_capture; then
        capture_end_ms="$(now_ms)"
        capture_ms=$((capture_end_ms - capture_start_ms))
        capture_status="failed"
        capture_failures=$((capture_failures + 1))
        inference_status="skipped"
    elif [ ! -s "$FRAME_PATH" ]; then
        capture_end_ms="$(now_ms)"
        capture_ms=$((capture_end_ms - capture_start_ms))
        capture_status="empty"
        capture_failures=$((capture_failures + 1))
        inference_status="skipped"
    else
        capture_end_ms="$(now_ms)"
        capture_ms=$((capture_end_ms - capture_start_ms))
        frame_bytes="$(frame_size_bytes "$FRAME_PATH")"
        frame_dims="$(frame_dimensions "$FRAME_PATH")"
        inference_start_ms="$(now_ms)"
        if "$DEMO_BIN" "$MODEL_PATH" "$FRAME_PATH" > "$DEMO_OUTPUT_PATH" 2>&1; then
            parsed="$(parse_person_result)"
            person_detected="${parsed%%,*}"
            max_confidence="${parsed#*,}"
            if [ "$person_detected" = "yes" ]; then
                person_frames=$((person_frames + 1))
            fi
        else
            inference_status="failed"
            inference_failures=$((inference_failures + 1))
        fi
        inference_end_ms="$(now_ms)"
        inference_ms=$((inference_end_ms - inference_start_ms))
    fi

    if [ "$inference_status" = "skipped" ]; then
        inference_ms=0
    fi

    loop_end_ms="$(now_ms)"
    frame_ms=$((loop_end_ms - loop_start_ms))
    total_frame_ms=$((total_frame_ms + frame_ms))
    total_capture_ms=$((total_capture_ms + capture_ms))
    total_inference_ms=$((total_inference_ms + inference_ms))

    current_rss_kb="$(rss_kb)"
    if [ "$current_rss_kb" -gt "$max_rss_kb" ]; then
        max_rss_kb="$current_rss_kb"
    fi

    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$timestamp" \
        "$person_detected" \
        "$max_confidence" \
        "$capture_ms" \
        "$inference_ms" \
        "$frame_ms" \
        "$frame_bytes" \
        "$frame_dims" \
        "$capture_status" \
        "$inference_status" >> "$CSV_PATH"

    if [ "$person_detected" = "yes" ]; then
        echo "  person=yes max_confidence=$max_confidence capture_ms=$capture_ms inference_ms=$inference_ms total_frame_ms=$frame_ms frame_bytes=$frame_bytes frame_dims=$frame_dims rss_kb=$current_rss_kb"
    else
        echo "  person=no capture_ms=$capture_ms inference_ms=$inference_ms total_frame_ms=$frame_ms frame_bytes=$frame_bytes frame_dims=$frame_dims capture=$capture_status inference=$inference_status rss_kb=$current_rss_kb"
    fi

    sleep "$FRAME_INTERVAL_SECONDS"
done

if [ "$frame_count" -gt 0 ]; then
    average_frame_ms=$((total_frame_ms / frame_count))
    average_capture_ms=$((total_capture_ms / frame_count))
    average_inference_ms=$((total_inference_ms / frame_count))
else
    average_frame_ms=0
    average_capture_ms=0
    average_inference_ms=0
fi

echo
echo "Continuous detection summary"
echo "  frames attempted: $frame_count"
echo "  capture failures: $capture_failures"
echo "  inference failures: $inference_failures"
echo "  frames with person detected: $person_frames"
echo "  average capture time: ${average_capture_ms}ms"
echo "  average inference time: ${average_inference_ms}ms"
echo "  average capture+inference time: ${average_frame_ms}ms"
echo "  max script RSS observed: ${max_rss_kb}KB"
echo "  csv written: $CSV_PATH"
