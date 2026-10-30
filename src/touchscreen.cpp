#include "touchscreen.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <iostream>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>
#include "debug.hpp"

namespace {

// Test one bit in an evdev capability bitmap.
bool hasBit(const unsigned long* bitmap, int bit) {
    return (bitmap[static_cast<size_t>(bit) / (sizeof(unsigned long) * 8)] >>
            (bit % (sizeof(unsigned long) * 8))) & 1UL;
}

// Check whether a directory entry has the eventN form.
bool isEventName(const char* name) {
    if (std::strncmp(name, "event", 5) != 0 || name[5] == '\0') return false;
    for (const char* cursor = name + 5; *cursor; ++cursor) {
        if (*cursor < '0' || *cursor > '9') return false;
    }
    return true;
}

} // namespace

Touchscreen::~Touchscreen() {
    close();
}

bool Touchscreen::open(const std::string& device) {
    close();
    if (!device.empty()) {
        if (openCandidate(device)) return true;
        std::cerr << "[Touch] Cannot use " << device
                  << ": expected BTN_TOUCH/BTN_LEFT or ABS_MT_TRACKING_ID with absolute X/Y\n";
        return false;
    }

    DIR* directory = opendir("/dev/input");
    if (!directory) return false;
    std::vector<std::string> candidates;
    while (dirent* entry = readdir(directory)) {
        if (isEventName(entry->d_name)) candidates.emplace_back("/dev/input/" + std::string(entry->d_name));
    }
    closedir(directory);
    std::sort(candidates.begin(), candidates.end());
    for (const std::string& candidate : candidates) {
        if (openCandidate(candidate)) return true;
    }
    return false;
}

bool Touchscreen::openCandidate(const std::string& path) {
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (descriptor < 0) return false;
    unsigned long keyBits[(KEY_MAX + 1 + sizeof(unsigned long) * 8 - 1) /
                          (sizeof(unsigned long) * 8)]{};
    unsigned long absBits[(ABS_MAX + 1 + sizeof(unsigned long) * 8 - 1) /
                          (sizeof(unsigned long) * 8)]{};
    if (ioctl(descriptor, EVIOCGBIT(EV_KEY, sizeof(keyBits)), keyBits) < 0 ||
        ioctl(descriptor, EVIOCGBIT(EV_ABS, sizeof(absBits)), absBits) < 0) {
        ::close(descriptor);
        return false;
    }
    const bool hasTouchKey = hasBit(keyBits, BTN_TOUCH) || hasBit(keyBits, BTN_LEFT);
    const bool hasTrackingId = hasBit(absBits, ABS_MT_TRACKING_ID);
    if ((!hasTouchKey && !hasTrackingId) ||
        !(hasBit(absBits, ABS_X) || hasBit(absBits, ABS_MT_POSITION_X)) ||
        !(hasBit(absBits, ABS_Y) || hasBit(absBits, ABS_MT_POSITION_Y))) {
        ::close(descriptor);
        return false;
    }
    // Prefer multitouch axes when both axis families are advertised; some BSPs
    // leave legacy ABS_X/ABS_Y ranges at 800x480 while reporting 1024x600 data
    // through ABS_MT_POSITION_X/ABS_MT_POSITION_Y.
    const int xCode = hasBit(absBits, ABS_MT_POSITION_X) ? ABS_MT_POSITION_X : ABS_X;
    const int yCode = hasBit(absBits, ABS_MT_POSITION_Y) ? ABS_MT_POSITION_Y : ABS_Y;
    input_absinfo xInfo{};
    input_absinfo yInfo{};
    if (ioctl(descriptor, EVIOCGABS(xCode), &xInfo) < 0 ||
        ioctl(descriptor, EVIOCGABS(yCode), &yInfo) < 0 || xInfo.maximum <= xInfo.minimum ||
        yInfo.maximum <= yInfo.minimum) {
        ::close(descriptor);
        return false;
    }
    fd_ = descriptor;
    minX_ = xInfo.minimum;
    maxX_ = xInfo.maximum;
    minY_ = yInfo.minimum;
    maxY_ = yInfo.maximum;
    currentX_ = minX_;
    currentY_ = minY_;
    touching_ = false;
    trackingIdMode_ = !hasTouchKey && hasTrackingId;
    xAxisAdjusted_ = false;
    yAxisAdjusted_ = false;
    std::cout << "[Touch] Opened " << path << " range=" << minX_ << ".." << maxX_
              << "," << minY_ << ".." << maxY_
              << (trackingIdMode_ ? " mode=tracking-id" : " mode=touch-key") << '\n';
    return true;
}

