#include "framebuffer.hpp"
#include "jpeg_encoder.hpp"
#include "bounded_queue.hpp"

#include <cstring>
#include <future>
#include <iostream>
#include <stdexcept>
#include <vector>

/**
 * @brief  Throw a test failure when an assertion is false.
 */
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

/**
 * @brief  Check framebuffer pixel formats, borders, offsets, padding, and memory guards.
 */
void testSurface(int bits, bool swapRedBlue = false) {
    const size_t bytes = bits / 8;
    fb_var_screeninfo variable{};
    variable.xres = 8;
    variable.yres = 4;
    variable.xres_virtual = 10;
    variable.yres_virtual = 6;
    variable.xoffset = 1;
    variable.yoffset = 1;
    variable.bits_per_pixel = bits;
    variable.red = fb_bitfield{static_cast<uint32_t>(bits == 16 ? 11 : 16),
                               static_cast<uint32_t>(bits == 16 ? 5 : 8), 0};
    variable.green = fb_bitfield{static_cast<uint32_t>(bits == 16 ? 5 : 8),
                                 static_cast<uint32_t>(bits == 16 ? 6 : 8), 0};
    variable.blue = fb_bitfield{0, static_cast<uint32_t>(bits == 16 ? 5 : 8), 0};
    if (bits == 32) variable.transp = fb_bitfield{24, 8, 0};
    if (swapRedBlue) std::swap(variable.red, variable.blue);
    fb_fix_screeninfo fixed{};
    fixed.type = FB_TYPE_PACKED_PIXELS;
    fixed.visual = FB_VISUAL_TRUECOLOR;
    fixed.line_length = 10 * bytes + 3;
    fixed.smem_len = fixed.line_length * 6;
    std::vector<uint8_t> memory(fixed.smem_len + 16, 0xa5);
    FramebufferSurface surface;
    require(surface.configure(memory.data(), fixed.smem_len, fixed, variable), "valid surface rejected");
    cv::Mat red(2, 2, CV_8UC3, cv::Scalar(0, 0, 255));
    require(surface.render(red), "render failed");
    const uint32_t alpha = bits == 32 ? 0xff000000u : 0;
    const uint32_t redPixel = alpha | (((1u << variable.red.length) - 1) << variable.red.offset);
    for (size_t offset = 0; offset < memory.size(); ++offset) {
        const size_t row = offset / fixed.line_length;
        const size_t colByte = offset % fixed.line_length;
        uint8_t expected = 0xa5;
        if (row >= 1 && row < 5 && colByte >= bytes && colByte < 9 * bytes) {
            const size_t x = colByte / bytes - 1;
            const uint32_t value = x >= 2 && x < 6 ? redPixel : alpha;
            expected = reinterpret_cast<const uint8_t*>(&value)[colByte % bytes];
        }
        require(memory[offset] == expected, "pixel, border, padding or memory guard mismatch");
    }
    cv::Mat green(1, 2, CV_8UC3, cv::Scalar(0, 255, 0));
    require(surface.render(green), "geometry change failed");
    require(!surface.render(cv::Mat()), "empty input accepted");
    variable.red.offset = 32;
    require(!surface.configure(memory.data(), fixed.smem_len, fixed, variable), "invalid bitfield accepted");
    variable.red.offset = bits == 16 ? 11 : 16;
    require(!surface.configure(memory.data(), 2, fixed, variable), "short memory accepted");
    fixed.visual = FB_VISUAL_DIRECTCOLOR;
    require(!surface.configure(memory.data(), fixed.smem_len, fixed, variable), "unsupported visual accepted");
}

/**
 * @brief  Verify that the LCD photo button is drawn immediately and has a usable hit area.
 */
