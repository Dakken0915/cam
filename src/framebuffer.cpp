#include "framebuffer.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <iostream>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <opencv2/imgproc.hpp>

namespace {

/**
 * @brief  Validate a framebuffer channel and calculate its bit mask.
 * @param  [in] field Channel bitfield.
 * @param  [in] bits Total bits per pixel.
 * @param  [in] required Whether the channel must have a nonzero length.
 * @param  [out] mask Bit mask for a valid channel; zero for an absent optional channel.
 * @retval true The channel fits and uses a supported bit order.
 * @retval false The channel is missing, reversed, too wide, or out of range.
 */
bool channelMask(const fb_bitfield& field, uint32_t bits, bool required,
                 uint32_t& mask) {
    mask = 0;
    if (field.length == 0) return !required;
    if (field.msb_right || field.length > 8 || field.offset >= bits ||
        field.length > bits - field.offset) return false;
    mask = ((1u << field.length) - 1u) << field.offset;
    return true;
}

/**
 * @brief  Quantize an 8-bit component into a validated framebuffer bitfield.
 * @param  [in] value Component intensity.
 * @param  [in] field Previously validated bitfield with a length of at most eight.
 * @retval Component Packed component bits, or zero for an absent channel.
 */
uint32_t packChannel(uint8_t value, const fb_bitfield& field) {
    if (!field.length) return 0;
    return (static_cast<uint32_t>(value) >> (8 - field.length)) << field.offset;
}

} // namespace

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
bool FramebufferSurface::configure(uint8_t* memory, size_t length,
                                   const fb_fix_screeninfo& fixed,
                                   const fb_var_screeninfo& variable) {
    reset();
    const uint32_t bits = variable.bits_per_pixel;
    if (!memory || fixed.type != FB_TYPE_PACKED_PIXELS ||
        fixed.visual != FB_VISUAL_TRUECOLOR || variable.nonstd || variable.grayscale ||
        variable.rotate || (variable.vmode & FB_VMODE_YWRAP) ||
        (bits != 16 && bits != 24 && bits != 32) ||
        !variable.xres || !variable.yres || variable.xres > INT_MAX ||
        variable.yres > INT_MAX) return false;

    const uint64_t right = static_cast<uint64_t>(variable.xoffset) + variable.xres;
    const uint64_t bottom = static_cast<uint64_t>(variable.yoffset) + variable.yres;
    const uint64_t rowBytes = right * (bits / 8);
    const uint64_t end = (bottom - 1) * fixed.line_length + rowBytes;
    if (right > variable.xres_virtual || bottom > variable.yres_virtual ||
        rowBytes > fixed.line_length || end > length || end > fixed.smem_len)
        return false;

    uint32_t r, g, b, a;
    if (!channelMask(variable.red, bits, true, r) ||
        !channelMask(variable.green, bits, true, g) ||
        !channelMask(variable.blue, bits, true, b) ||
        !channelMask(variable.transp, bits, false, a) ||
        (r & g) || (r & b) || (g & b) || ((r | g | b) & a)) return false;

    memory_ = memory;
    fixed_ = fixed;
    variable_ = variable;
    alpha_ = a;
    for (int value = 0; value < 256; ++value) {
        redTable_[value] = packChannel(static_cast<uint8_t>(value), variable.red);
        greenTable_[value] = packChannel(static_cast<uint8_t>(value), variable.green);
        blueTable_[value] = packChannel(static_cast<uint8_t>(value), variable.blue);
    }
    return true;
}

/**
 * @brief  Scale and center a BGR frame in the visible framebuffer area.
 * @param  [in] bgr Nonempty CV_8UC3 input image.
 * @retval true The frame was written to the configured surface.
 * @retval false No surface is bound or the input image is invalid.
 * @note   Preserves aspect ratio, row padding, and nonvisible pages; OpenCV errors propagate.
 */
