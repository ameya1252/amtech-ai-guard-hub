#!/bin/sh

DATASET_DIR="${AMTECH_DATASET_DIR:-/root/amtech-dataset}"
EXPORT_DIR="${AMTECH_EXPORT_DIR:-/root/rknn_yolov5_demo_export}"
DEMO_BIN="${AMTECH_DEMO_BIN:-$EXPORT_DIR/rknn_yolov5_demo}"
MODEL_PATH="${AMTECH_MODEL_PATH:-$EXPORT_DIR/model/yolov5.rknn}"
RESULTS_CSV="${AMTECH_RESULTS_CSV:-/root/results.csv}"
DETAILS_CSV="${AMTECH_DETAILS_CSV:-/root/detections.csv}"
TMP_OUTPUT="/tmp/amtech_batch_output.$$"

printf "filename,person_detected,max_confidence\n" > "$RESULTS_CSV"
printf "filename,class_name,confidence,x1,y1,x2,y2\n" > "$DETAILS_CSV"

cleanup()
{
    rm -f "$TMP_OUTPUT"
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
    filename="$1"
    output_file="$2"

    awk -v filename="$filename" '
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
                printf "%s,%s,%.3f,%s,%s,%s,%s\n",
                    filename, class_name, confidence + 0, box[1], box[2], box[3], box[4]
            }
        }
    ' "$output_file" >> "$DETAILS_CSV"
}

list_images()
{
    if [ "$#" -gt 0 ]; then
        for image_path in "$@"
        do
            printf "%s\n" "$image_path"
        done
    else
        find "$DATASET_DIR" -maxdepth 1 -type f -name "data_*.jpg" |
            sed 's#^\(.*data_\)\([0-9][0-9]*\)\.jpg$#\2 \1\2.jpg#' |
            sort -n |
            sed 's/^[0-9][0-9]* //'
    fi
}

list_images "$@" | while IFS= read -r image_path
do
    image_path=$(printf "%s" "$image_path" | sed 's/[[:space:]]*$//')
    filename=$(basename "$image_path")
    printf "Processing [%s]...\n" "$filename"

    "$DEMO_BIN" "$MODEL_PATH" "$image_path" > "$TMP_OUTPUT" 2>&1
    status=$?

    if [ "$status" -ne 0 ]; then
        printf "Warning: demo failed for %s with exit code %s\n" "$filename" "$status" >&2
    fi

    max_confidence=$(extract_max_person_confidence "$TMP_OUTPUT")
    append_detection_details "$filename" "$TMP_OUTPUT"

    if [ -n "$max_confidence" ]; then
        person_detected="yes"
    else
        person_detected="no"
        max_confidence=""
    fi

    printf "%s,%s,%s\n" "$filename" "$person_detected" "$max_confidence" >> "$RESULTS_CSV"
done

printf "Done. Results saved to %s\n" "$RESULTS_CSV"
printf "Detailed detections saved to %s\n" "$DETAILS_CSV"
