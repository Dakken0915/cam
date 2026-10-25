#!/bin/sh

# Generate the C++ defaults consumed by main.cpp from a Kconfig-style file.
set -eu

config_file=${1:-.config}
output_file=${2:?output header path is required}

get_value() {
    key=$1
    fallback=$2
    value=""
    if [ -f "$config_file" ]; then
        value=$(sed -n "s/^${key}=//p" "$config_file" | tail -n 1 || true)
        if [ -z "$value" ] && grep -q "^# ${key} is not set$" "$config_file"; then
            value=n
        fi
    fi
    value=${value#\"}
    value=${value%\"}
    if [ -z "$value" ]; then
        value=$fallback
    fi
    printf '%s' "$value"
}

get_bool() {
    [ "$(get_value "$1" "$2")" = "y" ] && printf '1' || printf '0'
}

get_choice_value() {
    prefix=$1
    fallback=$2
    shift 2
    while [ "$#" -gt 0 ]; do
        suffix=$1
        selected=$2
        if [ "$(get_value "${prefix}${suffix}" n)" = y ]; then
            printf '%s' "$selected"
            return
        fi
        shift 2
    done
    printf '%s' "$fallback"
}

get_resolution() {
    resolution=$(get_value CONFIG_CAM_RESOLUTION "")
    if [ -n "$resolution" ]; then
        printf '%s' "$resolution"
        return
    fi
    for candidate in 320_240 640_480 800_600 1280_720 1280_1024; do
        if [ "$(get_value CONFIG_CAM_RES_$candidate n)" = y ]; then
            printf '%s' "${candidate%_*}x${candidate#*_}"
            return
        fi
    done
    printf '%sx%s' "$(get_value CONFIG_CAM_CAMERA_WIDTH 640)" "$(get_value CONFIG_CAM_CAMERA_HEIGHT 480)"
}

quote_cpp() {
    printf '%s' "$1" | sed 's/[\\\"]/[\\&]/g'
}

device=$(quote_cpp "$(get_value CONFIG_CAM_CAMERA_DEVICE /dev/video1)")
resolution=$(get_resolution)
width=${resolution%x*}
height=${resolution#*x}
lcd_device=$(quote_cpp "$(get_value CONFIG_CAM_FRAMEBUFFER_DEVICE /dev/fb0)")
touch_device=$(quote_cpp "$(get_value CONFIG_CAM_TOUCH_DEVICE /dev/input/event1)")
lcd_interval=$(get_choice_value CONFIG_CAM_LCD_INTERVAL_ 3 1 1 2 2 3 3 4 4 5 5)
snapshot_dir=$(quote_cpp "$(get_value CONFIG_CAM_SNAPSHOT_DIR /root/photos)")
cascade=$(quote_cpp "$(get_value CONFIG_CAM_FACE_CASCADE assets/haarcascade_frontalface_default.xml)")
format_mjpeg=$(get_bool CONFIG_CAM_FORMAT_MJPEG y)
face_interval=$(get_choice_value CONFIG_CAM_FACE_INTERVAL_ 30 1 1 5 5 10 10 30 30 60 60)
case "$resolution" in
    320x240|640x480|800x600|1280x720|1280x1024) ;;
    *) echo "Unsupported camera resolution: $resolution" >&2; exit 1 ;;
esac
case "$width:$height" in *[!0-9:]*) echo "Invalid camera dimensions" >&2; exit 1;; esac
if [ "$width" -lt 2 ] || [ "$width" -gt 8192 ] || [ $((width % 2)) -ne 0 ] ||
   [ "$height" -lt 1 ] || [ "$height" -gt 8192 ]; then
    echo "Camera width must be even and dimensions must be within 1..8192" >&2
    exit 1
fi

mkdir -p "$(dirname "$output_file")"
{
    printf '%s\n' '#ifndef CAM_GENERATED_CONFIG_HPP' '#define CAM_GENERATED_CONFIG_HPP'
    printf '#define CAM_CONFIG_DEVICE "%s"\n' "$(quote_cpp "$(get_value CONFIG_CAM_CAMERA_DEVICE /dev/video1)")"
    printf '#define CAM_CONFIG_FRAMEBUFFER "%s"\n' "$lcd_device"
    printf '#define CAM_CONFIG_TOUCH_DEVICE "%s"\n' "$touch_device"
    printf '#define CAM_CONFIG_SNAPSHOT_DIR "%s"\n' "$snapshot_dir"
    printf '#define CAM_CONFIG_FACE_CASCADE "%s"\n' "$cascade"
    printf '#define CAM_CONFIG_FORMAT_MJPEG %s\n' "$format_mjpeg"
    printf '#define CAM_CONFIG_WIDTH %s\n' "$width"
    printf '#define CAM_CONFIG_HEIGHT %s\n' "$height"
    printf '#define CAM_CONFIG_LCD_ENABLED %s\n' "$(get_bool CONFIG_CAM_LCD_ENABLED y)"
    printf '#define CAM_CONFIG_TOUCH_ENABLED %s\n' "$(get_bool CONFIG_CAM_TOUCH_ENABLED y)"
    printf '#define CAM_CONFIG_FACE_ENABLED %s\n' "$(get_bool CONFIG_CAM_FACE_ENABLED y)"
    printf '#define CAM_CONFIG_LCD_INTERVAL %s\n' "$lcd_interval"
    printf '#define CAM_CONFIG_JPEG_QUALITY %s\n' "$(get_value CONFIG_CAM_JPEG_QUALITY 80)"
    printf '#define CAM_CONFIG_FACE_INTERVAL %s\n' "$face_interval"
    printf '#define CAM_CONFIG_GRAY_THRESHOLD %s\n' "$(get_value CONFIG_CAM_GRAY_THRESHOLD 75)"
    printf '#define CAM_CONFIG_HTTP_PORT %s\n' "$(get_value CONFIG_CAM_HTTP_PORT 8080)"
    printf '%s\n' '#endif'
} > "$output_file"
