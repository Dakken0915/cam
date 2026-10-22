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
    drawPhotoButton();
    return true;
}

/**
 * @brief  Draw the LCD photo button without replacing the camera frame.
 * @retval true The button was drawn on a configured surface.
 * @retval false The surface is unavailable or too small for a button.
 */
bool FramebufferSurface::renderPhotoButton() {
    const PhotoButton button = photoButton();
    if (button.width <= 0 || button.height <= 0) return false;
    drawPhotoButton();
    return true;
}

/**
 * @brief  Return a fixed-size photo button anchored at the lower-right corner.
 * @retval PhotoButton Button geometry in visible screen coordinates.
 * @note   Very small test surfaces do not expose a button.
 */
PhotoButton FramebufferSurface::photoButton() const {
    const int screenWidth = static_cast<int>(variable_.xres);
    const int screenHeight = static_cast<int>(variable_.yres);
    if (!memory_ || screenWidth < 80 || screenHeight < 40) return {};
    PhotoButton button;
    button.width = std::min(220, screenWidth - 20);
    button.height = std::min(80, screenHeight - 20);
    button.x = screenWidth - button.width - 20;
    button.y = screenHeight - button.height - 20;
    return button;
}

/**
 * @brief  Check whether a screen point falls inside the photo button.
 * @param  [in] x Horizontal screen coordinate.
 * @param  [in] y Vertical screen coordinate.
 * @retval true The point is within the button rectangle.
 * @retval false The point is outside or the surface has no button.
 */
bool FramebufferSurface::isPhotoButton(int x, int y) const {
    const PhotoButton button = photoButton();
    return button.width > 0 && x >= button.x && x < button.x + button.width &&
           y >= button.y && y < button.y + button.height;
}

/**
 * @brief  Pack an RGB color into the configured framebuffer pixel format.
 * @param  [in] blue Blue channel intensity.
 * @param  [in] green Green channel intensity.
 * @param  [in] red Red channel intensity.
 * @retval uint32_t Packed pixel value.
 */
uint32_t FramebufferSurface::packPixel(uint8_t blue, uint8_t green, uint8_t red) const {
    return alpha_ | redTable_[red] | greenTable_[green] | blueTable_[blue];
}

/**
 * @brief  Write one packed pixel inside the visible framebuffer area.
 * @param  [in] x Horizontal visible coordinate.
 * @param  [in] y Vertical visible coordinate.
 * @param  [in] pixel Packed framebuffer pixel.
 */
void FramebufferSurface::writePixel(int x, int y, uint32_t pixel) {
    const PhotoButton screen = {0, 0, static_cast<int>(variable_.xres),
                                static_cast<int>(variable_.yres)};
    if (!memory_ || x < screen.x || y < screen.y || x >= screen.width || y >= screen.height)
        return;
    const size_t bytes = variable_.bits_per_pixel / 8;
    uint8_t* destination = memory_ +
        (static_cast<size_t>(variable_.yoffset) + y) * fixed_.line_length +
        (static_cast<size_t>(variable_.xoffset) + x) * bytes;
    std::memcpy(destination, &pixel, bytes);
}

/**
 * @brief  Draw one bitmap letter using a compact five-by-seven font.
 * @param  [in] glyph Letter to draw; PHOTO letters are supported.
 * @param  [in] x Left coordinate of the glyph.
 * @param  [in] y Top coordinate of the glyph.
 * @param  [in] scale Pixel scale for the glyph bitmap.
 * @param  [in] color Packed framebuffer color.
 * @note   Unsupported letters are ignored so the button remains usable.
 */
