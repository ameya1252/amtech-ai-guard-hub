#!/bin/sh

# Standalone persistent-RTSP + RKNN YOLOv5 reliability test for the Luckfox board.
# Usage:
#   RTSP_URL='rtsp://user:pass@camera-ip:554/stream1' ./tests/test_continuous_detection_persistent_rtsp.sh
#
# Keeps one ffmpeg RTSP connection open and writes periodic JPEG frames. The
# detection loop processes only stable, fully-written frame files.

set -u

DEMO_BIN="${DEMO_BIN:-/root/rknn_yolov5_demo_export/rknn_yolov5_demo}"
MODEL_PATH="${MODEL_PATH:-/root/rknn_yolov5_demo_export/model/yolov5.rknn}"
FRAME_DIR="${FRAME_DIR:-/tmp/amtech_rtsp_frames}"
DEMO_OUTPUT_PATH="${DEMO_OUTPUT_PATH:-/tmp/amtech_detection_output.txt}"
CSV_PATH="${CSV_PATH:-/tmp/amtech_continuous_detection_persistent.csv}"
TEST_DURATION_SECONDS="${TEST_DURATION_SECONDS:-180}"
FRAME_INTERVAL_SECONDS="${FRAME_INTERVAL_SECONDS:-2}"
FRAME_STABLE_DELAY_SECONDS="${FRAME_STABLE_DELAY_SECONDS:-1}"
FRAME_RETENTION_COUNT="${FRAME_RETENTION_COUNT:-20}"
FFMPEG_LOG_PATH="${FFMPEG_LOG_PATH:-/tmp/amtech_rtsp_ffmpeg.log}"

ffmpeg_pid=""

cleanup()
{
    if [ -n "$ffmpeg_pid" ]; then
        if kill -0 "$ffmpeg_pid" 2>/dev/null; then
            echo
            echo "Stopping ffmpeg pid $ffmpeg_pid..."
            kill "$ffmpeg_pid" 2>/dev/null || true
            wait "$ffmpeg_pid" 2>/dev/null || true
        fi
    fi
}

trap cleanup EXIT INT TERM

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

file_mtime_ms()
{
    path="$1"

    value="$(stat -c %Y "$path" 2>/dev/null || true)"
    if [ -z "$value" ]; then
        value="$(stat -f %m "$path" 2>/dev/null || true)"
    fi

    case "$value" in
        ''|*[!0-9]*|0)
            now_ms
            ;;
        *)
            echo $((value * 1000))
            ;;
    esac
}

rss_kb()
{
    awk '/VmRSS:/ { print $2; found=1 } END { if (!found) print 0 }' /proc/$$/status 2>/dev/null
}

