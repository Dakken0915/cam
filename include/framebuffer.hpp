#ifndef FRAMEBUFFER_HPP
#define FRAMEBUFFER_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include <linux/fb.h>
#include <opencv2/core.hpp>

struct PhotoButton {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct DisplayStats {
    double captureFps = 0.0;
    double fps = 0.0;
    double lcdFps = 0.0;
    double lcdMs = 0.0;
    double jpegMs = 0.0;
};

// Borrowed display memory also supports rendering tests without LCD hardware.
class FramebufferSurface {
public:
    /**
     * @brief  Validate a framebuffer layout and bind its borrowed memory.
     * @param  [in,out] memory Writable storage for the framebuffer surface.
     * @param  [in] length Available storage length in bytes.
     * @param  [in] fixed Driver memory layout and visual information.
     * @param  [in] variable Driver resolution, offsets, and pixel bitfields.
     * @retval true The surface is bound to a supported, valid layout.
     * @retval false The memory bounds or pixel format are invalid.
     * @warning The caller owns the memory and must keep it and the display mode valid until reset().
     */
    bool configure(uint8_t* memory, size_t length, const fb_fix_screeninfo& fixed,
                   const fb_var_screeninfo& variable);
    /**
     * @brief  Scale and center a BGR frame in the visible framebuffer area.
     * @param  [in] bgr Nonempty CV_8UC3 input image.
     * @retval true The frame was written to the configured surface.
     * @retval false No surface is bound or the input image is invalid.
     * @note   Preserves aspect ratio, row padding, and nonvisible pages; OpenCV errors propagate.
     */
    bool render(const cv::Mat& bgr);
    /**
     * @brief  Draw the LCD photo button without replacing the camera frame.
     * @retval true The button was drawn on a configured surface.
     * @retval false The surface is unavailable or too small for a button.
     */
    bool renderPhotoButton();
    /**
     * @brief  Return the configured visible framebuffer dimensions.
     * @retval pair<int, int> Width and height in screen pixels; both are zero when unconfigured.
     */
    std::pair<int, int> screenSize() const;
    /**
     * @brief  Return the LCD photo button rectangle.
     * @retval PhotoButton Screen coordinates and dimensions; zero dimensions mean unavailable.
     */
    PhotoButton photoButton() const;
    /**
     * @brief  Test whether a screen coordinate is inside the photo button.
     * @param  [in] x Horizontal screen coordinate.
     * @param  [in] y Vertical screen coordinate.
     * @retval true The coordinate is inside the button.
     * @retval false The coordinate is outside or the button is unavailable.
     */
    bool isPhotoButton(int x, int y) const;
    /**
     * @brief  Draw performance statistics in the unused LCD side margin.
     * @param  [in] stats Current FPS and processing time values.
     * @retval true The statistics were rendered successfully.
     * @retval false The framebuffer is unavailable or the margin is too small.
     * @note   The overlay is written directly to the framebuffer and is not included in JPEG output.
     */
    bool renderStats(const DisplayStats& stats);
    /**
     * @brief  Unbind borrowed memory and release the scaling cache.
     * @note   Does not free the caller's memory or clear the display.
     */
    void reset();

private:
    friend class FramebufferDisplay;

    /**
     * @brief  Select the framebuffer page used by subsequent drawing operations.
     * @param  [in] page Zero-based page index in the configured virtual framebuffer.
     * @retval true The page is available and selected.
     * @retval false The surface is unconfigured or the page is outside the buffer.
     */
    bool setPage(uint32_t page);

    uint8_t* memory_ = nullptr;
    fb_fix_screeninfo fixed_{};
    fb_var_screeninfo variable_{};
    cv::Mat scaled_;
    uint32_t redTable_[256]{};
    uint32_t greenTable_[256]{};
    uint32_t blueTable_[256]{};
    uint32_t alpha_ = 0;
    int sourceWidth_ = 0;
    int sourceHeight_ = 0;
    std::vector<uint8_t> packedRow_;
    uint32_t pageCount_ = 1;
    uint32_t page_ = 0;
    uint32_t initializedPages_ = 0;

    uint32_t packPixel(uint8_t blue, uint8_t green, uint8_t red) const;
    void drawPhotoButton();
    void writePixel(int x, int y, uint32_t pixel);
};

class FramebufferDisplay {
public:
    /**
     * @brief  Create an unopened framebuffer display object.
     */
    FramebufferDisplay() = default;
    /**
     * @brief  Unmap display memory and close the framebuffer device.
     */
    ~FramebufferDisplay();
    /**
     * @brief  Prevent copying ownership of the display descriptor and mapping.
     */
    FramebufferDisplay(const FramebufferDisplay&) = delete;
    /**
     * @brief  Prevent copy assignment of framebuffer resources.
     */
    FramebufferDisplay& operator=(const FramebufferDisplay&) = delete;

    /**
     * @brief  Open a framebuffer device and map its existing display mode.
     * @param  [in] device Framebuffer device path.
     * @retval true Display memory is mapped and the pixel layout is supported.
     * @retval false Device access, mapping, or layout validation failed.
     * @note   Releases any previous display resources; does not set panel timing or backlight.
     */
    bool open(const std::string& device = "/dev/fb0");
    /**
     * @brief  Render a BGR frame through the bound LCD surface.
     * @param  [in] bgr Nonempty CV_8UC3 input image.
     * @retval true The frame was rendered.
     * @retval false The display is unopened or the input is invalid.
     * @note   OpenCV rendering errors propagate to the caller.
     */
    bool render(const cv::Mat& bgr);
    /**
     * @brief  Return the visible framebuffer dimensions used for touch normalization.
     * @retval pair<int, int> Width and height in screen pixels; both are zero when unopened.
     */
    std::pair<int, int> screenSize() const;
    /**
     * @brief  Return the photo button rectangle in framebuffer coordinates.
     * @retval PhotoButton Current button geometry, or zero dimensions if unavailable.
     */
    PhotoButton photoButton() const;
    /**
     * @brief  Check whether a framebuffer coordinate activates the photo button.
     * @param  [in] x Horizontal framebuffer coordinate.
     * @param  [in] y Vertical framebuffer coordinate.
     * @retval true The coordinate is inside the button.
     * @retval false The coordinate is outside the button or the device is unopened.
     */
    bool isPhotoButton(int x, int y) const;
    /**
     * @brief  Render performance statistics on the LCD overlay area.
     * @param  [in] stats Current display and encoding statistics.
     * @retval true The statistics were rendered.
     * @retval false The framebuffer is not available.
     */
    bool renderStats(const DisplayStats& stats);
    /**
     * @brief  Unbind and unmap display memory, then close the device.
     * @note   Safe to call repeatedly; does not clear the screen or restore its previous contents.
     */
    void close();

private:
    int fd_ = -1;
    uint8_t* memory_ = nullptr;
    size_t length_ = 0;
    FramebufferSurface surface_;
    fb_var_screeninfo variable_{};
    uint32_t baseYOffset_ = 0;
    uint32_t frontPage_ = 0;
    uint32_t backPage_ = 0;
    bool doubleBuffered_ = false;
    int vsyncState_ = -1;

    /**
     * @brief  Present a rendered page through the framebuffer page-flip ioctl.
     * @param  [in] page Zero-based page index to make visible.
     * @retval true The page was presented, or single-buffer fallback was enabled.
     * @retval false The display is unavailable or the page index is invalid.
     */
    bool presentPage(uint32_t page);
};

#endif