void FramebufferSurface::drawGlyph(char glyph, int x, int y, int scale, uint32_t color) {
    static const uint8_t p[] = {0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10};
    static const uint8_t h[] = {0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11};
    static const uint8_t o[] = {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e};
    static const uint8_t t[] = {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
    const uint8_t* rows = nullptr;
    switch (glyph) {
    case 'P': rows = p; break;
    case 'H': rows = h; break;
    case 'O': rows = o; break;
    case 'T': rows = t; break;
    default: return;
    }
    if (scale <= 0) return;
    for (int row = 0; row < 7; ++row) {
        for (int column = 0; column < 5; ++column) {
            if (!(rows[row] & (1u << (4 - column)))) continue;
            for (int yy = 0; yy < scale; ++yy) {
                for (int xx = 0; xx < scale; ++xx)
                    writePixel(x + column * scale + xx, y + row * scale + yy, color);
            }
        }
    }
}

/**
 * @brief  Draw a colored camera icon and PHOTO button over the current LCD frame.
 * @note   The overlay is written only to the framebuffer and is not included in JPEG output.
 */
void FramebufferSurface::drawPhotoButton() {
    const PhotoButton button = photoButton();
    if (button.width <= 0) return;
    const uint32_t background = packPixel(24, 105, 168);
    const uint32_t border = packPixel(255, 255, 255);
    const uint32_t lens = packPixel(20, 20, 20);
    for (int y = button.y; y < button.y + button.height; ++y) {
        for (int x = button.x; x < button.x + button.width; ++x) {
            const bool edge = x == button.x || y == button.y ||
                              x == button.x + button.width - 1 ||
                              y == button.y + button.height - 1;
            writePixel(x, y, edge ? border : background);
        }
    }
    const char* label = "PHOTO";
    const bool showLabel = button.width >= 160;
    const int scale = button.width >= 200 ? 3 : 2;
    const int labelWidth = 5 * 6 * scale;
    const int iconCenterX = showLabel ? button.x + button.width / 4 :
                            button.x + button.width / 2;
    const int iconX = iconCenterX - 30;
    const int iconY = button.y + button.height / 2 - 18;
    for (int y = iconY; y < iconY + 36; ++y) {
        for (int x = iconX; x < iconX + 60; ++x) {
            const bool body = x >= iconX && x < iconX + 60 && y >= iconY + 8 && y < iconY + 34;
            const bool top = x >= iconX + 12 && x < iconX + 28 && y >= iconY && y < iconY + 10;
            if (body || top) writePixel(x, y, border);
        }
    }
    const int centerX = iconX + 30;
    const int centerY = iconY + 21;
    for (int y = centerY - 10; y <= centerY + 10; ++y) {
        for (int x = centerX - 10; x <= centerX + 10; ++x) {
            if ((x - centerX) * (x - centerX) + (y - centerY) * (y - centerY) <= 100)
                writePixel(x, y, lens);
        }
    }
    if (showLabel) {
        int textX = button.x + button.width - labelWidth - 8;
        const int textY = button.y + (button.height - 7 * scale) / 2;
        for (int index = 0; label[index] != '\0'; ++index) {
            drawGlyph(label[index], textX, textY, scale, border);
            textX += 6 * scale;
        }
    }
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
    const PhotoButton button = surface_.photoButton();
    std::cout << "[LCD] PHOTO button x=" << button.x << " y=" << button.y
              << " w=" << button.width << " h=" << button.height << '\n';
    surface_.renderPhotoButton();
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
 * @brief  Return the photo button rectangle in framebuffer coordinates.
 * @retval PhotoButton Current button geometry, or zero dimensions if unavailable.
 */
PhotoButton FramebufferDisplay::photoButton() const {
    return surface_.photoButton();
}

/**
 * @brief  Check whether a framebuffer coordinate activates the photo button.
 * @param  [in] x Horizontal framebuffer coordinate.
 * @param  [in] y Vertical framebuffer coordinate.
 * @retval true The coordinate is inside the button.
 * @retval false The coordinate is outside the button or the device is unopened.
 */
bool FramebufferDisplay::isPhotoButton(int x, int y) const {
    return surface_.isPhotoButton(x, y);
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