bool FramebufferSurface::render(const cv::Mat& bgr) {
    if (!memory_ || bgr.empty() || bgr.type() != CV_8UC3) return false;
    const int screenWidth = static_cast<int>(variable_.xres);
    const int screenHeight = static_cast<int>(variable_.yres);
    int width = screenWidth;
    int height = screenHeight;
    if (static_cast<int64_t>(bgr.cols) * screenHeight >
        static_cast<int64_t>(bgr.rows) * screenWidth) {
        height = std::max(1, static_cast<int>(
            static_cast<int64_t>(bgr.rows) * screenWidth / bgr.cols));
    } else {
        width = std::max(1, static_cast<int>(
            static_cast<int64_t>(bgr.cols) * screenHeight / bgr.rows));
    }
    cv::resize(bgr, scaled_, cv::Size(width, height), 0, 0, cv::INTER_NEAREST);
    const size_t bytes = variable_.bits_per_pixel / 8;
    const int left = (screenWidth - width) / 2;
    const int top = (screenHeight - height) / 2;

    // Clear only when geometry changes; row padding and nonvisible pages stay untouched.
    if (sourceWidth_ != bgr.cols || sourceHeight_ != bgr.rows) {
        for (int y = 0; y < screenHeight; ++y) {
            uint8_t* row = memory_ +
                (static_cast<size_t>(variable_.yoffset) + y) * fixed_.line_length +
                static_cast<size_t>(variable_.xoffset) * bytes;
            for (int x = 0; x < screenWidth; ++x) {
                std::memcpy(row + x * bytes, &alpha_, bytes);
            }
        }
        sourceWidth_ = bgr.cols;
        sourceHeight_ = bgr.rows;
    }
    for (int y = 0; y < height; ++y) {
        const cv::Vec3b* source = scaled_.ptr<cv::Vec3b>(y);
        uint8_t* destination = memory_ +
            (static_cast<size_t>(variable_.yoffset) + top + y) * fixed_.line_length +
            (static_cast<size_t>(variable_.xoffset) + left) * bytes;
        for (int x = 0; x < width; ++x) {
            const uint32_t pixel = alpha_ | redTable_[source[x][2]] |
                                   greenTable_[source[x][1]] | blueTable_[source[x][0]];
            std::memcpy(destination + x * bytes, &pixel, bytes);
        }
    }
    return true;
}

/**
 * @brief  Unbind borrowed memory and release the scaling cache.
 * @note   Does not free the caller's memory or clear the display.
 */
void FramebufferSurface::reset() {
    memory_ = nullptr;
    sourceWidth_ = sourceHeight_ = 0;
    scaled_.release();
}

/**
 * @brief  Open a framebuffer device and map its existing display mode.
 * @param  [in] device Framebuffer device path.
 * @retval true Display memory is mapped and the pixel layout is supported.
 * @retval false Device access, mapping, or layout validation failed.
 * @note   Releases any previous display resources; does not set panel timing or backlight.
 */
bool FramebufferDisplay::open(const std::string& device) {
    close();
    fd_ = ::open(device.c_str(), O_RDWR | O_CLOEXEC);
    if (fd_ < 0) {
        std::cerr << "[LCD] Cannot open " << device << ": " << std::strerror(errno) << '\n';
        return false;
    }
    fb_fix_screeninfo fixed{};
    fb_var_screeninfo variable{};
    if (ioctl(fd_, FBIOGET_FSCREENINFO, &fixed) < 0 ||
        ioctl(fd_, FBIOGET_VSCREENINFO, &variable) < 0 || !fixed.smem_len) {
        std::cerr << "[LCD] Cannot read framebuffer information\n";
        close();
        return false;
    }
    length_ = fixed.smem_len;
    void* mapped = mmap(nullptr, length_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (mapped == MAP_FAILED) {
        std::cerr << "[LCD] mmap failed: " << std::strerror(errno) << '\n';
        close();
        return false;
    }
    memory_ = static_cast<uint8_t*>(mapped);
    if (!surface_.configure(memory_, length_, fixed, variable)) {
        std::cerr << "[LCD] Unsupported framebuffer format or invalid memory layout\n";
        close();
        return false;
    }
    std::cout << "[LCD] " << device << ' ' << variable.xres << 'x' << variable.yres
              << ' ' << variable.bits_per_pixel << " bpp, stride=" << fixed.line_length << '\n';
    return true;
}

/**
 * @brief  Render a BGR frame through the bound LCD surface.
 * @param  [in] bgr Nonempty CV_8UC3 input image.
 * @retval true The frame was rendered.
 * @retval false The display is unopened or the input is invalid.
 * @note   OpenCV rendering errors propagate to the caller.
 */
bool FramebufferDisplay::render(const cv::Mat& bgr) {
    return surface_.render(bgr);
}

/**
 * @brief  Unbind and unmap display memory, then close the device.
 * @note   Safe to call repeatedly; does not clear the screen or restore its previous contents.
 */
void FramebufferDisplay::close() {
    surface_.reset();
    if (memory_) munmap(memory_, length_);
    memory_ = nullptr;
    length_ = 0;
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
}

/**
 * @brief  Unmap display memory and close the framebuffer device.
 */
FramebufferDisplay::~FramebufferDisplay() {
    close();
}
