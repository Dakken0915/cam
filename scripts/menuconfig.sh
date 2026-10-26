#!/bin/sh

# Small host-side menuconfig frontend for this user-space application.
set -eu

config_file=${1:-.config}
if ! command -v whiptail >/dev/null 2>&1; then
    echo "menuconfig requires whiptail (ncurses)" >&2
    exit 1
fi

# Match the blue ncurses palette used by the Linux kernel menuconfig UI.
NEWT_COLORS='root=white,blue border=white,blue window=white,blue shadow=black,black title=white,blue button=black,cyan actbutton=white,blue compactbutton=white,blue checkbox=white,blue actcheckbox=black,cyan entry=white,blue disentry=gray,blue label=white,blue listbox=white,blue actlistbox=white,cyan actsellistbox=white,cyan textbox=white,blue acttextbox=white,cyan helpline=white,blue roottext=white,blue emptyscale=blue,blue fullscale=cyan,blue'
export NEWT_COLORS

get_value() {
    key=$1
    fallback=$2
    case "$key" in
        CONFIG_*) full_key=$key ;;
        *) full_key=CONFIG_$key ;;
    esac
    value=""
    if [ -f "$config_file" ]; then
        value=$(sed -n "s/^${full_key}=//p" "$config_file" | tail -n 1 || true)
        if [ -z "$value" ] && grep -q "^# ${full_key} is not set$" "$config_file"; then
            value=n
        fi
    fi
    value=${value#\"}
    value=${value%\"}
    [ -n "$value" ] && printf '%s' "$value" || printf '%s' "$fallback"
}

# @brief Display a selectable menu and return the selected value.
# @param prompt Menu title and description.
# @param current Value highlighted when the menu opens.
# @param options Pairs of value and human-readable label.
# @retval 0 A value was selected and printed.
# @retval 1 The user cancelled the menu.
menu_choice() {
    prompt=$1
    current=$2
    shift 2
    whiptail --title "cam menuconfig" --default-item "$current" \
        --menu "$prompt" 20 78 12 "$@" 3>&1 1>&2 2>&3
}

# @brief Edit a string value in a focused input dialog.
# @param prompt Input description.
# @param current Existing string value.
# @retval 0 The edited value was printed.
# @retval 1 The user cancelled the dialog.
edit_string() {
    prompt=$1
    current=$2
    whiptail --title "cam menuconfig" --inputbox "$prompt" 10 76 "$current" \
        3>&1 1>&2 2>&3
}

# @brief Replace a shell variable only after a menu selection succeeds.
# @param variable Variable name to update.
# @param prompt Menu prompt.
# @param current Current selected value.
# @param options Pairs of menu values and labels.
update_choice() {
    variable=$1
    prompt=$2
    current=$3
    shift 3
    if selected=$(menu_choice "$prompt" "$current" "$@"); then
        set_value "$variable" "$selected"
    fi
}

# @brief Replace a shell variable only after a string dialog is accepted.
# @param variable Variable name to update.
# @param prompt Input prompt.
# @param current Current string value.
update_string() {
    variable=$1
    prompt=$2
    current=$3
    if selected=$(edit_string "$prompt" "$current"); then
        set_value "$variable" "$selected"
    fi
}

# @brief Assign a menu value to one of the known configuration variables.
# @param variable Controlled shell variable name.
# @param value New value.
set_value() {
    variable=$1
    value=$2
    case "$variable" in
        format) format=$value ;;
        resolution) resolution=$value ;;
        camera_device) camera_device=$value ;;
        frame_rate) frame_rate=$value ;;
        exposure_auto_priority) exposure_auto_priority=$value ;;
        lcd_enabled) lcd_enabled=$value ;;
        lcd_interval) lcd_interval=$value ;;
        framebuffer_device) framebuffer_device=$value ;;
        touch_enabled) touch_enabled=$value ;;
        touch_device) touch_device=$value ;;
        face_enabled) face_enabled=$value ;;
        jpeg_quality) jpeg_quality=$value ;;
        face_interval) face_interval=$value ;;
        gray_threshold) gray_threshold=$value ;;
        http_port) http_port=$value ;;
        snapshot_dir) snapshot_dir=$value ;;
        face_cascade) face_cascade=$value ;;
        *) return 1 ;;
    esac
}

if [ -n "${MCONF:-}" ]; then
    mconf_bin=$MCONF
elif command -v mconf >/dev/null 2>&1; then
    mconf_bin=$(command -v mconf)
elif command -v kconfig-mconf >/dev/null 2>&1; then
    mconf_bin=$(command -v kconfig-mconf)
else
    mconf_bin=
fi

if [ -n "$mconf_bin" ]; then
    KCONFIG_CONFIG=$config_file "$mconf_bin" config/Kconfig
    exit 0
fi

if [ "$(get_value CONFIG_CAM_FORMAT_MJPEG n)" = y ]; then
    format=y
else
    format=n