void Touchscreen::setScreenSize(int width, int height) {
    screenWidth_ = std::max(1, width);
    screenHeight_ = std::max(1, height);
}

int Touchscreen::normalize(int value, int minimum, int maximum, int displaySize) {
    if (maximum <= minimum || displaySize <= 1) return 0;
    const int64_t clamped = std::max<int64_t>(minimum, std::min<int64_t>(maximum, value));
    return static_cast<int>((clamped - minimum) * (displaySize - 1) /
                            (maximum - minimum));
}

bool Touchscreen::waitForTap(TouchPoint& point, int timeoutMs) {
    if (fd_ < 0 || timeoutMs < 0) return false;
    pollfd descriptor{fd_, POLLIN, 0};
    const int ready = poll(&descriptor, 1, timeoutMs);
    if (ready <= 0 || !(descriptor.revents & POLLIN)) return false;
    input_event events[16];
    const ssize_t bytes = read(fd_, events, sizeof(events));
    if (bytes <= 0 || bytes % static_cast<ssize_t>(sizeof(input_event)) != 0) return false;
    const size_t count = static_cast<size_t>(bytes) / sizeof(input_event);
    for (size_t index = 0; index < count; ++index) {
        const input_event& event = events[index];
        if (event.type == EV_ABS) {
            if (event.code == ABS_X || event.code == ABS_MT_POSITION_X) {
                if (event.value > maxX_ && screenWidth_ > 1) {
                    if (!xAxisAdjusted_) {
                        CAM_DEBUG("[Touch] X raw value " << event.value
                                  << " exceeds advertised max; using screen max "
                                  << screenWidth_ - 1);
                        xAxisAdjusted_ = true;
                    }
                    maxX_ = screenWidth_ - 1;
                }
                currentX_ = event.value;
            }
            if (event.code == ABS_Y || event.code == ABS_MT_POSITION_Y) {
                if (event.value > maxY_ && screenHeight_ > 1) {
                    if (!yAxisAdjusted_) {
                        CAM_DEBUG("[Touch] Y raw value " << event.value
                                  << " exceeds advertised max; using screen max "
                                  << screenHeight_ - 1);
                        yAxisAdjusted_ = true;
                    }
                    maxY_ = screenHeight_ - 1;
                }
                currentY_ = event.value;
            }
            if (trackingIdMode_ && event.code == ABS_MT_TRACKING_ID) {
                if (event.value >= 0) touching_ = true;
                if (event.value < 0 && touching_) {
                    point.x = normalize(currentX_, minX_, maxX_, screenWidth_);
                    point.y = normalize(currentY_, minY_, maxY_, screenHeight_);
                    touching_ = false;
                    CAM_DEBUG("[Touch] Release raw=" << currentX_ << ',' << currentY_
                              << " screen=" << point.x << ',' << point.y);
                    return true;
                }
            }
        } else if (event.type == EV_KEY &&
                   (event.code == BTN_TOUCH || event.code == BTN_LEFT)) {
            if (event.value != 0) touching_ = true;
            if (event.value == 0 && touching_) {
                point.x = normalize(currentX_, minX_, maxX_, screenWidth_);
                point.y = normalize(currentY_, minY_, maxY_, screenHeight_);
                touching_ = false;
                CAM_DEBUG("[Touch] Release raw=" << currentX_ << ',' << currentY_
                          << " screen=" << point.x << ',' << point.y);
                return true;
            }
        }
    }
    return false;
}

void Touchscreen::close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    touching_ = false;
    trackingIdMode_ = false;
    xAxisAdjusted_ = false;
    yAxisAdjusted_ = false;
}

bool Touchscreen::isOpen() const {
    return fd_ >= 0;
}
