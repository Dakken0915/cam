#ifndef FRAMEBUFFER_HPP
#define FRAMEBUFFER_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <linux/fb.h>
#include <opencv2/core.hpp>

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
     * @brief  Unbind borrowed memory and release the scaling cache.
     * @note   Does not free the caller's memory or clear the display.
     */
    void reset();

private:
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
     * @brief  Unbind and unmap display memory, then close the device.
     * @note   Safe to call repeatedly; does not clear the screen or restore its previous contents.
     */
    void close();

private:
    int fd_ = -1;
    uint8_t* memory_ = nullptr;
    size_t length_ = 0;
    FramebufferSurface surface_;
};

#endif