process_rss_kb()
{
    pid="$1"

    awk '/VmRSS:/ { print $2; found=1 } END { if (!found) print 0 }' "/proc/$pid/status" 2>/dev/null
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

is_stable_file()
{
    path="$1"

    [ -s "$path" ] || return 1
    size_before="$(wc -c < "$path" 2>/dev/null || echo 0)"
    sleep "$FRAME_STABLE_DELAY_SECONDS"
    [ -s "$path" ] || return 1
    size_after="$(wc -c < "$path" 2>/dev/null || echo 0)"

    [ "$size_before" = "$size_after" ] && [ "$size_after" -gt 0 ]
}

newest_frame()
{
    ls -1 "$FRAME_DIR"/frame_*.jpg 2>/dev/null | sort | tail -n 1
}

delete_old_frames()
{
    frame_count="$(ls -1 "$FRAME_DIR"/frame_*.jpg 2>/dev/null | wc -l | awk '{ print $1 }')"
    delete_count=$((frame_count - FRAME_RETENTION_COUNT))

    if [ "$delete_count" -gt 0 ]; then
        ls -1 "$FRAME_DIR"/frame_*.jpg 2>/dev/null | sort | head -n "$delete_count" | while IFS= read -r old_frame; do
            rm -f "$old_frame"
        done
    fi
}

mkdir -p "$FRAME_DIR"
rm -f "$FRAME_DIR"/frame_*.jpg "$DEMO_OUTPUT_PATH"

echo "Starting persistent RTSP ffmpeg capture"
echo "  frame dir: $FRAME_DIR"
echo "  ffmpeg log: $FFMPEG_LOG_PATH"

ffmpeg \
    -hide_banner \
    -loglevel warning \
    -rtsp_transport tcp \
    -i "$RTSP_URL" \
    -vf fps=0.5 \
    -q:v 2 \
    "$FRAME_DIR/frame_%06d.jpg" > "$FFMPEG_LOG_PATH" 2>&1 &
ffmpeg_pid="$!"

sleep 3
if ! kill -0 "$ffmpeg_pid" 2>/dev/null; then
    echo "ERROR: ffmpeg exited during startup. Log:"
    cat "$FFMPEG_LOG_PATH"
    exit 1
fi

start_ms="$(now_ms)"
end_ms=$((start_ms + TEST_DURATION_SECONDS * 1000))
processed_count=0
inference_failures=0
stale_skips=0
person_frames=0
total_inference_ms=0
total_stability_ms=0
total_process_ms=0
total_age_ms=0
max_age_ms=0
max_rss_kb="$(rss_kb)"
max_ffmpeg_rss_kb=0
last_processed=""

echo "timestamp,frame_file,person_detected,max_confidence,stability_check_ms,demo_inference_ms,total_process_ms,frame_age_ms,inference_status" > "$CSV_PATH"

echo
echo "Starting persistent continuous detection test"
echo "  demo: $DEMO_BIN"
echo "  model: $MODEL_PATH"
echo "  ffmpeg fps: 0.5 (one frame every 2 seconds)"
echo "  duration: ${TEST_DURATION_SECONDS}s"
echo "  csv: $CSV_PATH"
echo

while :; do
    loop_ms="$(now_ms)"
    if [ "$loop_ms" -ge "$end_ms" ]; then
        break
    fi

    if ! kill -0 "$ffmpeg_pid" 2>/dev/null; then
        echo "ERROR: ffmpeg stopped unexpectedly. Log:"
        cat "$FFMPEG_LOG_PATH"
        break
    fi

    frame_path="$(newest_frame)"
    if [ -z "$frame_path" ]; then
        sleep 1
        continue
    fi

    if [ "$frame_path" = "$last_processed" ]; then
        stale_skips=$((stale_skips + 1))
        sleep 1
        continue
    fi

    process_start_ms="$(now_ms)"
    stability_start_ms="$process_start_ms"
    if ! is_stable_file "$frame_path"; then
        sleep 1
        continue
    fi
    stability_end_ms="$(now_ms)"
    stability_ms=$((stability_end_ms - stability_start_ms))

    timestamp="$(date '+%Y-%m-%dT%H:%M:%S%z')"
    frame_name="$(basename "$frame_path")"
    frame_age_ms=$(( $(now_ms) - $(file_mtime_ms "$frame_path") ))
    if [ "$frame_age_ms" -lt 0 ]; then
        frame_age_ms=0
    fi

    person_detected="no"
    max_confidence=""
    inference_status="ok"

    echo "Processing $frame_name at $timestamp..."

    rm -f "$DEMO_OUTPUT_PATH"
    inference_start_ms="$(now_ms)"
    if "$DEMO_BIN" "$MODEL_PATH" "$frame_path" > "$DEMO_OUTPUT_PATH" 2>&1; then
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
    process_end_ms="$inference_end_ms"
    total_process_frame_ms=$((process_end_ms - process_start_ms))

    processed_count=$((processed_count + 1))
    total_inference_ms=$((total_inference_ms + inference_ms))
    total_stability_ms=$((total_stability_ms + stability_ms))
    total_process_ms=$((total_process_ms + total_process_frame_ms))
    total_age_ms=$((total_age_ms + frame_age_ms))
    if [ "$frame_age_ms" -gt "$max_age_ms" ]; then
        max_age_ms="$frame_age_ms"
    fi

    current_rss_kb="$(rss_kb)"
    if [ "$current_rss_kb" -gt "$max_rss_kb" ]; then
        max_rss_kb="$current_rss_kb"
    fi
    current_ffmpeg_rss_kb="$(process_rss_kb "$ffmpeg_pid")"
    if [ "$current_ffmpeg_rss_kb" -gt "$max_ffmpeg_rss_kb" ]; then
        max_ffmpeg_rss_kb="$current_ffmpeg_rss_kb"
    fi

    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$timestamp" \
        "$frame_name" \
        "$person_detected" \
        "$max_confidence" \
        "$stability_ms" \
        "$inference_ms" \
        "$total_process_frame_ms" \
        "$frame_age_ms" \
        "$inference_status" >> "$CSV_PATH"

    if [ "$person_detected" = "yes" ]; then
        echo "  person=yes max_confidence=$max_confidence stability_ms=$stability_ms demo_inference_ms=$inference_ms total_process_ms=$total_process_frame_ms frame_age_ms=$frame_age_ms script_rss_kb=$current_rss_kb ffmpeg_rss_kb=$current_ffmpeg_rss_kb"
    else
        echo "  person=no stability_ms=$stability_ms demo_inference_ms=$inference_ms total_process_ms=$total_process_frame_ms frame_age_ms=$frame_age_ms inference=$inference_status script_rss_kb=$current_rss_kb ffmpeg_rss_kb=$current_ffmpeg_rss_kb"
    fi

    last_processed="$frame_path"
    delete_old_frames
done

if [ "$processed_count" -gt 0 ]; then
    average_inference_ms=$((total_inference_ms / processed_count))
    average_stability_ms=$((total_stability_ms / processed_count))
    average_process_ms=$((total_process_ms / processed_count))
    average_age_ms=$((total_age_ms / processed_count))
else
    average_inference_ms=0
    average_stability_ms=0
    average_process_ms=0
    average_age_ms=0
fi

echo
echo "Persistent continuous detection summary"
echo "  frames processed: $processed_count"
echo "  inference failures: $inference_failures"
echo "  stale/latest-frame skips: $stale_skips"
echo "  frames with person detected: $person_frames"
echo "  average file stability check time: ${average_stability_ms}ms"
echo "  average demo inference time: ${average_inference_ms}ms"
echo "  average total processing time: ${average_process_ms}ms"
echo "  average frame age: ${average_age_ms}ms"
echo "  max frame age: ${max_age_ms}ms"
echo "  max script RSS observed: ${max_rss_kb}KB"
echo "  max ffmpeg RSS observed: ${max_ffmpeg_rss_kb}KB"
echo "  csv written: $CSV_PATH"
echo "  ffmpeg log: $FFMPEG_LOG_PATH"
