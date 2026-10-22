#ifndef TOUCHSCREEN_HPP
#define TOUCHSCREEN_HPP

#include <cstdint>
#include <string>

struct TouchPoint {
    int x = 0;
    int y = 0;
};

class Touchscreen {
public:
    /**
     * @brief  Create an unopened evdev touchscreen reader.
     */
    Touchscreen() = default;
    /**
     * @brief  Close the input device and release its descriptor.
     */
    ~Touchscreen();
    Touchscreen(const Touchscreen&) = delete;
    Touchscreen& operator=(const Touchscreen&) = delete;

    /**
     * @brief  Open a touchscreen event device and read its absolute ranges.
     * @param  [in] device Explicit event path; an empty path scans /dev/input/event*.
     * @retval true A device with touch keys or tracking IDs and X/Y absolute axes was opened.
     * @retval false No suitable device could be opened or queried.
     * @note   Coordinates are normalized to the display size supplied by setScreenSize().
     */
    bool open(const std::string& device = "");
    /**
     * @brief  Set the output display size used for coordinate normalization.
     * @param  [in] width Display width in pixels.
     * @param  [in] height Display height in pixels.
     */
    void setScreenSize(int width, int height);
    /**
     * @brief  Wait for a touch release and return its normalized screen coordinate.
     * @param  [out] point Released touch coordinate.
     * @param  [in] timeoutMs Poll timeout in milliseconds.
     * @retval true A touch release was received.
     * @retval false The timeout elapsed, the device is closed, or the event was invalid.
     * @note   A tap is reported only when its release point is inside the input range.
     */
    bool waitForTap(TouchPoint& point, int timeoutMs = 250);
    /**
     * @brief  Close the touchscreen event device.
     * @note   Safe to call repeatedly.
     */
    void close();
    /**
     * @brief  Report whether an evdev device is open.
     * @retval true The reader owns a valid descriptor.
     * @retval false No descriptor is open.
     */
    bool isOpen() const;

private:
    int fd_ = -1;
    int minX_ = 0;
    int maxX_ = 1;
    int minY_ = 0;
    int maxY_ = 1;
    int screenWidth_ = 1;
    int screenHeight_ = 1;
    int currentX_ = 0;
    int currentY_ = 0;
    bool touching_ = false;
    bool trackingIdMode_ = false;
    bool xAxisAdjusted_ = false;
    bool yAxisAdjusted_ = false;

    /**
     * @brief  Open one candidate and verify touch keys or tracking IDs with absolute X/Y axes.
     * @param  [in] path Candidate evdev path.
     * @retval true The candidate is a compatible touchscreen.
     * @retval false The candidate is not compatible or cannot be queried.
     */
    bool openCandidate(const std::string& path);
    /**
     * @brief  Convert a raw absolute coordinate to a display coordinate.
     * @param  [in] value Raw axis value.
     * @param  [in] minimum Raw axis minimum.
     * @param  [in] maximum Raw axis maximum.
     * @param  [in] displaySize Output display size.
     * @retval int Clamped coordinate in the range 0..displaySize-1.
     */
    static int normalize(int value, int minimum, int maximum, int displaySize);
};

#endif