fi
resolution=$(get_value CONFIG_CAM_RESOLUTION "$(get_value CONFIG_CAM_CAMERA_WIDTH 640)x$(get_value CONFIG_CAM_CAMERA_HEIGHT 480)")
case "$resolution" in 320x240|640x480|800x600|1280x720|1280x1024) ;; *) resolution=640x480 ;; esac
camera_device=$(get_value CONFIG_CAM_CAMERA_DEVICE /dev/video1)
lcd_enabled=$(get_value CONFIG_CAM_LCD_ENABLED y)
framebuffer_device=$(get_value CONFIG_CAM_FRAMEBUFFER_DEVICE /dev/fb0)
lcd_interval=$(get_value CONFIG_CAM_LCD_INTERVAL 3)
touch_enabled=$(get_value CONFIG_CAM_TOUCH_ENABLED y)
touch_device=$(get_value CONFIG_CAM_TOUCH_DEVICE /dev/input/event1)
face_enabled=$(get_value CONFIG_CAM_FACE_ENABLED y)
jpeg_quality=$(get_value CONFIG_CAM_JPEG_QUALITY 80)
face_interval=$(get_value CONFIG_CAM_FACE_INTERVAL 30)
gray_threshold=$(get_value CONFIG_CAM_GRAY_THRESHOLD 75)
http_port=$(get_value CONFIG_CAM_HTTP_PORT 8080)
snapshot_dir=$(get_value CONFIG_CAM_SNAPSHOT_DIR /root/photos)
face_cascade=$(get_value CONFIG_CAM_FACE_CASCADE assets/haarcascade_frontalface_default.xml)

read_choice() {
    prefix=$1
    fallback=$2
    shift 2
    while [ "$#" -gt 0 ]; do
        candidate=$1
        if [ "$(get_value "${prefix}${candidate}" n)" = y ]; then
            printf '%s' "$candidate"
            return
        fi
        shift
    done
    printf '%s' "$fallback"
}

frame_rate=$(read_choice CONFIG_CAM_FPS_ 30 10 20 30)
exposure_auto_priority=$(get_value CONFIG_CAM_EXPOSURE_AUTO_PRIORITY n)
lcd_interval=$(read_choice CONFIG_CAM_LCD_INTERVAL_ 3 1 2 3 4 5)
case "$jpeg_quality" in 50|60|70|80|90|100) ;; *) jpeg_quality=80 ;; esac
face_interval=$(read_choice CONFIG_CAM_FACE_INTERVAL_ 30 1 5 10 30 60)
case "$gray_threshold" in 50|60|70|75|80|90) ;; *) gray_threshold=75 ;; esac
case "$http_port" in 80|8000|8080|8888) ;; *) http_port=8080 ;; esac

camera_menu() {
    while choice=$(menu_choice "Camera input" "format" \
        format "Pixel format: $format" frame_rate "Frame rate: ${frame_rate} FPS" \
        exposure_auto_priority "Auto exposure may lower FPS: $exposure_auto_priority" \
        resolution "Resolution: $resolution" \
        device "Video device: $camera_device" back "Back"); do
        case "$choice" in
            format) update_choice format "Camera pixel format" "$format" y "MJPEG (camera compressed)" n "YUYV (raw)" ;;
            frame_rate) update_choice frame_rate "Requested camera frame rate" "$frame_rate" \
                10 "10 FPS" 20 "20 FPS" 30 "30 FPS" ;;
            exposure_auto_priority) update_choice exposure_auto_priority \
                "Allow auto exposure to reduce frame rate in low light" "$exposure_auto_priority" \
                n "Disabled: keep requested frame rate" y "Enabled: prioritize image brightness" ;;
            resolution) update_choice resolution "Camera resolution" "$resolution" \
                320x240 "320 x 240" 640x480 "640 x 480" 800x600 "800 x 600" \
                1280x720 "1280 x 720" 1280x1024 "1280 x 1024" ;;
            device) update_string camera_device "Video device" "$camera_device" ;;
            back) return 0 ;;
        esac
    done
}

display_menu() {
    while choice=$(menu_choice "LCD and touch" "lcd" \
        lcd "LCD: $lcd_enabled" interval "LCD interval: $lcd_interval" \
        framebuffer "Framebuffer: $framebuffer_device" touch "Touch: $touch_enabled" \
        touch_device "Touch device: $touch_device" back "Back"); do
        case "$choice" in
            lcd) update_choice lcd_enabled "Enable LCD by default" "$lcd_enabled" y Enabled n Disabled ;;
            interval) update_choice lcd_interval "LCD refresh interval" "$lcd_interval" \
                1 "Every frame" 2 "Every 2 frames" 3 "Every 3 frames" 4 "Every 4 frames" 5 "Every 5 frames" ;;
            framebuffer) update_string framebuffer_device "Framebuffer device" "$framebuffer_device" ;;
            touch) update_choice touch_enabled "Enable touch capture button" "$touch_enabled" y Enabled n Disabled ;;
            touch_device) update_string touch_device "Touch event device" "$touch_device" ;;
            back) return 0 ;;
        esac
    done
}

