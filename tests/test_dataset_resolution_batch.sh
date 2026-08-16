#!/bin/sh

DATASET_DIR="${AMTECH_DATASET_DIR:-/root/amtech-dataset}"
EXPORT_DIR="${AMTECH_EXPORT_DIR:-/root/rknn_yolov5_demo_export}"
DEMO_BIN="${AMTECH_DEMO_BIN:-$EXPORT_DIR/rknn_yolov5_demo}"
MODEL_PATH="${AMTECH_MODEL_PATH:-$EXPORT_DIR/model/yolov5.rknn}"
OUTPUT_DIR="${AMTECH_OUTPUT_DIR:-/root/amtech-resolution-results}"
RESOLUTIONS="${AMTECH_RESOLUTIONS:-640 480 320}"
TMP_DIR="${AMTECH_TMP_DIR:-/tmp/amtech_resolution_batch}"
TMP_OUTPUT="$TMP_DIR/demo_output.$$"

mkdir -p "$OUTPUT_DIR" "$TMP_DIR"

SUMMARY_CSV="$OUTPUT_DIR/resolution_summary.csv"
DETAILS_CSV="$OUTPUT_DIR/resolution_detections.csv"

printf "resolution,filename,person_detected,max_confidence\n" > "$SUMMARY_CSV"
printf "resolution,filename,class_name,confidence,x1,y1,x2,y2\n" > "$DETAILS_CSV"

cleanup()
{
    rm -f "$TMP_OUTPUT" "$TMP_DIR"/resized_*.jpg
}

trap cleanup EXIT INT TERM

extract_max_person_confidence()
{
    awk '
        {
            token = ""

            if ($0 ~ /person[[:space:]]*@/) {
                token = $NF
                gsub(/[^0-9.]/, "", token)
            } else if ($0 ~ /Alarm:[[:space:]]*person detected/ && match($0, /confidence=[0-9.][0-9.]*/)) {
                token = substr($0, RSTART + 11, RLENGTH - 11)
            }

            if (token ~ /^[0-9][0-9.]*$/) {
                value = token + 0
                if (value >= 0 && value <= 1 && (!found || value > max)) {
                    max = value
                }
                found = 1
            }
        }
        END {
            if (found) {
                printf "%.3f", max
            }
        }
    ' "$1"
}

append_detection_details()
{
    resolution="$1"
    filename="$2"
    output_file="$3"

    awk -v resolution="$resolution" -v filename="$filename" '
        /[[:space:]]@[[:space:]]*\(/ {
            line = $0

            class_name = line
            sub(/[[:space:]]+@[[:space:]]*\(.*/, "", class_name)
            gsub(/^[[:space:]]+|[[:space:]]+$/, "", class_name)

            coords = line
            sub(/^.*\(/, "", coords)
            sub(/\).*$/, "", coords)
            split(coords, box, /[[:space:]]+/)

            confidence = line
            sub(/^.*\)[[:space:]]*/, "", confidence)
            gsub(/[^0-9.]/, "", confidence)

            if (class_name != "" &&
                confidence ~ /^[0-9][0-9.]*$/ &&
                box[1] ~ /^[0-9]+$/ &&
                box[2] ~ /^[0-9]+$/ &&
                box[3] ~ /^[0-9]+$/ &&
                box[4] ~ /^[0-9]+$/) {
                printf "%s,%s,%s,%.3f,%s,%s,%s,%s\n",
                    resolution, filename, class_name, confidence + 0,
                    box[1], box[2], box[3], box[4]
            }
        }
    ' "$output_file" >> "$DETAILS_CSV"
}

list_images()
{
    for image_path in "$DATASET_DIR"/data_*.jpg
    do
        if [ -f "$image_path" ]; then
            printf "%s\n" "$image_path"
        fi
    done
}

run_detection()
{
    image_path="$1"
    resolution="$2"
    filename="$3"
    resized_image="$TMP_DIR/resized_${resolution}_${filename}"

    ffmpeg -hide_banner -loglevel error -y \
        -i "$image_path" \
        -frames:v 1 \
        -vf "scale=${resolution}:${resolution}:force_original_aspect_ratio=decrease,pad=${resolution}:${resolution}:(ow-iw)/2:(oh-ih)/2" \
        -q:v 2 \
        "$resized_image"
    resize_status=$?

    if [ "$resize_status" -ne 0 ] || [ ! -s "$resized_image" ]; then
        printf "Warning: resize failed for %s at %sx%s\n" "$filename" "$resolution" "$resolution" >&2
        return 1
    fi

    # Run from the export directory because the Rockchip demo loads labels from ./model/.
    (
        cd "$EXPORT_DIR" || exit 1
        "$DEMO_BIN" "$MODEL_PATH" "$resized_image"
    ) > "$TMP_OUTPUT" 2>&1
}

for resolution in $RESOLUTIONS
do
    per_resolution_csv="$OUTPUT_DIR/results_${resolution}.csv"
    printf "filename,person_detected,max_confidence\n" > "$per_resolution_csv"

    printf "\n=== Resolution %sx%s ===\n" "$resolution" "$resolution"

    list_images | while IFS= read -r image_path
    do
        image_path=$(printf "%s" "$image_path" | sed 's/[[:space:]]*$//')
        filename=$(basename "$image_path")
        printf "Processing %s at %sx%s...\n" "$filename" "$resolution" "$resolution"

        run_detection "$image_path" "$resolution" "$filename"
        status=$?

        if [ "$status" -ne 0 ]; then
            printf "Warning: detection failed for %s at %sx%s\n" "$filename" "$resolution" "$resolution" >&2
        fi

        max_confidence=$(extract_max_person_confidence "$TMP_OUTPUT")
        append_detection_details "$resolution" "$filename" "$TMP_OUTPUT"

        if [ -n "$max_confidence" ]; then
            person_detected="yes"
        else
            person_detected="no"
            max_confidence=""
        fi

        printf "%s,%s,%s\n" "$filename" "$person_detected" "$max_confidence" >> "$per_resolution_csv"
        printf "%s,%s,%s,%s\n" "$resolution" "$filename" "$person_detected" "$max_confidence" >> "$SUMMARY_CSV"
    done

    printf "Saved %s\n" "$per_resolution_csv"
done

printf "\nDone.\n"
printf "Combined summary: %s\n" "$SUMMARY_CSV"
printf "Detailed detections: %s\n" "$DETAILS_CSV"
printf "Per-resolution files: %s/results_640.csv, results_480.csv, results_320.csv\n" "$OUTPUT_DIR"