void testPhotoButton() {
    fb_var_screeninfo variable{};
    variable.xres = 240;
    variable.yres = 120;
    variable.xres_virtual = variable.xres;
    variable.yres_virtual = variable.yres;
    variable.bits_per_pixel = 32;
    variable.red = fb_bitfield{16, 8, 0};
    variable.green = fb_bitfield{8, 8, 0};
    variable.blue = fb_bitfield{0, 8, 0};
    variable.transp = fb_bitfield{24, 8, 0};
    fb_fix_screeninfo fixed{};
    fixed.type = FB_TYPE_PACKED_PIXELS;
    fixed.visual = FB_VISUAL_TRUECOLOR;
    fixed.line_length = variable.xres * 4;
    fixed.smem_len = fixed.line_length * variable.yres;
    std::vector<uint8_t> memory(fixed.smem_len, 0);
    FramebufferSurface surface;
    require(surface.configure(memory.data(), memory.size(), fixed, variable),
            "photo button surface rejected");
    require(surface.renderPhotoButton(), "photo button was not drawn");
    const PhotoButton button = surface.photoButton();
    require(button.x == 136 && button.y == 16 && button.width == 80 && button.height == 80,
            "unexpected photo button geometry");
    require(surface.isPhotoButton(button.x + button.width / 2, button.y + button.height / 2),
            "photo button hit test rejected an interior point");
    require(!surface.isPhotoButton(button.x, button.y),
            "photo button hit test accepted an exterior point");
    uint32_t topLeft = 0;
    const size_t topCenter = static_cast<size_t>(button.y + 1) * fixed.line_length +
                             static_cast<size_t>(button.x + button.width / 2) * sizeof(uint32_t);
    std::memcpy(&topLeft, memory.data() + topCenter,
                sizeof(topLeft));
    require(topLeft == 0xffffffffu, "photo button border was not rendered");
}

/**
 * @brief  Check padded YUYV conversion, JPEG decoding, and invalid encoder inputs.
 */
void testEncoder() {
    JpegEncoder encoder;
    const std::vector<uint8_t> yuyv{16, 128, 16, 128, 0xa5, 0xa5, 0xa5, 0xa5,
                                   235, 128, 235, 128};
    const cv::Mat& bgr = encoder.convertToBgr(yuyv.data(), 2, 2, 8);
    require(bgr.at<cv::Vec3b>(0, 0) == cv::Vec3b(0, 0, 0), "black YUYV conversion failed");
    require(bgr.at<cv::Vec3b>(1, 0) == cv::Vec3b(255, 255, 255), "YUYV stride ignored");
    const auto jpeg = encoder.encodeBgr(bgr, 95);
    const auto decoded = cv::imdecode(jpeg, cv::IMREAD_COLOR);
    require(!decoded.empty() && decoded.cols == 2 && decoded.rows == 2, "invalid JPEG output");
    bool rejected = false;
    try { encoder.convertToBgr(yuyv.data(), 3, 2, 8); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "odd YUYV width accepted");
    rejected = false;
    try { encoder.encodeBgr(bgr, 101); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "invalid JPEG quality accepted");
}

/**
 * @brief  Check oldest-frame eviction, draining after closure, and consumer wakeup.
 */
void testQueue() {
    bool rejected = false;
    try { BoundedQueue<int> invalid(0); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "zero capacity accepted");
    BoundedQueue<int> queue(2);
    queue.push(1);
    queue.push(2);
    queue.push(3);
    queue.close();
    queue.push(4);
    int value = 0;
    require(queue.take(value) && value == 2, "oldest frame not discarded");
    require(queue.take(value) && value == 3, "closed queue must drain");
    require(!queue.take(value), "push after close accepted");
    BoundedQueue<int> waiting(1);
    /**
     * @brief  Wait for an empty queue to verify that close() wakes the consumer.
     */
    auto consumer = std::async(std::launch::async, [&waiting] {
        int item;
        return waiting.take(item);
    });
    waiting.close();
    require(consumer.wait_for(std::chrono::seconds(1)) == std::future_status::ready,
            "close did not wake consumer");
    require(!consumer.get(), "closed empty queue returned an item");
}

/**
 * @brief  Run video pipeline regression tests without camera or LCD hardware.
 */
int main() {
    try {
        testSurface(16);
        testSurface(24);
        testSurface(32);
        testSurface(16, true);
        testSurface(32, true);
        testPhotoButton();
        testEncoder();
        testQueue();
        std::cout << "Video pipeline tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