processing_menu() {
    while choice=$(menu_choice "Processing and output" "face" \
        face "Face analysis: $face_enabled" quality "JPEG quality: $jpeg_quality" \
        face_interval "Face interval: $face_interval" threshold "Gray threshold: $gray_threshold" \
        port "HTTP port: $http_port" snapshot "Snapshot directory: $snapshot_dir" \
        cascade "Haar cascade: $face_cascade" back "Back"); do
        case "$choice" in
            face) update_choice face_enabled "Enable face analysis" "$face_enabled" y Enabled n Disabled ;;
            quality) update_choice jpeg_quality "Software JPEG quality" "$jpeg_quality" \
                50 "50" 60 "60" 70 "70" 80 "80" 90 "90" 100 "100" ;;
            face_interval) update_choice face_interval "Face analysis interval" "$face_interval" \
                1 "Every frame" 5 "Every 5 frames" 10 "Every 10 frames" 30 "Every 30 frames" 60 "Every 60 frames" ;;
            threshold) update_choice gray_threshold "Grayscale match threshold" "$gray_threshold" \
                50 "50%" 60 "60%" 70 "70%" 75 "75%" 80 "80%" 90 "90%" ;;
            port) update_choice http_port "HTTP port" "$http_port" 80 "80" 8000 "8000" 8080 "8080" 8888 "8888" ;;
            snapshot) update_string snapshot_dir "Snapshot directory" "$snapshot_dir" ;;
            cascade) update_string face_cascade "Haar cascade path" "$face_cascade" ;;
            back) return 0 ;;
        esac
    done
}

write_choice() {
    prefix=$1
    selected=$2
    shift 2
    while [ "$#" -gt 0 ]; do
        candidate=$1
        if [ "$selected" = "$candidate" ]; then
            printf 'CONFIG_%s_%s=y\n' "$prefix" "$candidate"
        else
            printf '# CONFIG_%s_%s is not set\n' "$prefix" "$candidate"
        fi
        shift
    done
}

write_config() {
    tmp=$(mktemp "${config_file}.tmp.XXXXXX")
    trap 'rm -f "$tmp"' EXIT INT TERM
    width=${resolution%x*}
    height=${resolution#*x}
    {
        printf '# Generated by make menuconfig. Edit with make menuconfig.\n'
        [ "$format" = y ] && printf 'CONFIG_CAM_FORMAT_MJPEG=y\n' || printf '# CONFIG_CAM_FORMAT_MJPEG is not set\n'
        [ "$format" = n ] && printf 'CONFIG_CAM_FORMAT_YUYV=y\n' || printf '# CONFIG_CAM_FORMAT_YUYV is not set\n'
        printf 'CONFIG_CAM_RESOLUTION=%s\n' "$resolution"
        printf 'CONFIG_CAM_CAMERA_WIDTH=%s\nCONFIG_CAM_CAMERA_HEIGHT=%s\n' "$width" "$height"
        printf 'CONFIG_CAM_CAMERA_DEVICE="%s"\n' "$camera_device"
        write_choice CAM_FPS "$frame_rate" 10 20 30
        [ "$exposure_auto_priority" = y ] && printf 'CONFIG_CAM_EXPOSURE_AUTO_PRIORITY=y\n' || printf '# CONFIG_CAM_EXPOSURE_AUTO_PRIORITY is not set\n'
        [ "$lcd_enabled" = y ] && printf 'CONFIG_CAM_LCD_ENABLED=y\n' || printf '# CONFIG_CAM_LCD_ENABLED is not set\n'
        printf 'CONFIG_CAM_FRAMEBUFFER_DEVICE="%s"\n' "$framebuffer_device"
        write_choice CAM_LCD_INTERVAL "$lcd_interval" 1 2 3 4 5
        [ "$touch_enabled" = y ] && printf 'CONFIG_CAM_TOUCH_ENABLED=y\n' || printf '# CONFIG_CAM_TOUCH_ENABLED is not set\n'
        printf 'CONFIG_CAM_TOUCH_DEVICE="%s"\n' "$touch_device"
        [ "$face_enabled" = y ] && printf 'CONFIG_CAM_FACE_ENABLED=y\n' || printf '# CONFIG_CAM_FACE_ENABLED is not set\n'
        printf 'CONFIG_CAM_JPEG_QUALITY=%s\n' "$jpeg_quality"
        write_choice CAM_FACE_INTERVAL "$face_interval" 1 5 10 30 60
        printf 'CONFIG_CAM_GRAY_THRESHOLD=%s\nCONFIG_CAM_HTTP_PORT=%s\n' "$gray_threshold" "$http_port"
        printf 'CONFIG_CAM_SNAPSHOT_DIR="%s"\nCONFIG_CAM_FACE_CASCADE="%s"\n' "$snapshot_dir" "$face_cascade"
    } > "$tmp"
    mv "$tmp" "$config_file"
    trap - EXIT INT TERM
    echo "Configuration saved to $config_file"
}

while choice=$(menu_choice "Main menu" "camera" \
    camera "Camera input" display "LCD and touch" processing "Processing and output" \
    save "Save and exit" cancel "Exit without saving"); do
    case "$choice" in
        camera) camera_menu ;;
        display) display_menu ;;
        processing) processing_menu ;;
        save) write_config; exit 0 ;;
        cancel) exit 0 ;;
    esac
done
